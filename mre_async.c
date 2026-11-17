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
  |   - SIGCHLD semantics stay untouched; waitpid(-1, ...) is never used |
  |     (it would steal proc_open()/exec() children): the parent reaps   |
  |     only the pids it recorded, with WNOHANG, while handling later    |
  |     records;                                                         |
  |   - a failed script execution in the grandchild prints one stderr    |
  |     line per worker per 60 s window (atomic O_EXCL marker claim)     |
  |     instead of writing the main log;                                 |
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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* first-layer child reaping (bounds the zombie population)             */
/* ------------------------------------------------------------------ */
/*
 * Every record forks a first-layer child that leaves at once.  Nobody
 * waits for it, so each exit stays a zombie under the fpm worker forever
 * (the "short zombie" once assumed here does not exist - a zombie lives
 * until somebody wait()s or the worker dies).  At high rates the process
 * table fills, fork() starts failing with EAGAIN and every record is
 * dropped.
 *
 * SIGCHLD handling is not ours to change and the request must not block,
 * so the only safe design is opportunistic reaping: remember OUR OWN
 * first-layer pids and try waitpid(<that pid>, WNOHANG) on later records.
 * waitpid(-1, ...) is forbidden - it would reap the business code's
 * proc_open()/exec() children.
 *
 * In the steady state the array holds 0..1 live entries, so a record pays
 * at most one extra waitpid() syscall.
 */
#define MRE_ASYNC_CHILD_SLOTS 32

static pid_t mre_async_child_pids[MRE_ASYNC_CHILD_SLOTS];
static int   mre_async_child_count;

/* request process only - never executed by a forked child */
static void mre_async_reap_children(void)
{
	int i = 0;

	while (i < mre_async_child_count) {
		pid_t r = waitpid(mre_async_child_pids[i], NULL, WNOHANG);

		if (r == mre_async_child_pids[i] || (r < 0 && errno == ECHILD)) {
			/*
			 * Reaped now, or no longer our child (already reaped
			 * elsewhere): drop the slot and compress the array by
			 * moving the last entry into it, then recheck that entry.
			 */
			mre_async_child_count--;
			mre_async_child_pids[i] =
				mre_async_child_pids[mre_async_child_count];
			continue;
		}
		/* 0: still running; transient error: keep for the next record */
		i++;
	}
}

static void mre_async_track_child(pid_t pid)
{
	if (mre_async_child_count < MRE_ASYNC_CHILD_SLOTS) {
		mre_async_child_pids[mre_async_child_count++] = pid;
		return;
	}
	/*
	 * Array full: more than 32 first-layer children forked since the last
	 * opportunistic reap (a transient burst).  Overwrite the OLDEST entry,
	 * i.e. that one may keep its zombie: the leak is bounded by one per
	 * burst and needs extreme pressure to happen at all - accepted, a
	 * dropped slot must never turn into an unbounded array or a blocking
	 * wait.
	 */
	memmove(&mre_async_child_pids[0], &mre_async_child_pids[1],
		sizeof(pid_t) * (MRE_ASYNC_CHILD_SLOTS - 1));
	mre_async_child_pids[MRE_ASYNC_CHILD_SLOTS - 1] = pid;
}

/* ------------------------------------------------------------------ */
/* failure visibility: one throttled stderr line per worker             */
/* ------------------------------------------------------------------ */
/*
 * The grandchild must not write the main log (the line would carry the
 * child's pid and could recurse into the log script), so a failed script
 * execution goes to stderr - fpm puts it into its error log.  One marker
 * file per worker and per time bucket caps that at one line per 60 s:
 *   /tmp/.mre-async-warn-<worker pid>-<bucket>
 *   bucket = (long)(time(NULL) / MRE_ASYNC_WARN_INTERVAL)
 *
 * The claim is the creation itself: open(O_CREAT | O_EXCL) succeeds for
 * exactly one grandchild per worker and window.  The old code probed
 * stat()/mtime, printed, then refreshed the mtime - three non-atomic steps
 * that let two same-instant failures both pass the probe and print twice.
 *
 * Growth is bounded: a marker is created only on failure - at most one
 * empty file per worker per 60 s - and the system's /tmp cleanup removes
 * them; nothing is written into the file.
 *
 * The pid is the WORKER pid captured before fork(): in the grandchild
 * getppid() is the already-exiting first-layer child (or 1), never the
 * worker, so it cannot provide per-worker throttling.
 */
