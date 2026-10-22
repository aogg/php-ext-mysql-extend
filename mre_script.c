/*
  +----------------------------------------------------------------------+
  | mre - log path resolution through a userland PHP script (stage B)    |
  |                                                                      |
  | When mre.log_script / MRE_LOG_SCRIPT is configured, every record is  |
  | offered to that script before being written.  The script receives a  |
  | $context array (see README) and returns the target log file path;    |
  | returning null/false/a non-string degrades to the next resolver.     |
  |                                                                      |
  | Execution model (deliberate trade-off, see docs/设计.md §8):          |
  |   - the script runs through a fixed PHP-level wrapper compiled per   |
  |     record with zend_eval_stringl():                                 |
  |       try { $___mre_ret = include $___mre_path; }                    |
  |       catch (Throwable $___mre_e) { $___mre_ret = null;              |
  |       $___mre_warn = $___mre_e->getMessage(); }                      |
  |     a thrown exception (or an include ParseError) is therefore       |
  |     caught inside userland and never crosses the op_array boundary   |
  |     into C - this is the BUG-2 fix: the 7.4 crash happened on the    |
  |     engine's unwind path for exceptions that escape a top-level      |
  |     op_array executed from C, and no engine-internal exception       |
  |     surgery (zend_clear_exception in escaped-frame state, a          |
  |     getMessage() probe under a detached exception) can make that     |
  |     path safe; this design simply makes the path unreachable        |
  |   - the wrapper string is a constant that only references $___mre_*  |
  |     variables - the script path itself is injected through the       |
  |     global symbol table, never interpolated into the wrapper         |
  |   - the script is compiled by the engine's own include machinery,    |
  |     so opcache serves its shared-memory cache when enabled; without  |
  |     opcache each record pays one script compile (accepted cost,      |
  |     replaces the old per-request (mtime,size) op_array cache)        |
  |   - execution runs with EG(current_execute_data)=NULL so the wrapper |
  |     frame lands on the global symbol table; only $context/$___mre_*  |
  |     are injected there and removed right after the call - $_SERVER   |
  |     and the other superglobals are never written                     |
  |   - a pending exception of the interrupted code is detached (plain   |
  |     pointer save/restore, zend_execute() would silently skip         |
  |     execution otherwise) and restored afterwards; wrapper results    |
  |     are read back from $___mre_ret/$___mre_warn, and a failure is    |
  |     summarized into a one-time main-log warning so a broken script   |
  |     can never fail the request                                       |
  +----------------------------------------------------------------------+
*/

#include "php_mre.h"
#include "zend_API.h"
#include "zend_exceptions.h"
#include "zend_objects_API.h"

#include <string.h>
#include <sys/stat.h>

/*
 * Fixed wrapper source.  Only $___mre_* variables are referenced - the
 * script path arrives through the global symbol table ($___mre_path), so
 * a path can never break out of the wrapper by quoting tricks.  Declared
 * as non-const char[] on purpose: zend_eval_stringl() takes `char *str`
 * under PHP 7.4 and `const char *str` since 8.0, a static char array is
 * warning-free against both signatures.
 */
static char mre_wrapper_src[] =
	"try { $___mre_ret = include $___mre_path; } "
	"catch (Throwable $___mre_e) "
	"{ $___mre_ret = null; $___mre_warn = $___mre_e->getMessage(); }";

/* ------------------------------------------------------------------ */
/* request lifecycle                                                    */
/* ------------------------------------------------------------------ */
void mre_script_rinit(void)
{
	MRE_G(script_available) = (zend_bool)1;
	MRE_G(script_busy) = (zend_bool)0;
	/* script_warned deliberately not reset: the degradation warning is
	 * once per process, not once per request */
}

void mre_script_rshutdown(void)
{
	/* executing the log script after the executor shuts down is unsafe */
	MRE_G(script_available) = (zend_bool)0;
	MRE_G(script_busy) = (zend_bool)0;
}

/* ------------------------------------------------------------------ */
/* one-time degradation warning (into the main log, never routed)       */
/* ------------------------------------------------------------------ */
static void mre_script_warn(const char *path, const char *summary)
{
	char msg[MRE_PATH_MAX + 512];

	if (MRE_G(script_warned)) {
		return;
	}
	MRE_G(script_warned) = (zend_bool)1;
	snprintf(msg, sizeof(msg),
		"log_script '%s' failed (%s); falling back to rule/main log for records "
		"(this warning is emitted once per process)", path, summary);
	mre_log_write_main("warn", msg);
}

