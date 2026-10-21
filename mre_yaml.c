/*
  +----------------------------------------------------------------------+
  | mre - built-in minimal YAML parser (no libyaml)                      |
  |                                                                      |
  | Supported subset (enough for stage-B rule files):                    |
  |   - nested mappings with space indentation                           |
  |   - sequences ("- item"), incl. "- key: value" item maps             |
  |   - plain scalars, single/double quoted scalars                      |
  |   - comments (# at line start or after whitespace)                   |
  |   - literal block scalars "key: |"                                   |
  | Not supported: inline flow {} [], anchors &, aliases *, tags,        |
  | multi-document semantics (--- is skipped), tabs in indentation.      |
  |                                                                      |
  | All nodes live in persistent memory (safe for MINIT-time parsing),   |
  | freed by mre_yaml_free() at MSHUTDOWN / reload.                      |
  +----------------------------------------------------------------------+
*/

#include "php_mre.h"

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* memory helpers (persistent pool)                                     */
/* ------------------------------------------------------------------ */
#define MRE_YALLOC(size)     pemalloc((size), 1)
#define MRE_YFREE(ptr)       pefree((ptr), 1)
#define MRE_YSTRDUP(s, n)    mre_y_strndup((s), (n))

static char *mre_y_strndup(const char *s, size_t n)
{
	char *p = (char *)MRE_YALLOC(n + 1);
	if (n > 0) {
		memcpy(p, s, n);
	}
	p[n] = '\0';
	return p;
}

/* ------------------------------------------------------------------ */
/* node construction                                                    */
/* ------------------------------------------------------------------ */
static mre_yaml_node *mre_y_node(mre_yaml_kind kind)
{
	mre_yaml_node *n = (mre_yaml_node *)MRE_YALLOC(sizeof(mre_yaml_node));
	memset(n, 0, sizeof(*n));
	n->kind = kind;
	return n;
}

void mre_yaml_free(mre_yaml_node *node)
{
	size_t i;

	if (!node) {
		return;
	}
	if (node->scalar) {
		MRE_YFREE(node->scalar);
	}
	for (i = 0; i < node->count; i++) {
		if (node->kind == MRE_YAML_MAP) {
			if (node->keys && node->keys[i]) {
				MRE_YFREE(node->keys[i]);
			}
			if (node->values) {
				mre_yaml_free(node->values[i]);
			}
		} else if (node->kind == MRE_YAML_LIST) {
			if (node->items) {
				mre_yaml_free(node->items[i]);
			}
		}
	}
	if (node->keys) {
		MRE_YFREE(node->keys);
	}
	if (node->values) {
		MRE_YFREE(node->values);
	}
	if (node->items) {
		MRE_YFREE(node->items);
	}
	MRE_YFREE(node);
}

static void mre_y_map_add(mre_yaml_node *map, char *key_owned, mre_yaml_node *value)
{
	char **nk = (char **)MRE_YALLOC((map->count + 1) * sizeof(char *));
	mre_yaml_node **nv = (mre_yaml_node **)MRE_YALLOC((map->count + 1) * sizeof(mre_yaml_node *));

	if (map->count > 0) {
		memcpy(nk, map->keys, map->count * sizeof(char *));
		memcpy(nv, map->values, map->count * sizeof(mre_yaml_node *));
		MRE_YFREE(map->keys);
		MRE_YFREE(map->values);
	}
	nk[map->count] = key_owned;
	nv[map->count] = value;
	map->keys = nk;
	map->values = nv;
	map->count++;
}

static void mre_y_list_add(mre_yaml_node *list, mre_yaml_node *item)
{
	mre_yaml_node **ni = (mre_yaml_node **)MRE_YALLOC((list->count + 1) * sizeof(mre_yaml_node *));

	if (list->count > 0) {
		memcpy(ni, list->items, list->count * sizeof(mre_yaml_node *));
		MRE_YFREE(list->items);
	}
	ni[list->count] = item;
	list->items = ni;
	list->count++;
}