#define MRE_ASYNC_WARN_INTERVAL 60

static void mre_async_report_failure(pid_t worker_pid, const char *path,
                                     const char *summary)
{
	char marker[96];
	char buf[MRE_PATH_MAX + 256];
	time_t now;
	size_t len;
	int n, fd = -1;

	/*
	 * Atomic claim: only the winner of O_EXCL prints.  EEXIST means
	 * another grandchild of this worker already reported inside the
	 * current window, so stay silent.  Any other open() failure (read-only
	 * /tmp, no inodes, ...) must not silence the report - fall through and
	 * print anyway ("rather over-report than go silent"), no retry.
	 */
	now = time(NULL);
	if (now != (time_t)-1) {
		snprintf(marker, sizeof(marker),
			"/tmp/.mre-async-warn-%ld-%ld",
			(long)worker_pid, (long)(now / MRE_ASYNC_WARN_INTERVAL));
		fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd < 0 && errno == EEXIST) {
			return; /* this window was already reported: one line */
		}
	}
	/* now == (time_t)-1: no clock to bucket by, always report */

	n = snprintf(buf, sizeof(buf),
		"[mre] async log_script '%s' failed (%s); record dropped\n",
		path ? path : "", summary ? summary : "unknown failure");
	if (n > 0) {
		len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
		/* plain write(2): no stdio buffering, no FILE locks, no retry -
		 * the record is already lost, a dead stderr must not hang us */
		(void)write(STDERR_FILENO, buf, len);
	}

	if (fd >= 0) {
		close(fd); /* empty marker file: existence == "reported" */
	}
}

void mre_async_write(const char *type, const char *dir,
                     const char *content, size_t content_len,
                     const mre_extra *extra)
{
	const mre_config *cfg;
	mre_script_ctxinfo ci;
	char line[MRE_LOG_LINE_MAX];
	size_t line_len;
	struct stat st;
	pid_t pid, pid2, worker_pid;

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

	/*
	 * Opportunistic reaping before forking the next child: at most one
	 * waitpid() in the steady state, never blocking, and only for pids
	 * this extension recorded itself.
	 */
	mre_async_reap_children();

	/*
	 * Capture the request process pid once, BEFORE fork(): the grandchild
	 * cannot recover it (getpid() is the grandchild, getppid() is the
	 * already-exiting first-layer child).  One value serves both the
	 * per-worker failure throttle marker and $context['pid'], which must
	 * match the pid inside the rendered log line.
	 */
	worker_pid = getpid();
	ci.pid = (zend_long)worker_pid;

	pid = fork();
	if (pid > 0) {
		/* parent: remember our own child so a later record can reap it */
		mre_async_track_child(pid);
		return; /* zero wait - the child owns the record now */
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
		/* missing script: reported (throttled) instead of vanishing */
		mre_async_report_failure(worker_pid, cfg->log_script_async,
			"stat() failed");
		_exit(0);
	}

	zend_try {
		/*
		 * quiet=1: a script failure must not write into the parent's
		 * main log from a child (that line would also carry the child's
		 * pid); the failure summary travels back through `warn` and is
		 * printed to stderr instead (throttled per worker).  The return
		 * value is ignored by contract - in async mode the script
		 * writes the record itself, $context['line'] is already final.
		 */
		char warn[MRE_SCRIPT_WARN_MAX];

		if (mre_script_execute(cfg->log_script_async, &ci, NULL, 0,
				(zend_bool)1, warn, sizeof(warn)) == (zend_bool)0
				&& warn[0] != '\0') {
			mre_async_report_failure(worker_pid,
				cfg->log_script_async, warn);
		}
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
