/*
  +----------------------------------------------------------------------+
  | mre - unified log writer                                             |
  |                                                                      |
  | One record per line. JSON lines by default, plain text optional.     |
  | - append mode + flock  => safe for concurrent fpm workers            |
  | - handle cached per path => no open() per log line                   |
  | - special targets: "stdout", "stderr" and /proc/1/fd/1 (docker pid1) |
  | - on open failure: degrade to stderr and warn once                   |
  |                                                                      |
  | This file must not allocate Zend memory: it can be invoked from the  |
  | mysqlnd / streams hooks at any stage (even during shutdown).         |
  +----------------------------------------------------------------------+
*/

#include "php_mre.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

#ifndef PHP_WIN32
# include <sys/file.h> /* flock */
#endif

/* ------------------------------------------------------------------ */
/* overflow-safe append helper                                          */
/* ------------------------------------------------------------------ */

/* append printf-style text to buf; updates *len; never overflows */
static void mre_lbuf_appendf(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
	va_list ap;
	size_t left, written;

	if (*len >= cap) {
		return;
	}
	left = cap - *len;
	va_start(ap, fmt);
	written = (size_t)vsnprintf(buf + *len, left, fmt, ap);
	va_end(ap);
	if (written >= left) {
		*len = cap - 1; /* truncated */
	} else {
		*len += written;
	}
}

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

static zend_bool mre_is_special(const char *path, const char *what)
{
	return path != NULL && strcasecmp(path, what) == 0;
}

/* one-time warning about a degraded log target */
static void mre_log_warn_once(const char *path)
{
	if (MRE_G(log_open_warned)) {
		return;
	}
	MRE_G(log_open_warned) = (zend_bool)1;
	fprintf(stderr, "[mre] warning: cannot open log file '%s' (%s); "
		"falling back to stderr for this process\n",
		path ? path : "(null)", strerror(errno));
}

/* resolve (and cache) the FILE* for the configured log target */
static FILE *mre_log_target(const char *path)
{
	FILE *fp;

	if (!path || !*path) {
		return NULL; /* logging to file disabled */
	}

	/* stdout / stderr specials are never cached, they are always valid */
	if (mre_is_special(path, "stdout")) {
		return stdout;
	}
	if (mre_is_special(path, "stderr")) {
		return stderr;
	}

	if (MRE_G(log_fp) && MRE_G(log_fp_path)
			&& strcmp(MRE_G(log_fp_path), path) == 0) {
		return MRE_G(log_fp);
	}

	/* target changed: drop the old handle first */
	if (MRE_G(log_fp)) {
		fclose(MRE_G(log_fp));
		MRE_G(log_fp) = NULL;
	}
	if (MRE_G(log_fp_path)) {
		pefree(MRE_G(log_fp_path), 1);
		MRE_G(log_fp_path) = NULL;
	}

	mre_makedirs(path);
	fp = fopen(path, "a");
	if (!fp) {
		mre_log_warn_once(path);
		return NULL;
	}

	MRE_G(log_fp) = fp;
	/* persistent memory: the handle cache spans requests and is freed in
	 * MSHUTDOWN/GSHUTDOWN (request shutdown would reclaim request memory) */
	MRE_G(log_fp_path) = pestrdup(path, 1);
	return fp;
}

static void mre_log_emit(FILE *fp, const char *line, size_t len)
{
	if (!fp || len == 0) {
		return;
	}
#ifndef PHP_WIN32
	/* ignore flock failures: e.g. pipes do not always support it */
	flock(fileno(fp), LOCK_EX);
#endif
	fwrite(line, 1, len, fp);
	fputc('\n', fp);
	fflush(fp);
#ifndef PHP_WIN32
	flock(fileno(fp), LOCK_UN);
#endif
}

/* ------------------------------------------------------------------ */
/* "extra" renderers                                                    */
/* ------------------------------------------------------------------ */