/* ------------------------------------------------------------------ */
/* accessors                                                            */
/* ------------------------------------------------------------------ */
const mre_yaml_node *mre_yaml_map_get(const mre_yaml_node *map, const char *key)
{
	size_t i;

	if (!map || map->kind != MRE_YAML_MAP) {
		return NULL;
	}
	for (i = 0; i < map->count; i++) {
		if (map->keys[i] && strcmp(map->keys[i], key) == 0) {
			return map->values[i];
		}
	}
	return NULL;
}

const char *mre_yaml_scalar(const mre_yaml_node *node)
{
	if (!node || node->kind != MRE_YAML_SCALAR) {
		return NULL;
	}
	return node->scalar ? node->scalar : "";
}

/* ------------------------------------------------------------------ */
/* line model                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
	char  *text;   /* content after indentation, comments not stripped */
	size_t indent; /* count of leading spaces */
	size_t no;     /* 1-based line number for error messages */
} mre_y_line;

typedef struct {
	mre_y_line *lines;
	size_t      count;
	size_t      pos;
	char       *err;
	size_t      err_cap;
} mre_y_parser;

static void mre_y_err(mre_y_parser *p, size_t line_no, const char *msg)
{
	if (p->err && p->err[0] == '\0') {
		snprintf(p->err, p->err_cap, "yaml error at line %d: %s",
			(int)line_no, msg);
	}
}

/* strip a trailing comment outside of quotes; returns new length */
static size_t mre_y_strip_comment(const char *s, size_t len)
{
	size_t i;
	char quote = 0;

	for (i = 0; i < len; i++) {
		char c = s[i];
		if (quote) {
			if (c == '\\' && quote == '"') {
				i++; /* skip escaped char */
			} else if (c == quote) {
				quote = 0;
			}
		} else {
			if (c == '\'' || c == '"') {
				quote = c;
			} else if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
				return i;
			}
		}
	}
	return len;
}

/* right-trim spaces; returns new length */
static size_t mre_y_rtrim(const char *s, size_t len)
{
	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
		len--;
	}
	return len;
}

/* does the line (content part) start with "- " or is it exactly "-"? */
static zend_bool mre_y_is_dash(const char *s, size_t len)
{
	return len > 0 && s[0] == '-'
		&& (len == 1 || s[1] == ' ' || s[1] == '\t');
}

/* find the key/value separator ':' outside quotes, followed by space or EOL */
static size_t mre_y_find_colon(const char *s, size_t len)
{
	size_t i;
	char quote = 0;

	for (i = 0; i < len; i++) {
		char c = s[i];
		if (quote) {
			if (c == '\\' && quote == '"') {
				i++;
			} else if (c == quote) {
				quote = 0;
			}
		} else {
			if (c == '\'' || c == '"') {
				quote = c;
			} else if (c == ':'
				&& (i + 1 >= len || s[i + 1] == ' ' || s[i + 1] == '\t')) {
				return i;
			}
		}
	}
	return (size_t)-1;
}

/* unquote a scalar in place semantics: returns a fresh allocated string */
static char *mre_y_scalar_value(const char *s, size_t len)
{
	if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
		/* double quoted: handle common escapes */
		size_t i, o = 0;
		char *out = (char *)MRE_YALLOC(len); /* result never grows */

		for (i = 1; i + 1 < len; i++) {
			char c = s[i];
			if (c == '\\' && i + 2 < len) {
				char n = s[i + 1];
				switch (n) {
					case 'n':  out[o++] = '\n'; break;
					case 't':  out[o++] = '\t'; break;
					case 'r':  out[o++] = '\r'; break;
					case '"':  out[o++] = '"';  break;
					case '\\': out[o++] = '\\'; break;
					default:   out[o++] = c;    break;
				}
				i++;
			} else {
				out[o++] = c;
			}
		}
		out[o] = '\0';
		return out;
	}
	if (len >= 2 && s[0] == '\'' && s[len - 1] == '\'') {
		/* single quoted: only '' is special (literal quote) */
		size_t i, o = 0;
		char *out = (char *)MRE_YALLOC(len);
		for (i = 1; i + 1 < len; i++) {
			char c = s[i];
			if (c == '\'' && i + 1 < len - 1 && s[i + 1] == '\'') {
				out[o++] = '\'';
				i++;
			} else {
				out[o++] = c;
			}
		}
		out[o] = '\0';
		return out;
	}
	return MRE_YSTRDUP(s, len);
}

