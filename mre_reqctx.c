/*
  +----------------------------------------------------------------------+
  | mre - request context snapshot rendered into every log line          |
  |                                                                      |
  | The log line used to carry only time/pid/request_id/type/dir/        |
  | content/extra.  This file adds the request context the OpenObserve   |
  | push record already had, with exactly the same source and rules      |
  | (scripts/mre_oo_log_script.php -> mre_oo_record()):                  |
  |                                                                      |
  |   http_host    $_SERVER['HTTP_HOST'], empty -> SERVER_NAME           |
  |   http_path    path part of $_SERVER['REQUEST_URI'] (no query)       |
  |   http_scheme  HTTPS non-empty and not "off" -> https; else          |
  |                REQUEST_SCHEME lowered; else http.  Emitted only for  |
  |                requests that look like HTTP (CLI stays clean)        |
  |   http_ip      $_SERVER['REMOTE_ADDR']                               |
  |   headers      every HTTP_* header (lower case, '_' -> '-') plus     |
  |                CONTENT_TYPE / CONTENT_LENGTH, exactly like the       |
  |                log-script $context['headers']                        |
  |                                                                      |
  | Hard rule (same as mre_log.c): this code must NEVER allocate Zend    |
  | memory.  It runs from mysqlnd / streams hooks and from the fork()ed  |
  | async grandchild, where only the line rendering itself is allowed.    |
  |                                                                      |
  | Design:                                                              |
  |   - mre_reqctx_rinit() (RINIT, mre.c) resets the snapshot.  On an    |
  |     HTTP request it also materializes $_SERVER just in time: that    |
  |     allocation is fine in RINIT (php_hash_environment()/auto globals |
  |     are the engine's own machinery), and it guarantees the snapshot  |
  |     exists even when the business code never touches $_SERVER (the   |
  |     async path renders the line in the request process, before any   |
  |     script ran).  CLI and other SAPIs (no request_method) are left   |
  |     untouched so auto_globals_jit keeps its benefit there.            |
  |   - mre_reqctx_ensure() (called by mre_log_format_line) re-reads the |
  |     $_SERVER hash table with zend_hash_str_find / ZEND_HASH_FOREACH   |
  |     only and memcpy()s string values into the fixed MRE_G buffers.   |
  |     It re-scans on EVERY record on purpose: PHP's string-keyed hash   |
  |     table reuses the slot freed by unset(), so an "identity" key of   |
  |     (HashTable*, nNumOfElements, nNumUsed) is NOT a sound staleness   |
  |     test - an in-place edit ($_SERVER['X'] = ... / unset(...)) can    |
  |     leave all three unchanged and keep a stale snapshot (caught by    |
  |     tests/017_line_request_ctx.phpt).  The walk is O(#$_SERVER) plain |
  |     reads into fixed stack buffers with no allocation, which is cheap |
  |     next to formatting the record itself, and it is always correct.   |
  |   - Values are JSON-escaped while the snapshot is taken, so the log  |
  |     line renderer only assembles already-escaped fragments.          |
  |   - headers are capped at MRE_REQCTX_HEADERS_MAX bytes: when the     |
  |     next pair does not fit, scanning stops, the object stays closed  |
  |     and reqctx_headers_truncated marks it (rendered as               |
  |     "headers":{...,"truncated":true}).                               |
  +----------------------------------------------------------------------+
*/

#include "php_mre.h"
#include "zend_API.h"   /* zend_hash_str_find, ZEND_HASH_FOREACH        */
#include "main/SAPI.h"  /* SG(request_info).request_method              */

#include <string.h>
#include <ctype.h>

/*
 * NOT const: PHP 7.4 declares zend_is_auto_global_str(char *, size_t),
 * 8.x takes a const char * - a char[] satisfies both signatures.
 */
static char mre_reqctx_server_key[] = "_SERVER";

/* ------------------------------------------------------------------ */
/* small allocation-free string helpers                                 */
/* ------------------------------------------------------------------ */

/* the trim set of PHP's trim() default: " \t\n\r\0\x0B" */
static zend_bool mre_reqctx_is_trim(unsigned char c)
{
	return (zend_bool)(c == ' ' || c == '\t' || c == '\n' || c == '\r'
		|| c == '\0' || c == 0x0B);
}

static void mre_reqctx_lower(char *s)
{
	for (; *s; s++) {
		*s = (char)tolower((unsigned char)*s);
	}
}