static void mre_log_extra_json(char *buf, size_t cap, size_t *len, const mre_extra *e)
{
	char tmp[128];
	zend_bool first = (zend_bool)1;

#define MRE_PUT_SEP() do { if (!first) { mre_lbuf_appendf(buf, cap, len, ","); } first = 0; } while (0)

	if (!e) {
		mre_lbuf_appendf(buf, cap, len, "\"extra\":null");
		return;
	}

	mre_lbuf_appendf(buf, cap, len, "\"extra\":{");

	if (e->has_affected) {
		MRE_PUT_SEP();
		mre_lbuf_appendf(buf, cap, len, "\"affected_rows\":%llu", (unsigned long long)e->affected);
	}
	if (e->has_rows) {
		MRE_PUT_SEP();
		mre_lbuf_appendf(buf, cap, len, "\"rows\":%llu", (unsigned long long)e->rows);
	}
	if (e->has_fetched) {
		MRE_PUT_SEP();
		mre_lbuf_appendf(buf, cap, len, "\"fetched_rows\":%llu", (unsigned long long)e->fetched);
	}
	if (e->has_fields) {
		MRE_PUT_SEP();
		mre_lbuf_appendf(buf, cap, len, "\"fields\":%llu", (unsigned long long)e->fields);
	}
	if (e->has_stmt) {
		MRE_PUT_SEP();
		mre_lbuf_appendf(buf, cap, len, "\"stmt_id\":%llu", (unsigned long long)e->stmt_id);
	}
	if (e->has_error) {
		MRE_PUT_SEP();
		mre_json_escape(e->err_msg, strlen(e->err_msg), tmp, sizeof(tmp));
		mre_lbuf_appendf(buf, cap, len, "\"error_code\":%llu,\"error\":\"%s\",\"sqlstate\":\"%s\"",
			(unsigned long long)e->err_no, tmp, e->sqlstate);
	}
	if (e->has_host) {
		MRE_PUT_SEP();
		mre_json_escape(e->host, strlen(e->host), tmp, sizeof(tmp));
		mre_lbuf_appendf(buf, cap, len, "\"host\":\"%s\"", tmp);
	}
	if (e->has_port) {
		MRE_PUT_SEP();
		mre_lbuf_appendf(buf, cap, len, "\"port\":%d", e->port);
	}
	if (e->truncated) {
		MRE_PUT_SEP();
		mre_lbuf_appendf(buf, cap, len, "\"truncated\":true");
	}

	mre_lbuf_appendf(buf, cap, len, "}");

#undef MRE_PUT_SEP
}

/* text format renderer: k=v pairs appended after content */
static void mre_log_extra_text(char *buf, size_t cap, size_t *len, const mre_extra *e)
{
	if (!e) {
		return;
	}
	if (e->has_affected) {
		mre_lbuf_appendf(buf, cap, len, " affected_rows=%llu", (unsigned long long)e->affected);
	}
	if (e->has_rows) {
		mre_lbuf_appendf(buf, cap, len, " rows=%llu", (unsigned long long)e->rows);
	}
	if (e->has_fetched) {
		mre_lbuf_appendf(buf, cap, len, " fetched_rows=%llu", (unsigned long long)e->fetched);
	}
	if (e->has_fields) {
		mre_lbuf_appendf(buf, cap, len, " fields=%llu", (unsigned long long)e->fields);
	}
	if (e->has_stmt) {
		mre_lbuf_appendf(buf, cap, len, " stmt_id=%llu", (unsigned long long)e->stmt_id);
	}
	if (e->has_error) {
		mre_lbuf_appendf(buf, cap, len, " error_code=%llu error=\"%s\" sqlstate=%s",
			(unsigned long long)e->err_no, e->err_msg, e->sqlstate);
	}
	if (e->has_host) {
		mre_lbuf_appendf(buf, cap, len, " host=%s", e->host);
	}
	if (e->has_port) {
		mre_lbuf_appendf(buf, cap, len, " port=%d", e->port);
	}
	if (e->truncated) {
		mre_lbuf_appendf(buf, cap, len, " truncated=1");
	}
}

/* ------------------------------------------------------------------ */
/* public API                                                           */
/* ------------------------------------------------------------------ */

void mre_extra_init(mre_extra *e)
{
	memset(e, 0, sizeof(*e));
}