/* ------------------------------------------------------------------ */
/* $context building                                                    */
/* ------------------------------------------------------------------ */
/* $_SERVER HTTP_* + CONTENT_TYPE/CONTENT_LENGTH -> header => value     */
static void mre_ctx_headers(zval *out)
{
	zval *server;
	HashTable *ht;

	array_init(out);

	if (!zend_is_auto_global_str(ZEND_STRL("_SERVER"))) {
		return; /* no $_SERVER in this environment */
	}
	server = zend_hash_str_find(&EG(symbol_table), "_SERVER", 7);
	if (!server || Z_TYPE_P(server) != IS_ARRAY) {
		return; /* no $_SERVER: headers stay an empty array */
	}

	ht = Z_ARRVAL_P(server);
	{
		zend_string *k;
		zval *v;

		ZEND_HASH_FOREACH_STR_KEY_VAL(ht, k, v) {
			const char *name;
			size_t name_len;
			char hname[128];
			size_t o;

			if (!k || Z_TYPE_P(v) != IS_STRING) {
				continue;
			}
			name = ZSTR_VAL(k);
			name_len = ZSTR_LEN(k);

			if (name_len > 5 && memcmp(name, "HTTP_", 5) == 0) {
				name += 5;
				name_len -= 5;
			} else if (name_len == 12 && memcmp(name, "CONTENT_TYPE", 12) == 0) {
				/* keep as-is */
			} else if (name_len == 14 && memcmp(name, "CONTENT_LENGTH", 14) == 0) {
				/* keep as-is */
			} else {
				continue;
			}

			/* header-name style: lower case, '_' becomes '-' */
			for (o = 0; o < name_len && o < sizeof(hname) - 1; o++) {
				char c = name[o];
				hname[o] = (c == '_') ? '-' : (char)tolower((unsigned char)c);
			}
			hname[o] = '\0';
			add_assoc_stringl_ex(out, hname, o, Z_STRVAL_P(v), Z_STRLEN_P(v));
		} ZEND_HASH_FOREACH_END();
	}
}

/* `name` is char* (not const) because PHP 7.4 declares
 * zend_is_auto_global_str(char *, size_t); 8.x takes a const char *
 * and accepts a char * argument just as well */
static void mre_ctx_copy_superglobal(char *name, size_t name_len, zval *out)
{
	zval *gv;

	if (zend_is_auto_global_str(name, name_len)
			&& (gv = zend_hash_str_find(&EG(symbol_table), name, name_len))
			&& Z_TYPE_P(gv) == IS_ARRAY) {
		ZVAL_COPY(out, gv);
		return;
	}
	array_init(out);
}

/* session_id() without a build-time dependency on ext/session */
static void mre_ctx_session_id(zval *out)
{
	zend_function *fn;
	zval fname, rv;
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;

	fn = (zend_function *)zend_hash_str_find_ptr(EG(function_table),
		"session_id", sizeof("session_id") - 1);
	if (!fn) {
		ZVAL_STRING(out, "");
		return;
	}

	memset(&fci, 0, sizeof(fci));
	memset(&fcc, 0, sizeof(fcc));
	ZVAL_STRING(&fname, "session_id");
	fci.size = sizeof(fci);
	fci.function_name = fname;
	fci.retval = &rv;
	fci.param_count = 0;
#if PHP_VERSION_ID >= 80000
	fci.named_params = NULL;
#else
	fci.no_separation = 1;
#endif
	if (zend_call_function(&fci, &fcc) == SUCCESS && Z_TYPE(rv) == IS_STRING) {
		ZVAL_COPY(out, &rv);
	} else {
		ZVAL_STRING(out, "");
	}
	zval_ptr_dtor(&rv);
	zval_ptr_dtor(&fname);
}