/* copy len bytes into dst (cap bytes incl. NUL), NUL-terminated */
static void mre_reqctx_copy(char *dst, size_t cap, const char *src, size_t len)
{
	if (cap == 0) {
		return;
	}
	if (len >= cap) {
		len = cap - 1;
	}
	memcpy(dst, src, len);
	dst[len] = '\0';
}

/* copy with the same trim() the PHP push record applies */
static void mre_reqctx_copy_trimmed(char *dst, size_t cap,
                                    const char *src, size_t len)
{
	size_t s = 0, e = len, n;

	if (cap == 0) {
		return;
	}
	while (s < e && mre_reqctx_is_trim((unsigned char)src[s])) {
		s++;
	}
	while (e > s && mre_reqctx_is_trim((unsigned char)src[e - 1])) {
		e--;
	}
	n = e - s;
	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(dst, src + s, n);
	dst[n] = '\0';
}

/*
 * Read one $_SERVER member as a string.  Only IS_STRING is accepted (the
 * same rule the header collector in mre_script.c follows; HTTP_* / host /
 * ip entries are always strings in every SAPI).  Read-only: nothing is
 * materialized or allocated here.
 */
static zend_bool mre_reqctx_server_str(HashTable *ht,
                                       const char *key, size_t klen,
                                       const char **out, size_t *out_len)
{
	zval *v;

	if (ht == NULL) {
		return (zend_bool)0;
	}
	v = zend_hash_str_find(ht, key, klen);
	if (v == NULL || Z_TYPE_P(v) != IS_STRING) {
		return (zend_bool)0;
	}
	*out = Z_STRVAL_P(v);
	*out_len = Z_STRLEN_P(v);
	return (zend_bool)1;
}

/* ------------------------------------------------------------------ */
/* snapshot reset / capture                                             */
/* ------------------------------------------------------------------ */
static void mre_reqctx_reset(void)
{
	MRE_G(reqctx_valid) = (zend_bool)0;
	MRE_G(reqctx_has_host) = (zend_bool)0;
	MRE_G(reqctx_host)[0] = '\0';
	MRE_G(reqctx_has_path) = (zend_bool)0;
	MRE_G(reqctx_path)[0] = '\0';
	MRE_G(reqctx_has_scheme) = (zend_bool)0;
	MRE_G(reqctx_scheme)[0] = '\0';
	MRE_G(reqctx_has_ip) = (zend_bool)0;
	MRE_G(reqctx_ip)[0] = '\0';
	MRE_G(reqctx_has_headers) = (zend_bool)0;
	MRE_G(reqctx_headers_truncated) = (zend_bool)0;
	MRE_G(reqctx_headers)[0] = '\0';
}

/* headers object body: '"name":"value",...' already JSON-escaped */
static void mre_reqctx_collect_headers(HashTable *ht)
{
	zend_string *hk;
	zval *hv;
	size_t used = 0;

	MRE_G(reqctx_headers)[0] = '\0';

	ZEND_HASH_FOREACH_STR_KEY_VAL(ht, hk, hv) {
		const char *name;
		size_t name_len, o, need;
		char ename[MRE_REQCTX_HEADER_NAME_MAX];
		char eval[MRE_REQCTX_HEADER_VAL_MAX];
		size_t ename_len, eval_len;

		if (hk == NULL || Z_TYPE_P(hv) != IS_STRING) {
			continue; /* same rule as $context['headers'] */
		}
		name = ZSTR_VAL(hk);
		name_len = ZSTR_LEN(hk);

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
		for (o = 0; o < name_len && o < sizeof(ename); o++) {
			char c = name[o];
			ename[o] = (c == '_') ? '-' : (char)tolower((unsigned char)c);
		}
		if (o >= sizeof(ename)) {
			o = sizeof(ename) - 1;
		}
		{
			char raw[sizeof(ename)];

			memcpy(raw, ename, o); /* name bytes before escaping */
			raw[o] = '\0';
			ename_len = mre_json_escape(raw, o, ename, sizeof(ename));
		}

		eval_len = mre_json_escape(Z_STRVAL_P(hv), Z_STRLEN_P(hv),
			eval, sizeof(eval));
		/* escaping never shortens a string, so a smaller escaped length
		 * means the fixed buffer cut this name/value: report the headers
		 * object as truncated (still valid JSON) */
		if (Z_STRLEN_P(hv) > eval_len || name_len > ename_len) {
			MRE_G(reqctx_headers_truncated) = (zend_bool)1;
		}
		/* '"' name '"' ':' '"' value '"' = name + value + 5 bytes, plus
		 * one ',' when this is not the first pair, plus the NUL */
		need = (used ? 1 : 0) + ename_len + eval_len + 5;
		if (used + need + 1 >= sizeof(MRE_G(reqctx_headers))) {
			MRE_G(reqctx_headers_truncated) = (zend_bool)1;
			break;
		}
		if (used) {
			MRE_G(reqctx_headers)[used++] = ',';
		}
		MRE_G(reqctx_headers)[used++] = '"';
		memcpy(MRE_G(reqctx_headers) + used, ename, ename_len);
		used += ename_len;
		MRE_G(reqctx_headers)[used++] = '"';
		MRE_G(reqctx_headers)[used++] = ':';
		MRE_G(reqctx_headers)[used++] = '"';
		memcpy(MRE_G(reqctx_headers) + used, eval, eval_len);
		used += eval_len;
		MRE_G(reqctx_headers)[used++] = '"';
		MRE_G(reqctx_headers)[used] = '\0';
	} ZEND_HASH_FOREACH_END();

	if (used == 0) {
		MRE_G(reqctx_has_headers) = (zend_bool)0;
		MRE_G(reqctx_headers_truncated) = (zend_bool)0;
	} else {
		MRE_G(reqctx_has_headers) = (zend_bool)1;
	}
}