static mre_yaml_node *mre_y_parse_block(mre_y_parser *p, size_t min_indent);

/* literal block scalar: consume following lines deeper than parent indent */
static char *mre_y_parse_block_scalar(mre_y_parser *p, size_t parent_indent)
{
	char *out;
	size_t out_cap = 256, out_len = 0;

	out = (char *)MRE_YALLOC(out_cap);
	out[0] = '\0';

	while (p->pos < p->count) {
		mre_y_line *l = &p->lines[p->pos];
		size_t len;

		/* blank lines belong to the block when more content follows */
		if (l->indent == 0 && l->text[0] == '\0') {
			/* peek: does any deeper line follow? */
			size_t j = p->pos + 1;
			zend_bool deeper = (zend_bool)0;
			while (j < p->count) {
				if (p->lines[j].text[0] != '\0') {
					deeper = (zend_bool)(p->lines[j].indent > parent_indent);
					break;
				}
				j++;
			}
			if (!deeper) {
				break;
			}
			if (out_len + 1 >= out_cap) {
				out_cap *= 2;
				out = (char *)perealloc(out, out_cap, 1);
			}
			out[out_len++] = '\n';
			out[out_len] = '\0';
			p->pos++;
			continue;
		}
		if (l->indent <= parent_indent) {
			break;
		}
		len = mre_y_rtrim(l->text, strlen(l->text));
		if (out_len + len + 2 >= out_cap) {
			while (out_len + len + 2 >= out_cap) {
				out_cap *= 2;
			}
			out = (char *)perealloc(out, out_cap, 1);
		}
		if (out_len > 0) {
			out[out_len++] = '\n';
		}
		memcpy(out + out_len, l->text, len);
		out_len += len;
		out[out_len] = '\0';
		p->pos++;
	}
	return out;
}

static mre_yaml_node *mre_y_parse_scalar_line(mre_y_parser *p, mre_y_line *l,
                                              const char *s, size_t len)
{
	mre_yaml_node *n;

	if (len >= 1 && s[0] == '|') {
		/* literal block scalar */
		n = mre_y_node(MRE_YAML_SCALAR);
		n->scalar = mre_y_parse_block_scalar(p, l->indent);
		return n;
	}
	n = mre_y_node(MRE_YAML_SCALAR);
	n->scalar = mre_y_scalar_value(s, len);
	return n;
}

/*
 * Parse a block starting at p->pos with indent >= min_indent.
 * Returns NULL (without error) when the next line is less indented.
 */
