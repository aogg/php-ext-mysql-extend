/*
  +----------------------------------------------------------------------+
  | mre - asynchronous log-script execution (mre.log_script_async)       |
  |                                                                      |
  | When mre.log_script_async / MRE_LOG_SCRIPT_ASYNC is configured, the  |
  | extension stops writing records itself: every record is rendered     |
  | into its final log line in the request process and then handed to a  |
  | forked child that includes the user script.  The request side only   |
  | pays fork() and returns - it never waits for the script.             |
  |                                                                      |
  | Fork protocol (per record), see docs/设计.md §9:                     |
  |   pid = fork();                                                      |
  |   pid > 0  -> request returns immediately (zero wait)                |
  |   pid < 0  -> one warning per process, the record is dropped         |
  |   pid == 0 -> fork once more, then _exit(0) immediately; the         |
  |               grandchild does the work and is reparented away, so    |
  |               the long-lived process is never a direct child of the  |
  |               fpm worker                                            |
  |                                                                      |
  | Child rules (non-negotiable):                                       |
  |   - only _exit() may end the child: no exit(), no zend_bailout()    |
  |     escaping into the copied request frames, no RSHUTDOWN, no        |
  |     atexit/destructor run, no fflush of the parent's stdio buffers;  |
  |   - no waitpid()/SIGCHLD changes here (fpm owns those semantics);    |
  |   - every Zend allocation the script makes dies with _exit() - the  |
  |     child never returns into the request that forked it;             |
  |   - a bailout raised by the script (exit()/fatal) is caught by the   |
  |     child-local zend_try frame below and turns into _exit(0).        |
  |                                                                      |
  | ZTS builds: fork() after the engine started threads leaves the       |
  | child without the other threads' allocator/opcache lock state, so    |
  | async is compiled out there (NTS is the fpm norm): warn once, drop.  |
  +----------------------------------------------------------------------+
*/

#include "php_mre.h"

#include <string.h>

/*
 * One-time warning for the whole process.  Emitted into the main log
 * (mre_log_write_main never routes, so it cannot recurse into any script).
 * The warning itself is not "a record": it is allowed to hit the disk even
 * in async mode, exactly once per process.
 */
static void mre_async_warn_once(const char *reason)
{
	char msg[MRE_PATH_MAX + 512];

	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	if (MRE_G(script_async_warned)) {
		return;
	}
	MRE_G(script_async_warned) = (zend_bool)1;

	snprintf(msg, sizeof(msg),
		"log_script_async '%s' cannot run (%s); the record is dropped "
		"(async mode never falls back to mre.log_script or mre.log_file; "
		"this warning is emitted once per process)",
		MRE_G(cfg).log_script_async, reason);
	mre_log_write_main("warn", msg);
}

#ifdef PHP_WIN32

/*
 * Windows has no fork().  mre_async.c is intentionally not part of the
 * config.w32 source list; this stub only keeps the file self-contained
 * (and the failure honest) if a Windows build ever includes it.
 */
void mre_async_write(const char *type, const char *dir,
                     const char *content, size_t content_len,
                     const mre_extra *extra)
{
	(void)type;
	(void)dir;
	(void)content;
	(void)content_len;
	(void)extra;

	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	if (!MRE_G(cfg).enable || MRE_G(cfg).log_script_async[0] == '\0') {
		return;
	}
	mre_async_warn_once("fork() is not available on Windows");
}

#else /* !PHP_WIN32 */

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

void mre_async_write(const char *type, const char *dir,
                     const char *content, size_t content_len,
                     const mre_extra *extra)
{
	const mre_config *cfg;
	mre_script_ctxinfo ci;
	char line[MRE_LOG_LINE_MAX];
	size_t line_len;
	struct stat st;
	pid_t pid, pid2;

	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	cfg = &MRE_G(cfg);

	if (!cfg->enable || cfg->log_script_async[0] == '\0') {
		return; /* not the async configuration: caller keeps old flow */
	}

#ifdef ZTS
	/*
	 * Threaded SAPIs (Apache worker mpm, embed): after the engine started
	 * worker threads, fork() only duplicates the calling thread, while the
	 * Zend MM arena, opcache locks and libc FILE locks may still be owned
	 * by the threads that do not exist in the child.  Running the PHP
	 * script there is not safe, so the feature is compiled out: warn once
	 * and drop (never block, never fall back).
	 */
	mre_async_warn_once("asynchronous execution is disabled in ZTS builds");
	return;
#else
	/*
	 * Cannot run user code any more: after RSHUTDOWN the executor is gone,
	 * before/between requests EG(active) is 0.  Treat it as "async is not
	 * possible": drop the record with the once-per-process warning (the
	 * warning itself goes to the main log, which needs no executor).
	 */
	if (!MRE_G(script_available) || !EG(active)) {
		mre_async_warn_once(!MRE_G(script_available)
			? "the request executor is already shut down"
			: "the request executor is not active");
		return;
	}
	/* Re-entrant record emitted from inside the script's own forked child
	 * (script_busy is set in the grandchild): drop silently - forking here
	 * would spawn children recursively. */
	if (MRE_G(script_busy)) {
		return;
	}

	/*
	 * Render the line HERE, in the request process: pid / request_id /
	 * time are request-level fields and must not be taken from the child.
	 * fork() gives the child a copy-on-write copy of this stack buffer, so
	 * no serialization or pipe is needed to hand it over.
	 */
	line_len = mre_log_format_line(line, sizeof(line), type, dir,
		content, content_len, extra);

	ci.type = type;
	ci.dir = dir;
	ci.content = content;
	ci.content_len = content_len;
	ci.extra = extra;
	ci.line = line;
	ci.line_len = line_len;
	ci.format = cfg->json_format ? "json" : "text";

	pid = fork();
	if (pid > 0) {
		return; /* parent: zero wait - the child owns the record now */
	}
	if (pid < 0) {
		mre_async_warn_once("fork() failed");
		return;
	}

	/*
	 * First child: fork once more and leave at once.  The long-lived
	 * process is the grandchild, which is reparented away from the worker
	 * instead of remaining attached to its lifetime.  pid2 < 0 cannot do
	 * any work, so it leaves quietly as well.
	 */
	pid2 = fork();
	if (pid2 != 0) {
		_exit(0);
	}

	/*
	 * Grandchild.  From here on the ONLY legal exit is _exit(): this
	 * process holds a copy of the request's engine state and must never
	 * unwind it (no RSHUTDOWN, no user destructors, no stdio flush).
	 */
	MRE_G(script_busy) = (zend_bool)1;

	if (!EG(active)) {
		_exit(0); /* executor already gone in the copied state: silent */
	}
	if (stat(cfg->log_script_async, &st) != 0) {
		_exit(0); /* missing script: silent (stat is per record) */
	}

	zend_try {
		/*
		 * quiet=1: a script failure is the script owner's business and
		 * must not write into the parent's main log from a child (that
		 * line would also carry the child's pid).  The return value is
		 * ignored by contract - in async mode the script writes the
		 * record itself, $context['line'] is already the final bytes.
		 */
		mre_script_execute(cfg->log_script_async, &ci, NULL, 0, (zend_bool)1);
	} zend_catch {
		/*
		 * exit()/die()/fatal error inside the script raised a bailout in
		 * the child.  There is no request to unwind here: end the child.
		 */
		_exit(0);
	} zend_end_try();

	_exit(0);
#endif /* ZTS */
}

#endif /* PHP_WIN32 */
