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
  |   - the script is compiled through the zend_compile_file() global    |
  |     pointer, so opcache keeps serving its cached op_array            |
  |   - compiled op_arrays are cached per request keyed by (mtime,size); |
  |     the file is parsed once per request instead of once per record   |
  |   - the op_array is deliberately NOT destroyed by us: opcache may    |
  |     hand out its shared-memory copy, which must never be destroyed;  |
  |     a non-opcache build keeps one request-local op_array per request |
  |     which the memory manager reclaims at request end (bounded)       |
  |   - execution runs with EG(current_execute_data)=NULL so the script  |
  |     gets its own frame on the global symbol table; only $context is  |
  |     injected there and removed right after the call - $_SERVER and   |
  |     the other superglobals are never written                         |
  |   - exceptions / parse errors are caught (EG(exception)), summarized |
  |     into a one-time main-log warning and swallowed so a broken       |
  |     script can never fail the request; a pending exception of the    |
  |     interrupted code is restored afterwards                          |
  +----------------------------------------------------------------------+
*/

#include "php_mre.h"
#include "zend_API.h"
#include "zend_exceptions.h"
#include "zend_interfaces.h"

#include <string.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* request lifecycle                                                    */
/* ------------------------------------------------------------------ */
void mre_script_rinit(void)
{
	MRE_G(script_available) = (zend_bool)1;
	MRE_G(script_oparray) = NULL;
	MRE_G(script_mtime) = 0;
	MRE_G(script_size) = 0;
	MRE_G(script_busy) = (zend_bool)0;
}

void mre_script_rshutdown(void)
{
	/* the compiled op_array (if any) is request memory: just forget it */
	MRE_G(script_available) = (zend_bool)0;
	MRE_G(script_oparray) = NULL;
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

static void mre_ctx_copy_superglobal(const char *name, size_t name_len, zval *out)
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

/* current exception message summary ("" when none) */
static void mre_exception_summary(char *buf, size_t cap)
{
	zval rv;
	zend_object *ex = EG(exception);

	buf[0] = '\0';
	if (!ex) {
		mre_copy_str(buf, cap, "unknown error", 13);
		return;
	}
	/* zend_call_function refuses to run while an exception is pending:
	 * detach it around the getMessage() call */
	EG(exception) = NULL;
	ZVAL_UNDEF(&rv);
	if (zend_call_method_with_0_params(ex, ex->ce,
			NULL, "getmessage", &rv) && Z_TYPE(rv) == IS_STRING) {
		mre_copy_str(buf, cap, Z_STRVAL(rv), Z_STRLEN(rv));
	} else {
		mre_copy_str(buf, cap, "exception", 9);
	}
	zval_ptr_dtor(&rv);
	EG(exception) = ex;
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
/* compile (cached per request by mtime+size) and execute               */
/* ------------------------------------------------------------------ */
static zend_op_array *mre_script_compile(const char *path, time_t mtime,
                                         long size, char *summary,
                                         size_t summary_cap)
{
	zend_op_array *op;
	zend_file_handle fh;

	if (MRE_G(script_oparray)
			&& MRE_G(script_mtime) == mtime
			&& MRE_G(script_size) == size) {
		return (zend_op_array *)MRE_G(script_oparray); /* up to date */
	}

#if PHP_VERSION_ID >= 80000
	zend_stream_init_filename(&fh, path);
#else
	memset(&fh, 0, sizeof(fh));
	fh.type = ZEND_HANDLE_FILENAME;
	fh.filename = (char *)path;
	fh.opened_path = NULL;
	fh.free_filename = 0;
#endif

	op = zend_compile_file(&fh, ZEND_INCLUDE);

	if (!op) {
		if (EG(exception)) {
			mre_exception_summary(summary, summary_cap);
			zend_clear_exception();
		} else {
			mre_copy_str(summary, summary_cap, "compilation failed", 19);
		}
		return NULL;
	}

	MRE_G(script_oparray) = op;
	MRE_G(script_mtime) = mtime;
	MRE_G(script_size) = size;
	return op;
}

static zend_bool mre_script_run(const char *path, time_t mtime, long size,
                                const mre_script_ctxinfo *ci, zval *retval)
{
	zend_op_array *op;
	zend_execute_data *prev_ed;
	zend_object *pending;
	zval ctx;
	zend_bool ok = (zend_bool)1;
	char summary[256];

	op = mre_script_compile(path, mtime, size, summary, sizeof(summary));
	if (!op) {
		mre_script_warn(path, summary);
		return (zend_bool)0;
	}

	/* expose $context (and only it) in the global symbol table */
	mre_ctx_build(&ctx, ci);
	zend_hash_str_update(&EG(symbol_table), "context", 7, &ctx);

	pending = EG(exception);
	prev_ed = EG(current_execute_data);
	EG(current_execute_data) = NULL;

	zend_execute(op, retval);

	EG(current_execute_data) = prev_ed;

	if (EG(exception) && EG(exception) != pending) {
		mre_exception_summary(summary, sizeof(summary));
		zend_clear_exception();
		ok = (zend_bool)0;
	}
	/* keep whatever exception the interrupted code had pending */
	EG(exception) = pending;

	/* drop $context again - do not leak it into userland */
	zend_hash_str_del(&EG(symbol_table), "context", 7);

	if (!ok) {
		mre_script_warn(path, summary);
	}
	return ok;
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
	zval retval;
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

	/* stat once per record; the compile cache validates (mtime,size) */
	if (stat(cfg->log_script, &st) != 0) {
		mre_script_warn(cfg->log_script, "stat() failed");
		return (zend_bool)0;
	}

	ci.type = type;
	ci.dir = dir;
	ci.content = content;
	ci.content_len = content_len;
	ci.extra = extra;

	ZVAL_UNDEF(&retval);

	MRE_G(script_busy) = (zend_bool)1;
	ob_on = mre_call_simple("ob_start", 8);
	ok = mre_script_run(cfg->log_script, (time_t)st.st_mtime,
		(long)st.st_size, &ci, &retval);
	if (ob_on) {
		mre_call_simple("ob_end_clean", 12);
	}
	MRE_G(script_busy) = (zend_bool)0;

	if (ok && Z_TYPE(retval) == IS_STRING && Z_STRLEN(retval) > 0
			&& (size_t)Z_STRLEN(retval) < cap) {
		mre_copy_str(out, cap, Z_STRVAL(retval), Z_STRLEN(retval));
		zval_ptr_dtor(&retval);
		return (zend_bool)1;
	}

	zval_ptr_dtor(&retval);
	return (zend_bool)0;
}