static mre_yaml_node *mre_y_parse_block(mre_y_parser *p, size_t min_indent)
{
	mre_y_line *first;
	size_t len;
	mre_yaml_node *node;

	while (p->pos < p->count && p->lines[p->pos].text[0] == '\0') {
		p->pos++; /* blank / comment-only lines are insignificant here */
	}
	if (p->pos >= p->count) {
		return NULL;
	}
	first = &p->lines[p->pos];
	if (first->text[0] == '\0' || first->indent < min_indent) {
		return NULL;
	}
	len = mre_y_rtrim(first->text, strlen(first->text));

	if (mre_y_is_dash(first->text, len)) {
		/* -------- sequence -------- */
		size_t seq_indent = first->indent;

		node = mre_y_node(MRE_YAML_LIST);
		while (p->pos < p->count) {
			mre_y_line *l = &p->lines[p->pos];
			size_t llen, rest_off;

			if (l->text[0] == '\0') {
				p->pos++;
				continue;
			}
			if (l->indent != seq_indent) {
				break;
			}
			llen = mre_y_rtrim(l->text, strlen(l->text));
			if (!mre_y_is_dash(l->text, llen)) {
				break;
			}
			rest_off = 1;
			while (rest_off < llen && (l->text[rest_off] == ' ' || l->text[rest_off] == '\t')) {
				rest_off++;
			}
			if (rest_off >= llen) {
				/* bare "-": the item is a nested block */
				p->pos++;
				{
					mre_yaml_node *item = mre_y_parse_block(p, seq_indent + 1);
					if (item) {
						mre_y_list_add(node, item);
					} else {
						mre_y_list_add(node, mre_y_node(MRE_YAML_SCALAR));
					}
				}
				continue;
			}

			/* inline item: scalar, or the start of a map ("key: ...") */
			{
				const char *rest = l->text + rest_off;
				size_t rest_len = llen - rest_off;
				size_t colon = mre_y_find_colon(rest, rest_len);

				if (colon != (size_t)-1) {
					/* map item: rewrite "- " into spaces and re-parse as map */
					size_t map_indent = l->indent + rest_off;
					char *saved = l->text;

					l->text = (char *)MRE_YALLOC(map_indent + rest_len + 1);
					memset(l->text, ' ', map_indent);
					memcpy(l->text + map_indent, rest, rest_len);
					l->text[map_indent + rest_len] = '\0';
					l->indent = map_indent;
					MRE_YFREE(saved);

					{
						mre_yaml_node *item = mre_y_parse_block(p, map_indent);
						mre_y_list_add(node, item ? item : mre_y_node(MRE_YAML_SCALAR));
					}
				} else {
					mre_y_list_add(node, mre_y_parse_scalar_line(p, l, rest, rest_len));
					p->pos++;
				}
			}
		}
		return node;
	}

	/* -------- mapping -------- */
	{
		size_t map_indent = first->indent;

		node = mre_y_node(MRE_YAML_MAP);
		while (p->pos < p->count) {
			mre_y_line *l = &p->lines[p->pos];
			size_t llen, colon;
			char *key;
			mre_yaml_node *value;

			if (l->text[0] == '\0') {
				p->pos++;
				continue;
			}
			if (l->indent != map_indent) {
				break;
			}
			llen = mre_y_rtrim(l->text, strlen(l->text));
			if (mre_y_is_dash(l->text, llen)) {
				break; /* a nested sequence under the parent, not our key */
			}
			colon = mre_y_find_colon(l->text, llen);
			if (colon == (size_t)-1) {
				mre_y_err(p, l->no, "expected 'key: value'");
				mre_yaml_free(node);
				return NULL;
			}

			{
				/* trim leading indentation from the key */
				size_t kskip = 0;
				while (kskip < colon
						&& (l->text[kskip] == ' ' || l->text[kskip] == '\t')) {
					kskip++;
				}
				key = mre_y_scalar_value(l->text + kskip, colon - kskip);
			}
			{
				const char *vraw = l->text + colon + 1;
				size_t vlen = mre_y_rtrim(vraw, llen - colon - 1);
				/* skip leading spaces of the inline value */
				{
					size_t skip = 0;
					while (skip < vlen && (vraw[skip] == ' ' || vraw[skip] == '\t')) {
						skip++;
					}
					vraw += skip;
					vlen -= skip;
				}

				if (vlen == 0) {
					/* value is a nested block (map/list) */
					p->pos++;
					value = mre_y_parse_block(p, map_indent + 1);
					if (!value) {
						if (p->err && p->err[0] != '\0') {
							/* nested parse failed: propagate the error */
							MRE_YFREE(key);
							mre_yaml_free(node);
							return NULL;
						}
						value = mre_y_node(MRE_YAML_SCALAR);
						value->scalar = MRE_YSTRDUP("", 0);
					}
				} else {
					p->pos++;
					value = mre_y_parse_scalar_line(p, l, vraw, vlen);
				}
			}
			mre_y_map_add(node, key, value);
		}
		return node;
	}
}

