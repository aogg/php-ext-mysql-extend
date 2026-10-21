/*
  +----------------------------------------------------------------------+
  | mre - small utilities (no Zend API dependencies where possible)      |
  +----------------------------------------------------------------------+
*/

#include "php_mre.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* mre_copy_str: bounded copy with NUL termination                      */
/* ------------------------------------------------------------------ */
void mre_copy_str(char *dst, size_t dst_cap, const char *src, size_t src_len)
{
	if (!dst || dst_cap == 0) {
		return;
	}
	if (!src) {
		src_len = 0;
	}
	if (src_len >= dst_cap) {
		src_len = dst_cap - 1;
	}
	if (src_len > 0) {
		memcpy(dst, src, src_len);
	}
	dst[src_len] = '\0';
}

/* ------------------------------------------------------------------ */
/* mre_iso8601_now: local time ISO8601, e.g. 2026-10-21T10:30:00+08:00  */
/* libc only (safe from hooks during shutdown), no ext/date dependency  */
/* ------------------------------------------------------------------ */
void mre_iso8601_now(char *buf, size_t cap)
{
	time_t now = time(NULL);
	struct tm tmv;
	long gmtoff = 0;
	char sign = '+';

	if (cap < 32) {
		if (cap > 0) {
			buf[0] = '\0';
		}
		return;
	}

#ifdef PHP_WIN32
	{
		time_t local = now;
		struct tm ltm;
		localtime_s(&ltm, &local);
		tmv = ltm;
	}
	/* Windows struct tm has no tm_gmtoff: fall through to the UTC style */
	gmtoff = 0;
#else
	localtime_r(&now, &tmv);
	gmtoff = (long)tmv.tm_gmtoff;
#endif

	if (gmtoff < 0) {
		sign = '-';
		gmtoff = -gmtoff;
	}
	if (gmtoff == 0) {
		/* UTC: render with Z (still ISO8601) */
		snprintf(buf, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ",
			tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
			tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
	} else {
		snprintf(buf, cap, "%04d-%02d-%02dT%02d:%02d:%02d%c%02ld:%02ld",
			tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
			tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
			sign, gmtoff / 3600, (gmtoff % 3600) / 60);
	}
}

/* ------------------------------------------------------------------ */
/* mre_makedirs: create all missing parent directories (mkdir -p)       */
/* ------------------------------------------------------------------ */
void mre_makedirs(const char *path)
{
	char buf[MRE_PATH_MAX];
	size_t i, len;

	if (!path || !*path) {
		return;
	}
	len = strlen(path);
	if (len >= sizeof(buf)) {
		return;
	}
	memcpy(buf, path, len + 1);

	/* only the directory part */
	for (i = len; i > 0; i--) {
		if (buf[i - 1] == '/') {
			break;
		}
	}
	if (i == 0) {
		return; /* relative file without directory component */
	}
	buf[i - 1] = '\0';
	if (buf[0] == '\0') {
		return; /* root dir */
	}

	/* walk the path creating each component */
	for (i = 1; buf[i] != '\0'; i++) {
		if (buf[i] == '/') {
			buf[i] = '\0';
#ifdef PHP_WIN32
			_mkdir(buf);
#else
			mkdir(buf, 0777);
#endif
			buf[i] = '/';
		}
	}
#ifdef PHP_WIN32
	_mkdir(buf);
#else
	mkdir(buf, 0777);
#endif
}

/* ------------------------------------------------------------------ */
/* mre_json_escape: escape a string for JSON                            */
/* - quotes, backslash and control chars are escaped                    */
/* - bytes >= 0x80 pass through unchanged (UTF-8 stays UTF-8)           */
/* Returns the number of bytes written (excluding the NUL).             */
/* ------------------------------------------------------------------ */
size_t mre_json_escape(const char *in, size_t in_len, char *out, size_t out_cap)
{
	size_t i = 0, o = 0;

	if (!out || out_cap == 0) {
		return 0;
	}
	if (!in) {
		out[0] = '\0';
		return 0;
	}

	while (i < in_len && o + 6 < out_cap) {
		unsigned char c = (unsigned char)in[i];
		switch (c) {
			case '"':  out[o++] = '\\'; out[o++] = '"';  break;
			case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
			case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
			case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
			case '\t': out[o++] = '\\'; out[o++] = 't';  break;
			case '\b': out[o++] = '\\'; out[o++] = 'b';  break;
			case '\f': out[o++] = '\\'; out[o++] = 'f';  break;
			default:
				if (c < 0x20 || c == 0x7f) {
					o += (size_t)snprintf(out + o, out_cap - o, "\\u%04x", c);
				} else {
					out[o++] = (char)c;
				}
				break;
		}
		i++;
	}
	out[o] = '\0';
	return o;
}