/*
 * Path part of REQUEST_URI: drop query/fragment, and for absolute-form
 * request targets ("scheme://host/path") keep the path only.  Mirrors the
 * PHP push record, which runs parse_url(uri, PHP_URL_PATH) and omits the
 * field when that yields nothing.
 */
static zend_bool mre_reqctx_path_from_uri(const char *uri, size_t uri_len,
                                          char *out, size_t cap)
{
	size_t len = 0, i, start = 0, end;
	zend_bool absolute = (zend_bool)0;

	if (cap == 0) {
		return (zend_bool)0;
	}
	out[0] = '\0';
	if (uri == NULL || uri_len == 0) {
		return (zend_bool)0;
	}
	/* cut at '?' or '#' */
	for (i = 0; i < uri_len; i++) {
		if (uri[i] == '?' || uri[i] == '#') {
			break;
		}
	}
	len = i;

	/* absolute-form: skip "scheme://authority" */
	for (i = 0; i + 2 < len; i++) {
		if (uri[i] == ':' && uri[i + 1] == '/' && uri[i + 2] == '/') {
			absolute = (zend_bool)1;
			start = i + 3;
			break;
		}
	}
	if (absolute) {
		/* authority ends at the first '/' after it */
		for (i = start; i < len; i++) {
			if (uri[i] == '/') {
				break;
			}
		}
		if (i >= len) {
			return (zend_bool)0; /* no path component at all */
		}
		start = i;
	}

	end = len;
	if (start >= end) {
		return (zend_bool)0;
	}
	len = end - start;
	if (len >= cap) {
		len = cap - 1;
	}
	memcpy(out, uri + start, len);
	out[len] = '\0';
	return (zend_bool)1;
}