/* ------------------------------------------------------------------ */
/* public: parse a whole document                                       */
/* ------------------------------------------------------------------ */
mre_yaml_node *mre_yaml_parse(const char *data, size_t len,
                              char *err, size_t err_cap)
{
	mre_y_parser p;
	mre_yaml_node *root;
	size_t i, line_start;
	size_t line_cap = 16;
	mre_y_line *lines;

	if (err && err_cap > 0) {
		err[0] = '\0';
	}
	if (!data) {
		return NULL;
	}

	memset(&p, 0, sizeof(p));
	p.err = err;
	p.err_cap = err_cap;
	lines = (mre_y_line *)MRE_YALLOC(line_cap * sizeof(mre_y_line));

	/* split into lines */
	line_start = 0;
	p.count = 0;
	for (i = 0; i <= len; i++) {
		if (i == len || data[i] == '\n') {
			size_t raw = i - line_start;
			size_t ls;
			mre_y_line *l;

			if (p.count >= line_cap) {
				mre_y_line *nl;
				line_cap *= 2;
				nl = (mre_y_line *)MRE_YALLOC(line_cap * sizeof(mre_y_line));
				memcpy(nl, lines, p.count * sizeof(mre_y_line));
				MRE_YFREE(lines);
				lines = nl;
			}
			l = &lines[p.count];
			/* strip \r (windows) */
			if (raw > 0 && data[line_start + raw - 1] == '\r') {
				raw--;
			}
			/* count indentation */
			ls = 0;
			while (ls < raw && data[line_start + ls] == ' ') {
				ls++;
			}
			if (ls < raw && data[line_start + ls] == '\t') {
				mre_y_err(&p, p.count + 1, "tab characters are not allowed in indentation");
			}
			l->indent = ls;
			{
				const char *content = data + line_start + ls;
				size_t clen = (raw > ls) ? mre_y_strip_comment(content, raw - ls) : 0;
				clen = mre_y_rtrim(content, clen);
				l->text = MRE_YSTRDUP(content, clen);
			}
			l->no = p.count + 1;
			p.count++;

			line_start = i + 1;
		}
	}

	p.lines = lines;
	p.pos = 0;

	/* skip a leading document marker */
	if (p.count > 0 && strcmp(p.lines[0].text, "---") == 0) {
		p.pos = 1;
	}

	root = mre_y_parse_block(&p, 0);
	if (!root && (err == NULL || err[0] == '\0') && p.pos < p.count) {
		mre_y_err(&p, p.lines[p.pos].no, "unexpected content at document level");
	}
	/* trailing document content means the doc is a plain scalar; accept it */

	/* free line texts */
	for (i = 0; i < p.count; i++) {
		MRE_YFREE(p.lines[i].text);
	}
	MRE_YFREE(p.lines);

	return root;
}

/* ------------------------------------------------------------------ */
/* yaml config lifecycle (globals-backed)                               */
/* ------------------------------------------------------------------ */

/* stat() based mtime check */
static time_t mre_y_file_mtime(const char *path, zend_bool *exists)
{
#ifdef PHP_WIN32
	struct _stat st;
	*exists = (zend_bool)(_stat(path, &st) == 0);
	if (*exists) {
		return (time_t)st.st_mtime;
	}
#else
	struct stat st;
	*exists = (zend_bool)(stat(path, &st) == 0);
	if (*exists) {
		return st.st_mtime;
	}
#endif
	return 0;
}

void mre_yaml_config_free(void)
{
	if (MRE_G(yaml_root)) {
		mre_yaml_free(MRE_G(yaml_root));
		MRE_G(yaml_root) = NULL;
	}
	if (MRE_G(yaml_path)) {
		pefree(MRE_G(yaml_path), 1);
		MRE_G(yaml_path) = NULL;
	}
	MRE_G(yaml_mtime) = 0;
}

/*
 * Load / reload mre.yaml_config when configured.
 * Reloads when the path changed or the file mtime changed.
 * Safe to call on every RINIT.
 */