/* $_SERVER['KEY'] ?? '' */
static void mre_ctx_server_var(const char *key, zval *out)
{
	zval *server, *v;

	ZVAL_STRING(out, "");
	if (!zend_is_auto_global_str(ZEND_STRL("_SERVER"))) {
		return; /* no $_SERVER in this environment */
	}
	server = zend_hash_str_find(&EG(symbol_table), "_SERVER", 7);
	if (!server || Z_TYPE_P(server) != IS_ARRAY) {
		return;
	}
	v = zend_hash_str_find(Z_ARRVAL_P(server), key, strlen(key));
	if (v && Z_TYPE_P(v) == IS_STRING) {
		zval_ptr_dtor(out);
		ZVAL_COPY(out, v);
	}
}

/* extra[]: row counts / error info for the script */
static void mre_ctx_extra(const mre_extra *e, zval *out)
{
	array_init(out);
	if (!e) {
		return;
	}
	if (e->has_affected) {
		add_assoc_long_ex(out, "affected_rows", sizeof("affected_rows") - 1,
			(zend_long)e->affected);
	}
	if (e->has_rows) {
		add_assoc_long_ex(out, "rows", sizeof("rows") - 1, (zend_long)e->rows);
	}
	if (e->has_fetched) {
		add_assoc_long_ex(out, "fetched_rows", sizeof("fetched_rows") - 1,
			(zend_long)e->fetched);
	}
	if (e->has_fields) {
		add_assoc_long_ex(out, "fields", sizeof("fields") - 1,
			(zend_long)e->fields);
	}
	if (e->has_stmt) {
		add_assoc_long_ex(out, "stmt_id", sizeof("stmt_id") - 1,
			(zend_long)e->stmt_id);
	}
	if (e->has_error) {
		add_assoc_long_ex(out, "error_code", sizeof("error_code") - 1,
			(zend_long)e->err_no);
		add_assoc_string_ex(out, "error", sizeof("error") - 1, e->err_msg);
		add_assoc_string_ex(out, "sqlstate", sizeof("sqlstate") - 1, e->sqlstate);
	}
	if (e->has_host) {
		add_assoc_string_ex(out, "host", sizeof("host") - 1, e->host);
	}
	if (e->has_port) {
		add_assoc_long_ex(out, "port", sizeof("port") - 1, e->port);
	}
	if (e->truncated) {
		add_assoc_bool_ex(out, "truncated", sizeof("truncated") - 1, 1);
	}
}

/* ------------------------------------------------------------------ */
/* best-effort helpers                                                  */
/* ------------------------------------------------------------------ */
static zend_bool mre_call_simple(const char *fn, size_t fn_len)
{
	zend_function *f;
	zval fname, rv;
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;

	f = (zend_function *)zend_hash_str_find_ptr(EG(function_table), fn, fn_len);
	if (!f) {
		return (zend_bool)0;
	}
	memset(&fci, 0, sizeof(fci));
	memset(&fcc, 0, sizeof(fcc));
	ZVAL_STRINGL(&fname, fn, fn_len);
	fci.size = sizeof(fci);
	fci.function_name = fname;
	fci.retval = &rv;
	fci.param_count = 0;
#if PHP_VERSION_ID >= 80000
	fci.named_params = NULL;
#else
	fci.no_separation = 1;
#endif
	if (zend_call_function(&fci, &fcc) != SUCCESS) {
		zval_ptr_dtor(&fname);
		return (zend_bool)0;
	}
	zval_ptr_dtor(&rv);
	zval_ptr_dtor(&fname);
	return (zend_bool)1;
}

/* ------------------------------------------------------------------ */
/* record context passed down to the builder                            */
/* ------------------------------------------------------------------ */
typedef struct {
	const char      *type;
	const char      *dir;
	const char      *content;
	size_t           content_len;
	const mre_extra *extra;
} mre_script_ctxinfo;