/* core emitter: format one record and append it to `target` */
static void mre_log_emit_record(const char *target,
                                const char *type, const char *dir,
                                const char *content, size_t content_len,
                                const mre_extra *extra)
{
	char line[MRE_LOG_LINE_MAX];
	char esc[MRE_JSON_VAL_MAX];
	char tsbuf[40];
	size_t len = 0;
	FILE *fp;

	fp = mre_log_target(target);
	if (!fp) {
		return; /* open failure was already reported once */
	}

	mre_iso8601_now(tsbuf, sizeof(tsbuf));

	if (MRE_G(cfg).json_format) {
		char typebuf[16], dirbuf[16];

		mre_json_escape(type, type ? strlen(type) : 0, typebuf, sizeof(typebuf));
		mre_json_escape(dir, dir ? strlen(dir) : 0, dirbuf, sizeof(dirbuf));
		mre_json_escape(content, content_len, esc, sizeof(esc));

		/* request_id is generated internally ("[0-9-]" only): no escape */
		mre_lbuf_appendf(line, sizeof(line), &len,
			"{\"time\":\"%s\",\"pid\":%d,\"request_id\":\"%s\",\"type\":\"%s\",\"dir\":\"%s\",\"content\":\"%s\"",
			tsbuf, (int)getpid(), mre_request_id_get(), typebuf, dirbuf, esc);
		mre_lbuf_appendf(line, sizeof(line), &len, ",");
		mre_log_extra_json(line, sizeof(line), &len, extra);
		mre_lbuf_appendf(line, sizeof(line), &len, "}");
	} else {
		mre_json_escape(content, content_len, esc, sizeof(esc));
		mre_lbuf_appendf(line, sizeof(line), &len,
			"%s pid=%d request_id=%s type=%s dir=%s content=\"%s\"",
			tsbuf, (int)getpid(), mre_request_id_get(),
			type ? type : "", dir ? dir : "", esc);
		mre_log_extra_text(line, sizeof(line), &len, extra);
	}

	if (len >= sizeof(line)) {
		len = sizeof(line) - 1;
	}
	line[len] = '\0';

	mre_log_emit(fp, line, len);
}

void mre_log_write(const char *type, const char *dir,
                   const char *content, size_t content_len,
                   const mre_extra *extra)
{
	const mre_config *cfg;
	char target[MRE_PATH_MAX];

	/* config might not be resolved yet (e.g. MINIT time) - resolve lazily */
	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	cfg = &MRE_G(cfg);

	if (!cfg->enable) {
		return;
	}
#ifdef MRE_DBG_NOLOG
	return;
#endif

	/* stage B: pick the destination for this record.  Priority:
	 * log_script return value > yaml rule > mre.log_file */
	target[0] = '\0';
	if (!mre_route_resolve(type, dir, content, content_len, extra,
			target, sizeof(target))) {
		mre_copy_str(target, sizeof(target),
			cfg->log_file, strlen(cfg->log_file));
	}

	if (target[0] == '\0') {
		/* no target configured: degrade to stderr, warn once (stage A) */
		if (!MRE_G(log_open_warned)) {
			MRE_G(log_open_warned) = (zend_bool)1;
			fprintf(stderr, "[mre] warning: mre.enable=1 but mre.log_file is empty; "
				"logs go to stderr until a log file is configured\n");
		}
		mre_copy_str(target, sizeof(target), "stderr", 6);
	}

	mre_log_emit_record(target, type, dir, content, content_len, extra);
}

/* un-routed record straight into the main log (used for warnings:
 * must never recurse back into route resolution) */
void mre_log_write_main(const char *type, const char *content)
{
	const mre_config *cfg;
	const char *target;

	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	cfg = &MRE_G(cfg);

	if (cfg->log_file[0] != '\0') {
		target = cfg->log_file;
	} else {
		target = "stderr"; /* no main log configured */
	}
	mre_log_emit_record(target, type, "", content, strlen(content), NULL);
}

void mre_log_close(void)
{
	if (MRE_G(log_fp)) {
		fclose(MRE_G(log_fp));
		MRE_G(log_fp) = NULL;
	}
	if (MRE_G(log_fp_path)) {
		pefree(MRE_G(log_fp_path), 1);
		MRE_G(log_fp_path) = NULL;
	}
}