void mre_yaml_config_ensure(void)
{
	const mre_config *cfg;
	char errbuf[256];
	zend_bool exists = (zend_bool)0;
	time_t mtime;
	FILE *fp;
	char *data;
	long size;
	mre_yaml_node *root;

	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	cfg = &MRE_G(cfg);

	if (cfg->yaml_config[0] == '\0') {
		if (MRE_G(yaml_root)) {
			mre_yaml_config_free();
		}
		return;
	}

	mtime = mre_y_file_mtime(cfg->yaml_config, &exists);
	if (MRE_G(yaml_root) && MRE_G(yaml_path)
			&& strcmp(MRE_G(yaml_path), cfg->yaml_config) == 0
			&& MRE_G(yaml_mtime) == mtime && exists) {
		return; /* up to date */
	}

	mre_yaml_config_free();

	if (!exists) {
		if (!MRE_G(yaml_warned)) {
			MRE_G(yaml_warned) = (zend_bool)1;
			fprintf(stderr, "[mre] warning: yaml_config '%s' does not exist\n",
				cfg->yaml_config);
		}
		return;
	}

	fp = fopen(cfg->yaml_config, "rb");
	if (!fp) {
		fprintf(stderr, "[mre] warning: cannot open yaml_config '%s'\n",
			cfg->yaml_config);
		return;
	}
	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (size < 0 || size > 4 * 1024 * 1024) {
		fclose(fp);
		fprintf(stderr, "[mre] warning: yaml_config '%s' too large or unreadable\n",
			cfg->yaml_config);
		return;
	}
	data = (char *)MRE_YALLOC((size_t)size + 1);
	if (size > 0 && fread(data, 1, (size_t)size, fp) != (size_t)size) {
		MRE_YFREE(data);
		fclose(fp);
		fprintf(stderr, "[mre] warning: short read on yaml_config '%s'\n",
			cfg->yaml_config);
		return;
	}
	fclose(fp);
	data[size] = '\0';

	root = mre_yaml_parse(data, (size_t)size, errbuf, sizeof(errbuf));
	MRE_YFREE(data);

	if (!root || errbuf[0] != '\0') {
		if (root) {
			mre_yaml_free(root);
		}
		if (!MRE_G(yaml_warned)) {
			MRE_G(yaml_warned) = (zend_bool)1;
			fprintf(stderr, "[mre] warning: yaml_config '%s' failed to parse: %s\n",
				cfg->yaml_config, errbuf[0] ? errbuf : "unknown error");
		}
		return;
	}
	MRE_G(yaml_warned) = (zend_bool)0; /* success clears the one-shot warn */

	MRE_G(yaml_root) = root;
	MRE_G(yaml_path) = (char *)pemalloc(strlen(cfg->yaml_config) + 1, 1);
	memcpy(MRE_G(yaml_path), cfg->yaml_config, strlen(cfg->yaml_config) + 1);
	MRE_G(yaml_mtime) = mtime;
}

/* ------------------------------------------------------------------ */
/* debug helper: convert the parsed tree to a PHP array                 */
/* ------------------------------------------------------------------ */
static void mre_y_node_to_zval(const mre_yaml_node *node, zval *out)
{
	size_t i;

	if (!node || node->kind == MRE_YAML_SCALAR) {
		ZVAL_STRING(out, node && node->scalar ? node->scalar : "");
		return;
	}
	array_init(out);
	for (i = 0; i < node->count; i++) {
		zval child;
		if (node->kind == MRE_YAML_MAP) {
			mre_y_node_to_zval(node->values[i], &child);
			add_assoc_zval_ex(out, node->keys[i], strlen(node->keys[i]), &child);
		} else {
			mre_y_node_to_zval(node->items[i], &child);
			add_next_index_zval(out, &child);
		}
	}
}

zend_bool mre_yaml_dump_to_zval(zval *return_value)
{
	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	/* pick up runtime ini_set('mre.yaml_config', ...) changes immediately */
	mre_config_refresh();
	mre_yaml_config_ensure();
	if (!MRE_G(yaml_root)) {
		ZVAL_NULL(return_value);
		return (zend_bool)0;
	}
	mre_y_node_to_zval(MRE_G(yaml_root), return_value);
	return (zend_bool)1;
}
