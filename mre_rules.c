/*
  +----------------------------------------------------------------------+
  | mre - yaml rule engine (stage B)                                     |
  |                                                                      |
  | Reads the "rules:" list (and "default_log_file") from the parsed     |
  | yaml tree and compiles every rule's PCRE-style pattern to a PCRE2    |
  | code exactly once per yaml (re)load.  A record is routed to the      |
  | first matching rule's log_file; otherwise default_log_file wins;     |
  | otherwise the caller falls back to mre.log_file.                     |
  |                                                                      |
  | Pattern syntax is PHP preg style: "/pattern/mods" with modifiers     |
  |   i (caseless) m (multiline) s (dotall) x (extended)                 |
  |   u (utf)      U (ungreedy)  A (anchored)                            |
  | A pattern without a valid delimiter pair is used verbatim with no    |
  | modifiers.  A pattern that fails to compile is skipped with a        |
  | one-time stderr warning naming the rule and the PCRE2 error.         |
  |                                                                      |
  | PCRE2 is not linked in: the extension declares the small set of      |
  | functions it needs (pcre2_compile_8, pcre2_match_8, ...) and lets    |
  | the dynamic linker resolve them against the PHP process' own PCRE2   |
  | (Debian/Ubuntu: libpcre2-8.so.0 is linked by the PHP binary itself;  |
  | source builds: the binary exports the symbols).  The option values   |
  | below are part of the stable PCRE2 10.x public ABI.                  |
  |                                                                      |
  | The rule array lives in malloc'ed memory (survives requests), is     |
  | rebuilt only when the yaml tree is reloaded and freed at MSHUTDOWN.  |
  +----------------------------------------------------------------------+
*/

#include "php_mre.h"

#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* minimal self-contained PCRE2 8-bit API (stable 10.x ABI)             */
/* ------------------------------------------------------------------ */
#define MRE_PCRE2_CASELESS   0x00000008u
#define MRE_PCRE2_DOTALL     0x00000020u
#define MRE_PCRE2_EXTENDED   0x00000080u
#define MRE_PCRE2_MULTILINE  0x00000400u
#define MRE_PCRE2_UNGREEDY   0x00040000u
#define MRE_PCRE2_UTF        0x00080000u
#define MRE_PCRE2_ANCHORED   0x80000000u

#define MRE_PCRE2_NOMATCH    (-1)

typedef struct mre_pcre2_code mre_pcre2_code;

extern mre_pcre2_code *pcre2_compile_8(
	const unsigned char *pattern, size_t pattern_len, unsigned int options,
	int *errorcode, size_t *erroroffset, void *ccontext);
extern void pcre2_code_free_8(mre_pcre2_code *code);
extern int pcre2_match_8(
	const mre_pcre2_code *code, const unsigned char *subject,
	size_t length, size_t startoffset, unsigned int options,
	void *match_data, void *match_context);
extern void *pcre2_match_data_create_from_pattern_8(
	const mre_pcre2_code *code, void *gcontext);
extern void pcre2_match_data_free_8(void *match_data);
extern int pcre2_get_error_message_8(int errorcode,
	unsigned char *buffer, size_t size);

/* ------------------------------------------------------------------ */
/* rule storage                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
	char *name;         /* rule name ("" when absent)                   */
	char *type;         /* "mysql" | "redis" | "" (= both)              */
	char *pattern;      /* original match string (for the dump)         */
	char *log_file;     /* destination when the rule matches            */
	mre_pcre2_code *code;
	void *match_data;   /* created from code, reused between records    */
} mre_rule;

#define MRE_RULE_LOG_MAX MRE_PATH_MAX

static void mre_rule_free(mre_rule *r)
{
	if (r->code) {
		pcre2_code_free_8(r->code);
	}
	if (r->match_data) {
		pcre2_match_data_free_8(r->match_data);
	}
	free(r->name);
	free(r->type);
	free(r->pattern);
	free(r->log_file);
	memset(r, 0, sizeof(*r));
}