static void mre_ctx_build(zval *ctx, const mre_script_ctxinfo *ci)
{
	zval tmp;

	array_init(ctx);

	add_assoc_string_ex(ctx, "type", sizeof("type") - 1, ci->type ? ci->type : "");
	add_assoc_string_ex(ctx, "dir", sizeof("dir") - 1, ci->dir ? ci->dir : "");
	add_assoc_stringl_ex(ctx, "content", sizeof("content") - 1,
		ci->content ? ci->content : "", ci->content_len);

	mre_ctx_extra(ci->extra, &tmp);
	add_assoc_zval_ex(ctx, "extra", sizeof("extra") - 1, &tmp);

	mre_ctx_headers(&tmp);
	add_assoc_zval_ex(ctx, "headers", sizeof("headers") - 1, &tmp);

	mre_ctx_copy_superglobal("_POST", 5, &tmp);
	add_assoc_zval_ex(ctx, "post", sizeof("post") - 1, &tmp);

	mre_ctx_copy_superglobal("_COOKIE", 7, &tmp);
	add_assoc_zval_ex(ctx, "cookie", sizeof("cookie") - 1, &tmp);

	mre_ctx_copy_superglobal("_GET", 4, &tmp);
	add_assoc_zval_ex(ctx, "get", sizeof("get") - 1, &tmp);

	mre_ctx_session_id(&tmp);
	add_assoc_zval_ex(ctx, "session_id", sizeof("session_id") - 1, &tmp);

	mre_ctx_server_var("REQUEST_URI", &tmp);
	add_assoc_zval_ex(ctx, "uri", sizeof("uri") - 1, &tmp);

	mre_ctx_server_var("REQUEST_METHOD", &tmp);
	add_assoc_zval_ex(ctx, "method", sizeof("method") - 1, &tmp);
}

/* ------------------------------------------------------------------ */
/* execute the wrapper (which includes the script) and read results     */
/* ------------------------------------------------------------------ */
/* the $___mre_* / $context slots we own in the global symbol table */
static void mre_script_globals_cleanup(void)
{
	zend_hash_str_del(&EG(symbol_table), "context", 7);
	zend_hash_str_del(&EG(symbol_table), "___mre_path", 11);
	zend_hash_str_del(&EG(symbol_table), "___mre_ret", 10);
	zend_hash_str_del(&EG(symbol_table), "___mre_warn", 11);
}