/* read $_SERVER once into the fixed buffers (allocation-free) */
static void mre_reqctx_scan(HashTable *ht)
{
	const char *v;
	size_t vlen;
	char uri[MRE_REQCTX_PATH_MAX];
	zend_bool has_uri = (zend_bool)0;

	mre_reqctx_reset();

	/* ---- http_host: HTTP_HOST first, empty -> SERVER_NAME ---- */
	{
		static const char *host_keys[] = { "HTTP_HOST", "SERVER_NAME" };
		static const size_t host_lens[] = { 9, 11 };
		size_t k;

		for (k = 0; k < sizeof(host_keys) / sizeof(host_keys[0]); k++) {
			if (!mre_reqctx_server_str(ht, host_keys[k], host_lens[k],
					&v, &vlen)) {
				continue;
			}
			mre_reqctx_copy_trimmed(MRE_G(reqctx_host),
				sizeof(MRE_G(reqctx_host)), v, vlen);
			if (MRE_G(reqctx_host)[0] != '\0') {
				MRE_G(reqctx_has_host) = (zend_bool)1;
				break;
			}
		}
	}

	/* ---- uri (REQUEST_URI) -> http_path ---- */
	if (mre_reqctx_server_str(ht, "REQUEST_URI", 11, &v, &vlen)) {
		mre_reqctx_copy_trimmed(uri, sizeof(uri), v, vlen);
		has_uri = (zend_bool)(uri[0] != '\0');
		if (has_uri) {
			MRE_G(reqctx_has_path) = mre_reqctx_path_from_uri(
				uri, strlen(uri), MRE_G(reqctx_path),
				sizeof(MRE_G(reqctx_path)));
		}
	}

	/* ---- http_scheme: only for requests that look like HTTP ---- */
	if (MRE_G(reqctx_has_host) || has_uri) {
		char https[16];

		https[0] = '\0';
		if (mre_reqctx_server_str(ht, "HTTPS", 5, &v, &vlen)) {
			mre_reqctx_copy_trimmed(https, sizeof(https), v, vlen);
			mre_reqctx_lower(https);
		}
		if (https[0] != '\0' && strcmp(https, "off") != 0) {
			mre_reqctx_copy(MRE_G(reqctx_scheme),
				sizeof(MRE_G(reqctx_scheme)), "https", 5);
			MRE_G(reqctx_has_scheme) = (zend_bool)1;
		} else if (mre_reqctx_server_str(ht, "REQUEST_SCHEME", 14,
				&v, &vlen)) {
			mre_reqctx_copy_trimmed(MRE_G(reqctx_scheme),
				sizeof(MRE_G(reqctx_scheme)), v, vlen);
			mre_reqctx_lower(MRE_G(reqctx_scheme));
			MRE_G(reqctx_has_scheme) =
				(zend_bool)(MRE_G(reqctx_scheme)[0] != '\0');
			if (!MRE_G(reqctx_has_scheme)) {
				mre_reqctx_copy(MRE_G(reqctx_scheme),
					sizeof(MRE_G(reqctx_scheme)), "http", 4);
				MRE_G(reqctx_has_scheme) = (zend_bool)1;
			}
		} else {
			mre_reqctx_copy(MRE_G(reqctx_scheme),
				sizeof(MRE_G(reqctx_scheme)), "http", 4);
			MRE_G(reqctx_has_scheme) = (zend_bool)1;
		}
	}

	/* ---- http_ip: REMOTE_ADDR (never X-Forwarded-For) ---- */
	if (mre_reqctx_server_str(ht, "REMOTE_ADDR", 11, &v, &vlen)) {
		mre_reqctx_copy_trimmed(MRE_G(reqctx_ip),
			sizeof(MRE_G(reqctx_ip)), v, vlen);
		MRE_G(reqctx_has_ip) = (zend_bool)(MRE_G(reqctx_ip)[0] != '\0');
	}

	/* ---- headers ---- */
	mre_reqctx_collect_headers(ht);

	MRE_G(reqctx_valid) = (zend_bool)(MRE_G(reqctx_has_host)
		|| MRE_G(reqctx_has_path) || MRE_G(reqctx_has_scheme)
		|| MRE_G(reqctx_has_ip) || MRE_G(reqctx_has_headers));
}

/* ------------------------------------------------------------------ */
/* public API                                                           */
/* ------------------------------------------------------------------ */
void mre_reqctx_rinit(void)
{
	mre_reqctx_reset();

	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	if (!MRE_G(cfg).enable) {
		return; /* logging off: leave $_SERVER lazy */
	}

	/*
	 * Only real HTTP-ish requests need the snapshot, and only they are
	 * materialized here: CLI (and other SAPIs without a request method)
	 * has no request context anyway, and auto_globals_jit keeps its
	 * benefit for them.
	 */
	if (SG(request_info).request_method == NULL
			|| SG(request_info).request_method[0] == '\0') {
		return;
	}
	/*
	 * Materialize $_SERVER just in time.  This allocates Zend memory,
	 * which is fine in RINIT (this is exactly what the first userland
	 * $_SERVER access would do); mre_log.c must not, which is why the
	 * capture lives here and the renderer only copies the snapshot.
	 */
	(void)zend_is_auto_global_str(mre_reqctx_server_key,
		sizeof(mre_reqctx_server_key) - 1);
	mre_reqctx_ensure();
}

void mre_reqctx_ensure(void)
{
	zval *server;
	HashTable *ht;

	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	if (!MRE_G(cfg).enable) {
		return;
	}

	/* Read-only lookup: never create/materialize $_SERVER in the log path. */
	server = zend_hash_str_find(&EG(symbol_table), "_SERVER",
		sizeof("_SERVER") - 1);
	if (server == NULL || Z_TYPE_P(server) != IS_ARRAY) {
		if (MRE_G(reqctx_valid) || MRE_G(reqctx_has_headers)) {
			mre_reqctx_reset(); /* $_SERVER disappeared: drop the snapshot */
		}
		return;
	}

	ht = Z_ARRVAL_P(server);
	/* always re-read: see the staleness note in the file header */
	mre_reqctx_scan(ht);
}