void mre_rules_free(void)
{
	mre_rule *rules = (mre_rule *)MRE_G(rules);
	size_t i;

	for (i = 0; rules && i < MRE_G(rules_count); i++) {
		mre_rule_free(&rules[i]);
	}
	free(rules);
	MRE_G(rules) = NULL;
	MRE_G(rules_count) = 0;

	free(MRE_G(rules_default_log));
	MRE_G(rules_default_log) = NULL;

	free(MRE_G(rules_src_path));
	MRE_G(rules_src_path) = NULL;
	MRE_G(rules_src_mtime) = 0;
	MRE_G(rules_src_root) = NULL;
}

/* ------------------------------------------------------------------ */
/* pattern compile: "/body/mods" or verbatim                            */
/* ------------------------------------------------------------------ */
static char mre_pcre_close_of(char open)
{
	switch (open) {
		case '(': return ')';
		case '[': return ']';
		case '{': return '}';
		case '<': return '>';
		default:  return open;
	}
}

static mre_pcre2_code *mre_rule_compile(const char *in, size_t len,
                                        char *errbuf, size_t errcap)
{
	const char *body = in;
	size_t body_len = len;
	unsigned int opts = 0;
	char *tmp;
	mre_pcre2_code *code;
	int errcode = 0;
	size_t erroff = 0;

	if (len >= 2) {
		char open = in[0];
		char close;
		const char *last;

		if (!(open == '\\' || open == ' ' || (open >= 'a' && open <= 'z')
				|| (open >= 'A' && open <= 'Z')
				|| (open >= '0' && open <= '9'))) {
			size_t i;
			size_t depth = 0;

			close = mre_pcre_close_of(open);
			/* find the last unescaped closing delimiter */
			last = NULL;
			for (i = 1; i < len; i++) {
				if (in[i] == '\\' && i + 1 < len) {
					i++;
					continue;
				}
				if (open != close && in[i] == open) {
					depth++; /* nested opener: keep scanning */
					continue;
				}
				if (in[i] == close) {
					if (depth > 0) {
						depth--;
						continue;
					}
					last = in + i;
				}
			}
			if (last && last > in + 1) {
				body = in + 1;
				body_len = (size_t)(last - body);

				/* modifiers after the closing delimiter */
				for (i = (size_t)(last - in) + 1; i < len; i++) {
					switch (in[i]) {
						case 'i': opts |= MRE_PCRE2_CASELESS;  break;
						case 'm': opts |= MRE_PCRE2_MULTILINE; break;
						case 's': opts |= MRE_PCRE2_DOTALL;    break;
						case 'x': opts |= MRE_PCRE2_EXTENDED;  break;
						case 'u': opts |= MRE_PCRE2_UTF;       break;
						case 'U': opts |= MRE_PCRE2_UNGREEDY;  break;
						case 'A': opts |= MRE_PCRE2_ANCHORED;  break;
						default:
							snprintf(errbuf, errcap,
								"unsupported modifier '%c'", in[i]);
							return NULL;
					}
				}
			}
			/* no closing delimiter found: use the whole string verbatim */
		}
	}

	tmp = (char *)malloc(body_len + 1);
	if (!tmp) {
		snprintf(errbuf, errcap, "out of memory");
		return NULL;
	}
	memcpy(tmp, body, body_len);
	tmp[body_len] = '\0';

	code = pcre2_compile_8((const unsigned char *)tmp, body_len, opts,
		&errcode, &erroff, NULL);
	free(tmp);

	if (!code) {
		unsigned char msg[256];
		if (pcre2_get_error_message_8(errcode, msg, sizeof(msg)) >= 0) {
			snprintf(errbuf, errcap, "PCRE2: %s (offset %d)",
				(const char *)msg, (int)erroff);
		} else {
			snprintf(errbuf, errcap, "PCRE2 error code %d (offset %d)",
				errcode, (int)erroff);
		}
		return NULL;
	}
	return code;
}