static zend_bool mre_script_run(const char *path, const mre_script_ctxinfo *ci,
                                char *out, size_t cap)
{
	zend_execute_data *prev_ed;
	zend_object *pending;
	zval ctx, pathv, *ret, *warn;
	zend_bool routed = (zend_bool)0;

	/*
	 * The wrapper (a fixed one-liner, see mre_wrapper_src) does the whole
	 * job in userland: include the script and catch anything it throws -
	 * runtime exceptions and include ParseErrors alike are Throwable, so
	 * no exception ever escapes the wrapper's op_array into C.  The 7.4
	 * container crash (BUG-2) lived exactly on the escape path: an
	 * uncaught exception leaving a top-level op_array executed from C
	 * leaves EG(current_execute_data)=NULL, and clearing it there made
	 * 7.4 write EG(opline_before_exception) (a pointer into the dead
	 * script op_array) into the frame about to be resumed.  This design
	 * never enters that state, so the old engine-internal repair attempts
	 * are simply gone.
	 *
	 * Two pieces of engine state remain, both engine-sanctioned:
	 *   - EG(current_execute_data)=NULL around the call: zend_execute()
	 *     (zend_eval_stringl uses it internally) then pushes a
	 *     ZEND_CALL_TOP_CODE frame bound to &EG(symbol_table), so the
	 *     wrapper and the included script see $context/$___mre_* in the
	 *     globals exactly like {main} does and never touch the frame of
	 *     the interrupted code;
	 *   - EG(exception) pointer save/restore: a pending exception of the
	 *     interrupted code must not be visible to the wrapper (the
	 *     engine would skip execution or route it into the catch), and
	 *     must still be pending afterwards for whoever owned it.
	 */
	pending = EG(exception);
	EG(exception) = NULL;

	prev_ed = EG(current_execute_data);
	EG(current_execute_data) = NULL;

	/* expose $context and the script path (and only those) in globals */
	mre_ctx_build(&ctx, ci);
	ZVAL_STRING(&pathv, path);
	zend_hash_str_update(&EG(symbol_table), "context", 7, &ctx);
	zend_hash_str_update(&EG(symbol_table), "___mre_path", 11, &pathv);

	zend_try {
		/* compile+execute+destroy per record; the wrapper is a constant
		 * ~140-byte string (measured ~2us/record on 8.2), the script
		 * itself is compiled by include and cached by opcache when on */
		if (zend_eval_stringl(mre_wrapper_src, sizeof(mre_wrapper_src) - 1,
				NULL, "mre log_script wrapper") == FAILURE) {
			/* unreachable for our constant wrapper source; degrade with
			 * a summary instead of touching engine exception state */
			EG(current_execute_data) = prev_ed;
			EG(exception) = pending;
			mre_script_globals_cleanup();
			mre_script_warn(path, "wrapper compile failed");
			return (zend_bool)0;
		}
	} zend_catch {
		/* exit()/fatal inside the script: restore what we own, then
		 * let the bailout unwind the request as the engine expects */
		EG(current_execute_data) = prev_ed;
		EG(exception) = pending;
		mre_script_globals_cleanup();
		zend_bailout();
	} zend_end_try();

	/* the wrapper caught everything catchable (Throwable) - resume the
	 * interrupted frame */
	EG(current_execute_data) = prev_ed;

#if PHP_VERSION_ID >= 80000
	if (EG(exception) != NULL && EG(exception) != pending
			&& (zend_is_unwind_exit(EG(exception))
				|| zend_is_graceful_exit(EG(exception)))) {
		/*
		 * exit()/die() inside the script.  Since PHP 8.0 exit is not a
		 * bailout but a special exception the VM unwinds gracefully; it
		 * crosses the include/wrapper frames and surfaces here.  Leave it
		 * pending so the engine finishes the unwind the script asked for
		 * (the request ends with the requested exit status); degrade this
		 * record silently - whatever the script returned is irrelevant.
		 * The interrupted code's exception (if any) belonged to us while
		 * detached and is superseded by the exit - release it.
		 */
		if (pending != NULL) {
			zend_objects_store_del(pending);
		}
		mre_script_globals_cleanup();
		return (zend_bool)0;
	}
#endif

	/* nothing of ours can be pending here (Throwable was caught in the
	 * wrapper); keep whatever exception the interrupted code had pending */
	EG(exception) = pending;

	/* $___mre_warn: set by the wrapper's catch (message summary) */
	warn = zend_hash_str_find(&EG(symbol_table), "___mre_warn", 11);
	if (warn && Z_TYPE_P(warn) == IS_STRING && Z_STRLEN_P(warn) > 0) {
		mre_script_warn(path, Z_STRVAL_P(warn));
	}

	/* $___mre_ret: the include's return value (the requested log path) */
	ret = zend_hash_str_find(&EG(symbol_table), "___mre_ret", 10);
	if (ret && Z_TYPE_P(ret) == IS_STRING && Z_STRLEN_P(ret) > 0
			&& (size_t)Z_STRLEN_P(ret) < cap) {
		mre_copy_str(out, cap, Z_STRVAL_P(ret), Z_STRLEN_P(ret));
		routed = (zend_bool)1;
	}

	/* drop our globals again - do not leak them into userland */
	mre_script_globals_cleanup();

	return routed;
}

/* ------------------------------------------------------------------ */
/* public: try to resolve the target through the script                 */
/* ------------------------------------------------------------------ */
zend_bool mre_script_route(const char *type, const char *dir,
                           const char *content, size_t content_len,
                           const mre_extra *extra,
                           char *out, size_t cap)
{
	const mre_config *cfg;
	struct stat st;
	mre_script_ctxinfo ci;
	zend_bool ob_on, ok;

	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	cfg = &MRE_G(cfg);

	if (cfg->log_script[0] == '\0' || !cfg->enable) {
		return (zend_bool)0;
	}
	if (!MRE_G(script_available) || MRE_G(script_busy) || !EG(active)) {
		return (zend_bool)0;
	}

	/* stat once per record: a missing script warns once instead of
	 * emitting an include E_WARNING (captured by ob) on every record */
	if (stat(cfg->log_script, &st) != 0) {
		mre_script_warn(cfg->log_script, "stat() failed");
		return (zend_bool)0;
	}

	ci.type = type;
	ci.dir = dir;
	ci.content = content;
	ci.content_len = content_len;
	ci.extra = extra;

	MRE_G(script_busy) = (zend_bool)1;
	ob_on = mre_call_simple("ob_start", 8);
	ok = mre_script_run(cfg->log_script, &ci, out, cap);
	if (ob_on) {
		mre_call_simple("ob_end_clean", 12);
	}
	MRE_G(script_busy) = (zend_bool)0;

	return ok;
}