/* ------------------------------------------------------------------ */
/* rebuild from the current yaml tree                                   */
/* ------------------------------------------------------------------ */
static void mre_rules_warn(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "[mre] warning: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

static void mre_rules_rebuild(void)
{
	const mre_yaml_node *root, *rules_node, *def;
	mre_rule *rules = NULL;
	size_t n = 0, cap = 0;
	size_t i;

	root = MRE_G(yaml_root);
	mre_rules_free(); /* also clears the src markers */

	/* default_log_file */
	def = mre_yaml_map_get(root, "default_log_file");
	if (def && mre_yaml_scalar(def)[0] != '\0') {
		MRE_G(rules_default_log) = strdup(mre_yaml_scalar(def));
	}

	rules_node = mre_yaml_map_get(root, "rules");
	if (!rules_node || rules_node->kind != MRE_YAML_LIST) {
		goto done;
	}

	for (i = 0; i < rules_node->count; i++) {
		const mre_yaml_node *item = rules_node->items[i];
		const mre_yaml_node *nv_match, *nv_log, *nv_name, *nv_type;
		const char *match, *log, *name, *type;
		mre_rule r;
		char errbuf[256];

		if (!item || item->kind != MRE_YAML_MAP) {
			continue;
		}
		nv_match = mre_yaml_map_get(item, "match");
		nv_log   = mre_yaml_map_get(item, "log_file");
		if (!nv_log) {
			nv_log = mre_yaml_map_get(item, "log"); /* design-doc alias */
		}
		nv_name  = mre_yaml_map_get(item, "name");
		nv_type  = mre_yaml_map_get(item, "type");

		match = nv_match ? mre_yaml_scalar(nv_match) : NULL;
		log   = nv_log   ? mre_yaml_scalar(nv_log)   : NULL;
		name  = nv_name  ? mre_yaml_scalar(nv_name)  : "";
		type  = nv_type  ? mre_yaml_scalar(nv_type)  : "";

		if (!match || !*match || !log || !*log) {
			mre_rules_warn("rule #%d%s%s skipped: 'match' and 'log_file' are required",
				(int)(i + 1),
				(name && *name) ? " (" : "", (name && *name) ? name : "");
			continue;
		}
		if (*type && strcmp(type, MRE_TYPE_MYSQL) != 0
				&& strcmp(type, MRE_TYPE_REDIS) != 0) {
			mre_rules_warn("rule '%s' skipped: type must be mysql, redis or empty", name);
			continue;
		}

		memset(&r, 0, sizeof(r));
		r.pattern = strdup(match);
		r.log_file = strdup(log);
		r.name = strdup(name ? name : "");
		r.type = strdup(type ? type : "");
		if (!r.pattern || !r.log_file || !r.name || !r.type) {
			mre_rule_free(&r);
			continue;
		}

		r.code = mre_rule_compile(match, strlen(match), errbuf, sizeof(errbuf));
		if (!r.code) {
			/* clear warning per spec; never fatal */
			mre_rules_warn("rule '%s' pattern '%s' failed to compile: %s; rule skipped",
				name, match, errbuf);
			mre_rule_free(&r);
			continue;
		}
		r.match_data = pcre2_match_data_create_from_pattern_8(r.code, NULL);

		if (n >= cap) {
			size_t ncap = cap ? cap * 2 : 8;
			mre_rule *nr = (mre_rule *)realloc(rules, ncap * sizeof(mre_rule));
			if (!nr) {
				mre_rule_free(&r);
				break;
			}
			rules = nr;
			cap = ncap;
		}
		rules[n++] = r;
	}

done:
	MRE_G(rules) = rules;
	MRE_G(rules_count) = n;
	MRE_G(rules_src_path) = MRE_G(yaml_path) ? strdup(MRE_G(yaml_path)) : NULL;
	MRE_G(rules_src_mtime) = MRE_G(yaml_mtime);
	MRE_G(rules_src_root) = MRE_G(yaml_root);
}

/* rebuild only when the loaded yaml tree changed (hot reload) */
zend_bool mre_rules_sync(void)
{
	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	if (MRE_G(rules_src_root) == MRE_G(yaml_root)
			&& MRE_G(rules_src_mtime) == MRE_G(yaml_mtime)) {
		return (zend_bool)(MRE_G(rules_count) > 0);
	}
	mre_rules_rebuild();
	return (zend_bool)(MRE_G(rules_count) > 0);
}

/* ------------------------------------------------------------------ */
/* matching                                                             */
/* ------------------------------------------------------------------ */
static zend_bool mre_rule_matches(const mre_rule *r,
                                  const char *content, size_t content_len)
{
	int rc;

	if (!r->code || !r->match_data) {
		return (zend_bool)0;
	}
	rc = pcre2_match_8(r->code, (const unsigned char *)content, content_len,
		0, 0, r->match_data, NULL);
	return (zend_bool)(rc != MRE_PCRE2_NOMATCH && rc >= 0);
}

zend_bool mre_rules_route(const char *type, const char *content,
                          size_t content_len, char *out, size_t cap)
{
	mre_rule *rules = (mre_rule *)MRE_G(rules);
	size_t i;

	if (!rules || MRE_G(rules_count) == 0) {
		/* yaml present without rules: still honour default_log_file */
		if (MRE_G(rules_src_root) && MRE_G(rules_default_log)
				&& MRE_G(rules_default_log)[0] != '\0') {
			mre_copy_str(out, cap, MRE_G(rules_default_log),
				strlen(MRE_G(rules_default_log)));
			return (zend_bool)1;
		}
		return (zend_bool)0;
	}

	for (i = 0; i < MRE_G(rules_count); i++) {
		mre_rule *r = &rules[i];

		if (r->type[0] != '\0' && strcmp(r->type, type ? type : "") != 0) {
			continue;
		}
		if (mre_rule_matches(r, content ? content : "", content_len)) {
			mre_copy_str(out, cap, r->log_file, strlen(r->log_file));
			return (zend_bool)1;
		}
	}

	/* ordered, first match wins; nothing matched -> default_log_file */
	if (MRE_G(rules_default_log) && MRE_G(rules_default_log)[0] != '\0') {
		mre_copy_str(out, cap, MRE_G(rules_default_log),
			strlen(MRE_G(rules_default_log)));
		return (zend_bool)1;
	}
	return (zend_bool)0;
}

/* ------------------------------------------------------------------ */
/* dump support: effective rules + default_log_file                     */
/* ------------------------------------------------------------------ */
zend_bool mre_rules_dump_to_zval(zval *rules_out, zval *default_out)
{
	mre_rule *rules = (mre_rule *)MRE_G(rules);
	size_t i;

	if (MRE_G(rules_default_log) && MRE_G(rules_default_log)[0] != '\0') {
		ZVAL_STRING(default_out, MRE_G(rules_default_log));
	} else {
		ZVAL_NULL(default_out);
	}

	if (!rules || MRE_G(rules_count) == 0) {
		ZVAL_NULL(rules_out);
		return (zend_bool)0;
	}

	array_init(rules_out);
	for (i = 0; i < MRE_G(rules_count); i++) {
		mre_rule *r = &rules[i];
		zval row;

		array_init(&row);
		add_assoc_string_ex(&row, "name", sizeof("name") - 1, r->name);
		add_assoc_string_ex(&row, "type", sizeof("type") - 1,
			r->type[0] ? r->type : "");
		add_assoc_string_ex(&row, "match", sizeof("match") - 1, r->pattern);
		add_assoc_string_ex(&row, "log_file", sizeof("log_file") - 1, r->log_file);
		add_assoc_bool_ex(&row, "compiled", sizeof("compiled") - 1,
			(zend_bool)(r->code != NULL));
		add_next_index_zval(rules_out, &row);
	}
	return (zend_bool)1;
}

/* ------------------------------------------------------------------ */
/* route resolution: log_script > yaml rule > mre.log_file (caller)     */
/* Fills `out` and returns 1 when a record-specific target was picked.  */
/* ------------------------------------------------------------------ */
zend_bool mre_route_resolve(const char *type, const char *dir,
                            const char *content, size_t content_len,
                            const mre_extra *extra,
                            char *out, size_t cap)
{
	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	if (!MRE_G(cfg).enable) {
		return (zend_bool)0;
	}

	/* keep the compiled rules in sync with the loaded yaml tree */
	mre_rules_sync();

	if (mre_script_route(type, dir, content, content_len, extra, out, cap)) {
		return (zend_bool)1;
	}
	return mre_rules_route(type, content, content_len, out, cap);
}
