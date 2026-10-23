/*
  +----------------------------------------------------------------------+
  | mre - MySQL capture via mysqlnd                                      |
  |                                                                      |
  | Approach: override the global mysqlnd "conn_data" and "stmt" method  |
  | tables (mysqlnd_conn_data_set_methods / mysqlnd_stmt_set_methods)    |
  | and allocate one per-connection context through the official plugin  |
  | connection-data area (mysqlnd_plugin_register).                      |
  |                                                                      |
  | This sits below mysqli AND PDO_MYSQL (both go through mysqlnd), so    |
  | every connection class is covered.                                   |
  |                                                                      |
  | Version differences handled with PHP_VERSION_ID:                     |
  |   - store_result()/use_result() lost their `flags` param in 8.1       |
  |   - res::fetch_into lost `enum_mysqlnd_extension ext` in 8.1          |
  +----------------------------------------------------------------------+
*/

#include "php_mre.h"

#if MRE_HAVE_MYSQLND

#include <string.h>

/* ------------------------------------------------------------------ */
/* per connection context (lives in the mysqlnd plugin data area)       */
/* ------------------------------------------------------------------ */
typedef struct {
	char     sql[MRE_MAX_SQL_LOG]; /* last prepared/executed SQL, reused
	                                verbatim as the execute-line content
	                                (pure SQL, no "EXECUTE " prefix)     */
	size_t   sql_len;
	int      samples_left;         /* row sample budget for current result */
	zend_bool sampling;            /* sampling active for current result   */
	uint64_t rows_total;           /* rows in current (buffered) result    */
	uint64_t fetched;              /* sampled rows so far                  */
	mre_gtx_ctx gtx;               /* stage B: global transaction state    */
} mre_mysql_ctx;

/* originals (possibly already chained by another plugin) */
static func_mysqlnd_conn_data__query      mre_orig_query;
static func_mysqlnd_conn_data__store_result mre_orig_store_result;
static func_mysqlnd_conn_data__use_result mre_orig_use_result;
static func_mysqlnd_conn_data__next_result mre_orig_next_result;
static func_mysqlnd_conn_data__dtor       mre_orig_dtor;
static func_mysqlnd_stmt__prepare         mre_orig_stmt_prepare;
static func_mysqlnd_stmt__execute         mre_orig_stmt_execute;
static func_mysqlnd_stmt__get_result      mre_orig_stmt_get_result;
static func_mysqlnd_stmt__fetch           mre_orig_stmt_fetch;
static func_mysqlnd_res__fetch_into       mre_orig_fetch_into;
static mysqlnd_fetch_row_func             mre_orig_fetch_row;

/* our copies of the method tables + pristine copies for restore */
static MYSQLND_CLASS_METHODS_TYPE(mysqlnd_conn_data) mre_conn_methods;
static MYSQLND_CLASS_METHODS_TYPE(mysqlnd_stmt)      mre_stmt_methods;
static MYSQLND_CLASS_METHODS_TYPE(mysqlnd_conn_data) mre_conn_methods_orig;
static MYSQLND_CLASS_METHODS_TYPE(mysqlnd_stmt)      mre_stmt_methods_orig;

static unsigned int mre_plugin_id = (unsigned int)-1;

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */
static zend_bool mre_mysql_enabled(void)
{
	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	return (zend_bool)(MRE_G(cfg).enable && MRE_G(cfg).capture_mysql);
}

/* stage B: send a raw statement bypassing our own hooks (used by the
 * global transaction finalize: ROLLBACK / COMMIT) */
zend_bool mre_mysql_raw_send(MYSQLND_CONN_DATA *conn, const char *sql)
{
	if (!conn || !mre_orig_query || !sql) {
		return (zend_bool)0;
	}
	return (zend_bool)(mre_orig_query(conn, sql, strlen(sql)) == PASS);
}

/* gtx flag: independent from logging (works with mre.enable=0) */
static zend_bool mre_gtx_enabled(void)
{
	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	return MRE_G(cfg).gtx;
}

static mre_mysql_ctx *mre_ctx_get(MYSQLND_CONN_DATA *conn)
{
	mre_mysql_ctx **slot;

	if (mre_plugin_id == (unsigned int)-1 || !conn) {
		return NULL;
	}
	slot = (mre_mysql_ctx **)mysqlnd_plugin_get_plugin_connection_data_data(
		conn, mre_plugin_id);
	if (!slot) {
		return NULL; /* connection was created before we registered */
	}
	if (!*slot) {
		*slot = (mre_mysql_ctx *)pemalloc(sizeof(mre_mysql_ctx), conn->persistent);
		memset(*slot, 0, sizeof(mre_mysql_ctx));
	}
	return *slot;
}

static void mre_ctx_remember_sql(mre_mysql_ctx *ctx, const char *sql, size_t len)
{
	if (!ctx) {
		return;
	}
	mre_copy_str(ctx->sql, sizeof(ctx->sql), sql, len);
	ctx->sql_len = strlen(ctx->sql);
	ctx->samples_left = MRE_G(cfg).sample_rows;
	ctx->sampling = (zend_bool)(ctx->samples_left > 0);
	ctx->fetched = 0;
	ctx->rows_total = 0;
}

static void mre_extra_conn(mre_extra *e, MYSQLND_CONN_DATA *conn)
{
	mre_extra_init(e);
	if (conn && conn->host_info) {
		e->has_host = (zend_bool)1;
		mre_copy_str(e->host, sizeof(e->host),
			conn->host_info, strlen(conn->host_info));
	}
	if (conn) {
		e->has_port = (zend_bool)1;
		e->port = (int)conn->port;
	}
}

static void mre_extra_error(mre_extra *e, MYSQLND_ERROR_INFO *info)
{
	if (info && info->error_no != 0) {
		e->has_error = (zend_bool)1;
		e->err_no = info->error_no;
		mre_copy_str(e->err_msg, sizeof(e->err_msg),
			info->error, strlen(info->error));
		mre_copy_str(e->sqlstate, sizeof(e->sqlstate),
			info->sqlstate, MYSQLND_SQLSTATE_LENGTH);
	}
}

/* fill one req line's timing block: the wall time captured at call
 * start (overrides the line's top-level "time", which would otherwise
 * be the write moment - the req line is written AFTER the call so the
 * duration exists), the wall time of the response and the monotonic
 * delta between the two clock readings in milliseconds.
 * start == end with cost 0 marks "never really sent" (deferred COMMIT) */
static void mre_extra_timing(mre_extra *e, const char *time_start,
                             const char *time_end, double cost_ms)
{
	e->has_time_start = (zend_bool)1;
	mre_copy_str(e->time_start, sizeof(e->time_start),
		time_start, strlen(time_start));
	e->has_time_end = (zend_bool)1;
	mre_copy_str(e->time_end, sizeof(e->time_end),
		time_end, strlen(time_end));
	e->has_cost = (zend_bool)1;
	e->cost_ms = (cost_ms > 0.0) ? cost_ms : 0.0;
}

static uint64_t mre_affected_rows(MYSQLND_CONN_DATA *conn)
{
	return (conn && conn->upsert_status) ? conn->upsert_status->affected_rows : 0;
}

/* libmysql semantics: (uint64)-1 means "not applicable" (e.g. after SELECT) */
static zend_bool mre_has_affected(uint64_t affected)
{
	return affected != (uint64_t)-1;
}

/* forward declarations (defined below) */
static void mre_mysql_sample_row(MYSQLND_RES *result, zval *row);
static void mre_patch_result(MYSQLND_RES *res);
static void mre_log_result_meta(MYSQLND_CONN_DATA *conn, MYSQLND_RES *res,
                                zend_bool unbuffered);

/* ------------------------------------------------------------------ */
/* bind parameter capture (extra.cmd / extra.prepared / extra.sql_full) */
/*                                                                      */
/* At execute time mysqlnd's MYSQLND_STMT_DATA carries the bound values */
/* in param_bind[i] = {zv, type} (type = MYSQL_TYPE_*, what mysqli's    */
/* bind_param('i'/'d'/'s'/'b') and PDO's bindValue() handed over).  The */
/* original execute call serializes exactly these entries, so reading   */
/* them right before forwarding is a faithful picture of what is sent.  */
/* ------------------------------------------------------------------ */

/* one execute's captured bind state, rendered into mre_extra afterwards */
typedef struct {
	zend_bool    has_prepared;      /* 0 = nothing bound: omit fields  */
	unsigned int param_total;       /* bound params reported by mysqlnd*/
	unsigned int param_cap;         /* captured (<= MRE_PARAM_MAX)     */
	mre_param    params[MRE_PARAM_MAX];
	zend_bool    has_sql_full;
	char         sql_full[MRE_MAX_SQL_LOG];
} mre_prepared_info;

/* render one captured param as an SQL literal (for extra.sql_full)     */
static void mre_param_sql_literal(const mre_param *p, char *out, size_t cap)
{
	switch (p->kind) {
		case MRE_PARAM_NULL:
			mre_copy_str(out, cap, "NULL", 4);
			return;
		case MRE_PARAM_LONG:
			snprintf(out, cap, "%lld", (long long)p->lval);
			return;
		case MRE_PARAM_DOUBLE:
			mre_fmt_double(p->dval, out, cap);
			return;
		case MRE_PARAM_BLOB:
			mre_copy_str(out, cap, p->sval, strlen(p->sval));
			return;
		case MRE_PARAM_STR:
		default: {
			/* single-quoted literal; inner quotes doubled (''), no
			 * backslash escaping (NO_BACKSLASH_ESCAPES semantics -
			 * under other sql_mode a literal \\ is one byte off;
			 * documented limitation, see README) */
			size_t o = 0, i, n = strlen(p->sval);
			if (cap < 3) {
				return;
			}
			out[o++] = '\'';
			/* o + 5: worst case iteration writes 3 bytes ('' + char),
			 * then one closing quote and the NUL must still fit */
			for (i = 0; i < n && o + 5 < cap; i++) {
				if (p->sval[i] == '\'') {
					out[o++] = '\'';
				}
				out[o++] = p->sval[i];
			}
			out[o++] = '\'';
			out[o] = '\0';
			return;
		}
	}
}

/* copy src into dst; when longer than MRE_PARAM_TEXT_MAX bytes cut there
 * and append "…(N)" (N = the original byte length) */
static void mre_param_text_truncate(char *dst, size_t dst_cap,
                                    const char *src, size_t src_len)
{
	char suffix[40];

	if (src_len <= MRE_PARAM_TEXT_MAX) {
		mre_copy_str(dst, dst_cap, src, src_len);
		return;
	}
	snprintf(suffix, sizeof(suffix), "…(%llu)",
		(unsigned long long)src_len);
	mre_copy_str(dst, dst_cap, src, MRE_PARAM_TEXT_MAX);
	mre_copy_str(dst + MRE_PARAM_TEXT_MAX, dst_cap - MRE_PARAM_TEXT_MAX,
		suffix, strlen(suffix));
}

/* capture one bound parameter (declared type wins, zval carries value) */
static void mre_param_capture(mre_param *p, const MYSQLND_PARAM_BIND *pb)
{
	zval *zv = (zval *)&pb->zv;
	const char *src = NULL;
	size_t src_len = 0;

	memset(p, 0, sizeof(*p));
	if (Z_TYPE_P(zv) == IS_REFERENCE) {
		zv = Z_REFVAL_P(zv);
	}
	if (pb->type == MYSQL_TYPE_NULL || !zv || Z_TYPE_P(zv) == IS_UNDEF
			|| Z_TYPE_P(zv) == IS_NULL) {
		p->kind = MRE_PARAM_NULL;
		return;
	}

	switch (pb->type) {
		case MYSQL_TYPE_TINY:
		case MYSQL_TYPE_SHORT:
		case MYSQL_TYPE_LONG:
		case MYSQL_TYPE_LONGLONG:
		case MYSQL_TYPE_YEAR:
			p->kind = MRE_PARAM_LONG;
			p->lval = zval_get_long(zv);
			return;
		case MYSQL_TYPE_DOUBLE:
			p->kind = MRE_PARAM_DOUBLE;
			p->dval = zval_get_double(zv);
			return;
		case MYSQL_TYPE_TINY_BLOB:
		case MYSQL_TYPE_MEDIUM_BLOB:
		case MYSQL_TYPE_LONG_BLOB:
		case MYSQL_TYPE_BLOB: {
			/* "0x" + up to 32 bytes of hex, "…(N bytes)" when cut */
			static const char hexdig[] = "0123456789ABCDEF";
			size_t used = 2, hex_bytes, i;
			char suffix[40];

			p->kind = MRE_PARAM_BLOB;
			if (Z_TYPE_P(zv) == IS_STRING) {
				src = Z_STRVAL_P(zv);
				src_len = Z_STRLEN_P(zv);
			}
			hex_bytes = (src_len < 32) ? src_len : 32;
			p->sval[0] = '0';
			p->sval[1] = 'x';
			for (i = 0; i < hex_bytes; i++) {
				unsigned char c = (unsigned char)src[i];
				p->sval[used++] = hexdig[c >> 4];
				p->sval[used++] = hexdig[c & 0x0F];
			}
			p->sval[used] = '\0';
			if (hex_bytes < src_len) {
				snprintf(suffix, sizeof(suffix), "…(%llu bytes)",
					(unsigned long long)src_len);
				mre_copy_str(p->sval + used,
					sizeof(p->sval) - used, suffix, strlen(suffix));
			}
			return;
		}
		default:
			break;
	}

	/* VAR_STRING / STRING / DECIMAL / unknown: render by the value's own
	 * type so the literal matches what mysqlnd serializes */
	if (Z_TYPE_P(zv) == IS_LONG) {
		p->kind = MRE_PARAM_LONG;
		p->lval = Z_LVAL_P(zv);
		return;
	}
	if (Z_TYPE_P(zv) == IS_DOUBLE) {
		p->kind = MRE_PARAM_DOUBLE;
		p->dval = Z_DVAL_P(zv);
		return;
	}
	if (Z_TYPE_P(zv) == IS_TRUE) {
		p->kind = MRE_PARAM_LONG;
		p->lval = 1;
		return;
	}
	if (Z_TYPE_P(zv) == IS_FALSE) {
		p->kind = MRE_PARAM_LONG;
		p->lval = 0;
		return;
	}
	p->kind = MRE_PARAM_STR;
	if (Z_TYPE_P(zv) == IS_STRING) {
		mre_param_text_truncate(p->sval, sizeof(p->sval),
			Z_STRVAL_P(zv), Z_STRLEN_P(zv));
		return;
	}
	{
		zend_string *zs = zval_get_string(zv);
		mre_param_text_truncate(p->sval, sizeof(p->sval),
			ZSTR_VAL(zs), ZSTR_LEN(zs));
		zend_string_release(zs);
	}
}

/* capture the statement's bound parameters (call before executing)     */
static void mre_prepared_capture(mre_prepared_info *pi,
                                 const MYSQLND_STMT_DATA *d,
                                 const char *sql, size_t sql_len)
{
	unsigned int i;
	size_t o = 0, j;
	char lit[MRE_PARAM_TEXT_MAX + 40];
	size_t lit_len, p = 0;

	memset(pi, 0, sizeof(*pi));
	pi->param_total = d->param_count;
	if (!d->param_bind || d->param_count == 0) {
		return; /* nothing bound: prepared/sql_full stay omitted */
	}
	pi->has_prepared = (zend_bool)1;
	pi->param_cap = (d->param_count < MRE_PARAM_MAX)
		? d->param_count : MRE_PARAM_MAX;
	for (i = 0; i < pi->param_cap; i++) {
		mre_param_capture(&pi->params[i], &d->param_bind[i]);
	}

	/* sql_full: replace each '?' with the captured literal.  Best-effort
	 * textual substitution: '?' inside quoted strings/comments is not
	 * recognized (the server, not the text, defines placeholders); more
	 * '?' than captured params -> the tail stays as '?'. */
	for (j = 0; j < sql_len && o + 1 < sizeof(pi->sql_full); j++) {
		if (sql[j] != '?' || p >= pi->param_cap) {
			pi->sql_full[o++] = sql[j];
			continue;
		}
		mre_param_sql_literal(&pi->params[p], lit, sizeof(lit));
		p++;
		lit_len = strlen(lit);
		if (o + lit_len >= sizeof(pi->sql_full)) {
			break; /* out of room: sql_full stays truncated */
		}
		memcpy(pi->sql_full + o, lit, lit_len);
		o += lit_len;
	}
	pi->sql_full[o] = '\0';
	pi->has_sql_full = (zend_bool)1;
}

/* move a captured bind state into the log extra (before writing)       */
static void mre_extra_set_prepared(mre_extra *e, const mre_prepared_info *pi)
{
	if (!pi->has_prepared) {
		return;
	}
	e->has_prepared = (zend_bool)1;
	e->param_total = pi->param_total;
	e->param_cap = pi->param_cap;
	memcpy(e->params, pi->params, sizeof(mre_param) * pi->param_cap);
	e->has_sql_full = pi->has_sql_full;
	memcpy(e->sql_full, pi->sql_full, sizeof(e->sql_full));
}

/* ------------------------------------------------------------------ */
/* row sampling: res::fetch_into instance hook (forward decl)           */
/* ------------------------------------------------------------------ */
#if PHP_VERSION_ID < 80100
static void mre_hook_fetch_into(MYSQLND_RES *result, const unsigned int flags,
                                zval *return_value, enum_mysqlnd_extension ext
                                ZEND_FILE_LINE_DC)
#else
static void mre_hook_fetch_into(MYSQLND_RES *result, const unsigned int flags,
                                zval *return_value
                                ZEND_FILE_LINE_DC)
#endif
{
#if PHP_VERSION_ID < 80100
	mre_orig_fetch_into(result, flags, return_value, ext ZEND_FILE_LINE_CC);
#else
	mre_orig_fetch_into(result, flags, return_value ZEND_FILE_LINE_CC);
#endif
	mre_mysql_sample_row(result, return_value);
}

/* 8.1+: PDO fetches rows through m.fetch_row (zval** row) - wrap it too.
 * The definition must stay behind the same version guard as its forward
 * declaration and its assignment in mre_patch_result(): on 7.4/8.0 the
 * mysqlnd fetch_row prototype still uses `void *param, zend_bool *` (the
 * `zval **row, bool *` shape only exists on 8.1+, where Zend finally
 * provides the `bool` type).  Compiling this body unguarded broke the
 * 7.4/8.0 build with "unknown type name 'bool'" (container bug BUG-1). */
#if PHP_VERSION_ID >= 80100
static enum_func_status mre_hook_fetch_row(MYSQLND_RES *result, zval **row,
                                           const unsigned int flags,
                                           bool *fetched_anything)
{
	enum_func_status ret;
	ret = mre_orig_fetch_row(result, row, flags, fetched_anything);

	if (PASS == ret && fetched_anything && *fetched_anything) {
		mre_mysql_sample_row(result, row ? *row : NULL);
	}
	return ret;
}
#endif /* PHP_VERSION_ID >= 80100 */

/* ------------------------------------------------------------------ */
/* row sampling: instance hooks on the result's embedded method table   */
/* ------------------------------------------------------------------ */
#if PHP_VERSION_ID < 80100
static void mre_hook_fetch_into(MYSQLND_RES *result, const unsigned int flags,
                                zval *return_value, enum_mysqlnd_extension ext
                                ZEND_FILE_LINE_DC);
#else
static void mre_hook_fetch_into(MYSQLND_RES *result, const unsigned int flags,
                                zval *return_value
                                ZEND_FILE_LINE_DC);
static enum_func_status mre_hook_fetch_row(MYSQLND_RES *result, zval **row,
                                           const unsigned int flags,
                                           bool *fetched_anything);
#endif

static void mre_patch_result(MYSQLND_RES *res)
{
	if (res) {
		res->m.fetch_into = mre_hook_fetch_into;
#if PHP_VERSION_ID >= 80100
		/* 8.1+: every m.fetch_row() caller passes a zval** row pointer,
		 * so wrapping it is safe and covers PDO's fetch path too */
		res->m.fetch_row = mre_hook_fetch_row;
#endif
	}
}

static void mre_log_result_meta(MYSQLND_CONN_DATA *conn, MYSQLND_RES *res,
                                zend_bool unbuffered)
{
	mre_extra e;
	mre_mysql_ctx *ctx;

	mre_extra_conn(&e, conn);
	if (res) {
		e.has_fields = (zend_bool)1;
		e.fields = res->field_count;
		if (res->stored_data) {
			e.has_rows = (zend_bool)1;
			e.rows = res->m.num_rows(res);
		}
	}
	ctx = mre_ctx_get(conn);
	if (ctx) {
		ctx->rows_total = e.has_rows ? e.rows : 0;
	}
	if (unbuffered) {
		e.truncated = (zend_bool)1; /* row count unknown until drained */
	}
	mre_log_write(MRE_TYPE_MYSQL, MRE_DIR_RESP, "result", sizeof("result") - 1, &e);
}

/* shared row formatter: consumes one sample budget unit and logs the row */
static void mre_mysql_log_row(MYSQLND_CONN_DATA *conn, mre_mysql_ctx *ctx,
                              zval *row)
{
	mre_extra e;
	char rowdump[MRE_ROW_DUMP_MAX + 16];
	size_t rd = 0;
	zend_ulong cell_idx = 0;
	zval *zv;

	ctx->samples_left--;
	ctx->fetched++;

	rowdump[0] = '\0';
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(row), zv) {
		zend_string *zs;
		char cell[MRE_ROW_CELL_MAX + 4];
		size_t cl;

		if (cell_idx >= MRE_ROW_CELLS_MAX) {
			rd += (size_t)snprintf(rowdump + rd, sizeof(rowdump) - rd, "|...");
			break;
		}
		if (cell_idx > 0 && rd + 1 < sizeof(rowdump)) {
			rowdump[rd++] = '|';
			rowdump[rd] = '\0';
		}
		switch (Z_TYPE_P(zv)) {
			case IS_STRING:
				zs = Z_STR_P(zv);
				mre_copy_str(cell, sizeof(cell), ZSTR_VAL(zs), ZSTR_LEN(zs));
				break;
			case IS_NULL:
				cell[0] = '\\'; cell[1] = 'N'; cell[2] = '\0';
				break;
			case IS_TRUE:
				cell[0] = '1'; cell[1] = '\0';
				break;
			case IS_FALSE:
				cell[0] = '0'; cell[1] = '\0';
				break;
			case IS_LONG:
				snprintf(cell, sizeof(cell), ZEND_LONG_FMT, Z_LVAL_P(zv));
				break;
			case IS_DOUBLE:
				snprintf(cell, sizeof(cell), "%f", Z_DVAL_P(zv));
				break;
			default:
				zs = zval_get_string(zv);
				mre_copy_str(cell, sizeof(cell), ZSTR_VAL(zs), ZSTR_LEN(zs));
				zend_string_release(zs);
				break;
		}
		cl = strlen(cell);
		if (rd + cl >= sizeof(rowdump) - 4) {
			mre_copy_str(rowdump + rd, sizeof(rowdump) - rd, "...", 3);
			rd = strlen(rowdump);
			break;
		}
		memcpy(rowdump + rd, cell, cl);
		rd += cl;
		rowdump[rd] = '\0';
		cell_idx++;
	} ZEND_HASH_FOREACH_END();

	mre_extra_conn(&e, conn);
	e.has_fetched = (zend_bool)1;
	e.fetched = ctx->fetched;
	if (ctx->rows_total > 0) {
		e.has_rows = (zend_bool)1;
		e.rows = ctx->rows_total;
	}
	e.truncated = (zend_bool)(cell_idx >= MRE_ROW_CELLS_MAX
		|| rd >= sizeof(rowdump) - 8);
	mre_log_write(MRE_TYPE_MYSQL, MRE_DIR_RESP, rowdump, rd, &e);
}

/* shared row sampler: format + log one fetched row */
static void mre_mysql_sample_row(MYSQLND_RES *result, zval *row)
{
	mre_mysql_ctx *ctx;

	if (!result || !result->conn || !mre_mysql_enabled()) {
		return;
	}
	if (!row || Z_TYPE_P(row) != IS_ARRAY) {
		return;
	}
	if (zend_hash_num_elements(Z_ARRVAL_P(row)) == 0) {
		return;
	}
	ctx = mre_ctx_get(result->conn);
	if (!ctx || !ctx->sampling || ctx->samples_left <= 0) {
		return;
	}
	mre_mysql_log_row(result->conn, ctx, row);
}

/* row sampler for the stmt bind_result()+fetch() path: the fetched values
 * land in the bound userland variables (stmt->data->result_bind), not in a
 * zval array this version's m.fetch_row prototype would hand us, so copy
 * them into a temporary row array and run the shared sampler */
static void mre_mysql_sample_stmt_row(MYSQLND_STMT * const stmt)
{
	MYSQLND_STMT_DATA *d = stmt ? stmt->data : NULL;
	mre_mysql_ctx *ctx;
	zval row;
	unsigned int i;

	if (!d || !d->conn || !d->result_bind || d->field_count == 0) {
		return; /* no bind_result(): nothing to sample here */
	}
	if (!mre_mysql_enabled()) {
		return;
	}
	ctx = mre_ctx_get(d->conn);
	if (!ctx || !ctx->sampling || ctx->samples_left <= 0) {
		return;
	}

	array_init(&row);
	for (i = 0; i < d->field_count; i++) {
		zval *zv = &d->result_bind[i].zv;

		if (Z_TYPE_P(zv) == IS_REFERENCE) {
			zv = Z_REFVAL_P(zv);
		}
		if (!zv || Z_TYPE_P(zv) == IS_UNDEF) {
			continue;
		}
		Z_TRY_ADDREF_P(zv);
		add_next_index_zval(&row, zv);
	}
	mre_mysql_log_row(d->conn, ctx, &row);
	zval_ptr_dtor(&row);
}

/* ------------------------------------------------------------------ */
/* conn_data hooks                                                      */
/* ------------------------------------------------------------------ */
static enum_func_status mre_hook_query(MYSQLND_CONN_DATA * conn,
                                       const char * const query,
                                       const size_t query_len)
{
	enum_func_status ret;
	mre_mysql_ctx *ctx = mre_ctx_get(conn);
	int qkind = mre_gtx_sql_kind(query, query_len);
	zend_bool gtx_on = mre_gtx_enabled();
	zend_bool log_on = mre_mysql_enabled();
	double mono_start = 0.0;
	char time_start[40];

	/* start clock before the statement reaches the server; the req
	 * line itself is written after the call returned (it carries the
	 * measured duration), with time_start overriding its top-level
	 * "time" so the semantic start moment is preserved */
	if (log_on) {
		mono_start = mre_clock_mono_ms();
		mre_iso8601_now(time_start, sizeof(time_start));
	}

	/* stage B: transaction control interception happens BEFORE the
	 * statement reaches the server */
	if (gtx_on && ctx
			&& mre_gtx_pre_call(conn, &ctx->gtx, query, query_len, qkind)) {
		/* COMMIT intercepted and deferred: report success, tx stays open */
		if (log_on) {
			mre_extra e;

			mre_extra_conn(&e, conn);
			e.has_cmd = (zend_bool)1;
			mre_copy_str(e.cmd, sizeof(e.cmd), "query", 5);
			/* nothing was sent: time_end == time, cost 0 */
			mre_extra_timing(&e, time_start, time_start, 0.0);
			mre_log_write(MRE_TYPE_MYSQL, MRE_DIR_REQ, query,
				MIN(query_len, (size_t)(MRE_MAX_SQL_LOG - 1)), &e);

			mre_extra_conn(&e, conn);
			e.has_affected = (zend_bool)1;
			e.affected = 0;
			mre_log_write(MRE_TYPE_MYSQL, MRE_DIR_RESP,
				"GTX_COMMIT_DEFERRED", sizeof("GTX_COMMIT_DEFERRED") - 1, &e);
		}
		return PASS;
	}

	ret = mre_orig_query(conn, query, query_len);

	if (log_on && query) {
		mre_extra e;
		char time_end[40];

		mre_iso8601_now(time_end, sizeof(time_end));

		mre_extra_conn(&e, conn);
		e.has_cmd = (zend_bool)1;
		mre_copy_str(e.cmd, sizeof(e.cmd), "query", 5);
		mre_extra_timing(&e, time_start, time_end,
			mre_clock_mono_ms() - mono_start);
		mre_log_write(MRE_TYPE_MYSQL, MRE_DIR_REQ, query,
			MIN(query_len, (size_t)(MRE_MAX_SQL_LOG - 1)), &e);

		if (ctx) {
			mre_ctx_remember_sql(ctx, query, query_len);
		}
		mre_extra_conn(&e, conn);
		mre_extra_error(&e, conn->error_info);
		if (e.has_error) {
			mre_log_write(MRE_TYPE_MYSQL, MRE_DIR_RESP, "ERROR",
				sizeof("ERROR") - 1, &e);
		} else {
			uint64_t affected = mre_affected_rows(conn);
			if (mre_has_affected(affected)) {
				e.has_affected = (zend_bool)1;
				e.affected = affected;
			}
			mre_log_write(MRE_TYPE_MYSQL, MRE_DIR_RESP, "OK",
				sizeof("OK") - 1, &e);
		}
	}

	if (gtx_on && ctx) {
		mre_gtx_post_call(conn, &ctx->gtx, (zend_bool)(ret == PASS),
			query, qkind);
	}
	return ret;
}

#if PHP_VERSION_ID < 80100
static MYSQLND_RES *mre_hook_store_result(MYSQLND_CONN_DATA * const conn,
                                          const unsigned int flags)
{
	MYSQLND_RES *res = mre_orig_store_result(conn, flags);
#else
static MYSQLND_RES *mre_hook_store_result(MYSQLND_CONN_DATA * const conn)
{
	MYSQLND_RES *res = mre_orig_store_result(conn);
#endif
	if (res && mre_mysql_enabled()) {
		mre_patch_result(res);
		mre_log_result_meta(conn, res, (zend_bool)0);
	}
	return res;
}

#if PHP_VERSION_ID < 80100
static MYSQLND_RES *mre_hook_use_result(MYSQLND_CONN_DATA * const conn,
                                        const unsigned int flags)
{
	MYSQLND_RES *res = mre_orig_use_result(conn, flags);
#else
static MYSQLND_RES *mre_hook_use_result(MYSQLND_CONN_DATA * const conn)
{
	MYSQLND_RES *res = mre_orig_use_result(conn);
#endif
	if (res && mre_mysql_enabled()) {
		mre_patch_result(res);
		mre_log_result_meta(conn, res, (zend_bool)1);
	}
	return res;
}

static enum_func_status mre_hook_next_result(MYSQLND_CONN_DATA * const conn)
{
	enum_func_status ret = mre_orig_next_result(conn);

	if (mre_mysql_enabled() && conn) {
		mre_extra e;

		mre_extra_conn(&e, conn);
		mre_extra_error(&e, conn->error_info);
		if (!e.has_error) {
			e.has_affected = (zend_bool)1;
			e.affected = mre_affected_rows(conn);
		}
		mre_log_write(MRE_TYPE_MYSQL, MRE_DIR_RESP, "next_result",
			sizeof("next_result") - 1, &e);
	}
	return ret;
}

static void mre_hook_dtor(MYSQLND_CONN_DATA * conn)
{
	if (mre_plugin_id != (unsigned int)-1 && conn) {
		mre_mysql_ctx **slot =
			(mre_mysql_ctx **)mysqlnd_plugin_get_plugin_connection_data_data(
				conn, mre_plugin_id);
		if (slot && *slot) {
			/* stage B: a tracked connection leaving must leave the window */
			if ((*slot)->gtx.tracked || (*slot)->gtx.pending) {
				mre_gtx_conn_closed(conn, &(*slot)->gtx);
			}
			pefree(*slot, conn->persistent);
			*slot = NULL;
		}
	}
	mre_orig_dtor(conn);
}

/* ------------------------------------------------------------------ */
/* stmt hooks (covers mysqli prepared statements and PDO::prepare)      */
/* ------------------------------------------------------------------ */
static enum_func_status mre_hook_stmt_prepare(MYSQLND_STMT * const stmt,
                                              const char * const query,
                                              const size_t query_len)
{
	enum_func_status ret;
	zend_bool log_on;
	double mono_start = 0.0;
	char time_start[40];

	/* start clock before the COM_STMT_PREPARE roundtrip; same "req line
	 * written after the call" scheme as mre_hook_query */
	log_on = (zend_bool)(mre_mysql_enabled() && stmt && stmt->data && query);
	if (log_on) {
		mono_start = mre_clock_mono_ms();
		mre_iso8601_now(time_start, sizeof(time_start));
	}

	ret = mre_orig_stmt_prepare(stmt, query, query_len);

	if (log_on) {
		MYSQLND_STMT_DATA *d = stmt->data;
		mre_extra e;
		mre_mysql_ctx *ctx = d->conn ? mre_ctx_get(d->conn) : NULL;
		const char *resp_word;
		size_t resp_len;
		char time_end[40];

		mre_iso8601_now(time_end, sizeof(time_end));

		if (ctx) {
			mre_ctx_remember_sql(ctx, query, query_len);
		}
		mre_extra_conn(&e, d->conn);
		e.has_stmt = (zend_bool)1;
		e.stmt_id = d->stmt_id;
		e.has_cmd = (zend_bool)1;
		mre_copy_str(e.cmd, sizeof(e.cmd), "prepare", 7);
		mre_extra_timing(&e, time_start, time_end,
			mre_clock_mono_ms() - mono_start);
		mre_log_write(MRE_TYPE_MYSQL, MRE_DIR_REQ, query,
			MIN(query_len, (size_t)(MRE_MAX_SQL_LOG - 1)), &e);

		mre_extra_conn(&e, d->conn);
		mre_extra_error(&e, d->error_info);
		e.has_stmt = (zend_bool)1;
		e.stmt_id = d->stmt_id;
		if (!e.has_error) {
			e.has_fields = (zend_bool)1;
			e.fields = d->field_count;
		}
		resp_word = e.has_error ? "ERROR" : "PREPARED";
		resp_len = e.has_error ? sizeof("ERROR") - 1 : sizeof("PREPARED") - 1;
		mre_log_write(MRE_TYPE_MYSQL, MRE_DIR_RESP, resp_word, resp_len, &e);
	}
	return ret;
}

static MYSQLND_RES *mre_hook_stmt_get_result(MYSQLND_STMT * const stmt)
{
	MYSQLND_RES *res = mre_orig_stmt_get_result(stmt);

	if (res && stmt && stmt->data && mre_mysql_enabled()) {
		MYSQLND_STMT_DATA *d = stmt->data;
		mre_mysql_ctx *ctx = d->conn ? mre_ctx_get(d->conn) : NULL;

		if (ctx) {
			ctx->samples_left = MRE_G(cfg).sample_rows;
			ctx->sampling = (zend_bool)(ctx->samples_left > 0);
			ctx->fetched = 0;
			ctx->rows_total = res->stored_data ? res->m.num_rows(res) : 0;
		}
		mre_patch_result(res);
	}
	return res;
}

/* bind_result()+fetch(): on every version this path goes through the stmt
 * method table's fetch() (not through res::fetch_into / m.fetch_row with a
 * usable row array), so sample the bound variables here */
static enum_func_status mre_hook_stmt_fetch(MYSQLND_STMT * const stmt,
                                            zend_bool * const fetched_anything)
{
	enum_func_status ret = mre_orig_stmt_fetch(stmt, fetched_anything);

	if (PASS == ret && fetched_anything && *fetched_anything) {
		mre_mysql_sample_stmt_row(stmt);
	}
	return ret;
}

static enum_func_status mre_hook_stmt_execute(MYSQLND_STMT * const stmt)
{
	enum_func_status ret;
	MYSQLND_STMT_DATA *d;
	MYSQLND_CONN_DATA *conn;
	mre_mysql_ctx *ctx;
	mre_prepared_info pi;
	zend_bool have_pi = (zend_bool)0;
	int qkind = MRE_GTX_Q_NONE;
	zend_bool gtx_on = mre_gtx_enabled();
	zend_bool log_on = mre_mysql_enabled();
	double mono_start = 0.0;
	char time_start[40];

	if (log_on) {
		/* wall start time at hook entry: the deferred-COMMIT branch
		 * (nothing sent) needs it as both "time" and "time_end" */
		mre_iso8601_now(time_start, sizeof(time_start));
	}

	if (stmt && stmt->data) {
		d = stmt->data;
		conn = d->conn;
		ctx = conn ? mre_ctx_get(conn) : NULL;
		qkind = ctx ? mre_gtx_sql_kind(ctx->sql, ctx->sql_len)
		            : MRE_GTX_Q_NONE;

		if (gtx_on && ctx && qkind != MRE_GTX_Q_NONE
				&& mre_gtx_pre_call(conn, &ctx->gtx, ctx->sql, ctx->sql_len,
					qkind)) {
			/* deferred COMMIT via prepared statement: report success */
			if (log_on) {
				mre_extra e;

				mre_extra_conn(&e, conn);
				e.has_stmt = (zend_bool)1;
				e.stmt_id = d->stmt_id;
				e.has_cmd = (zend_bool)1;
				mre_copy_str(e.cmd, sizeof(e.cmd), "query", 5);
				/* nothing was sent: time_end == time, cost 0 */
				mre_extra_timing(&e, time_start, time_start, 0.0);
				mre_log_write(MRE_TYPE_MYSQL, MRE_DIR_REQ, "COMMIT",
					sizeof("COMMIT") - 1, &e);

				mre_extra_conn(&e, conn);
				e.has_stmt = (zend_bool)1;
				e.stmt_id = d->stmt_id;
				e.has_affected = (zend_bool)1;
				e.affected = 0;
				mre_log_write(MRE_TYPE_MYSQL, MRE_DIR_RESP,
					"GTX_COMMIT_DEFERRED",
					sizeof("GTX_COMMIT_DEFERRED") - 1, &e);
			}
			return PASS;
		}
		/* bind values must be read BEFORE the original execute touches
		 * the statement - they are exactly what is about to be sent */
		if (mre_mysql_enabled()) {
			mre_prepared_capture(&pi, d, ctx ? ctx->sql : "",
				ctx ? ctx->sql_len : 0);
			have_pi = (zend_bool)1;
		}
		/* start the duration clock right before the roundtrip */
		if (log_on) {
			mono_start = mre_clock_mono_ms();
		}
	} else {
		d = NULL;
		conn = NULL;
		ctx = NULL;
	}

	ret = mre_orig_stmt_execute(stmt);

	if (log_on && stmt && stmt->data) {
		MYSQLND_STMT_DATA *d = stmt->data;
		MYSQLND_CONN_DATA *conn = d->conn;
		mre_mysql_ctx *ctx = conn ? mre_ctx_get(conn) : NULL;
		mre_extra e;
		const char *content;
		size_t clen = 0;
		const char *resp_word;
		size_t resp_len;
		char time_end[40];

		mre_iso8601_now(time_end, sizeof(time_end));

		/* pure SQL only: the prepare-time statement itself, no prefix.
		 * The "this is an execute" information lives in extra.cmd. */
		content = ctx ? ctx->sql : "";
		clen = ctx ? ctx->sql_len : 0;

		mre_extra_conn(&e, conn);
		e.has_stmt = (zend_bool)1;
		e.stmt_id = d->stmt_id;
		e.has_cmd = (zend_bool)1;
		mre_copy_str(e.cmd, sizeof(e.cmd), "execute", 7);
		mre_extra_timing(&e, time_start, time_end,
			mre_clock_mono_ms() - mono_start);
		if (have_pi) {
			mre_extra_set_prepared(&e, &pi);
		}
		mre_log_write(MRE_TYPE_MYSQL, MRE_DIR_REQ, content, clen, &e);

		/* enable row sampling on the statement result set */
		if (ctx && d->result && ret == PASS) {
			ctx->samples_left = MRE_G(cfg).sample_rows;
			ctx->sampling = (zend_bool)(ctx->samples_left > 0);
			ctx->fetched = 0;
			ctx->rows_total = d->result->stored_data
				? d->result->m.num_rows(d->result) : 0;
			mre_patch_result(d->result);
		}

		mre_extra_conn(&e, conn);
		mre_extra_error(&e, d->error_info);
		e.has_stmt = (zend_bool)1;
		e.stmt_id = d->stmt_id;
		if (!e.has_error) {
			uint64_t affected =
				(d->upsert_status ? d->upsert_status->affected_rows : 0);
			if (mre_has_affected(affected)) {
				e.has_affected = (zend_bool)1;
				e.affected = affected;
			}
			e.has_fields = (zend_bool)1;
			e.fields = d->field_count;
		}
		resp_word = e.has_error ? "ERROR" : "EXECUTED";
		resp_len = e.has_error ? sizeof("ERROR") - 1 : sizeof("EXECUTED") - 1;
		mre_log_write(MRE_TYPE_MYSQL, MRE_DIR_RESP, resp_word, resp_len, &e);
	}

	if (gtx_on && ctx) {
		mre_gtx_post_call(conn, &ctx->gtx, (zend_bool)(ret == PASS),
			ctx->sql, qkind);
	}
	return ret;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                            */
/* ------------------------------------------------------------------ */
void mre_mysql_minit(void)
{
	{
		MYSQLND_CLASS_METHODS_TYPE(mysqlnd_conn_data) *cur =
			mysqlnd_conn_data_get_methods();

		mre_conn_methods = *cur;
		mre_conn_methods_orig = *cur;
		mre_orig_query = cur->query;
		mre_orig_store_result = cur->store_result;
		mre_orig_use_result = cur->use_result;
		mre_orig_next_result = cur->next_result;
		mre_orig_dtor = cur->dtor;

		mre_conn_methods.query = mre_hook_query;
		mre_conn_methods.store_result = mre_hook_store_result;
		mre_conn_methods.use_result = mre_hook_use_result;
		mre_conn_methods.next_result = mre_hook_next_result;
		mre_conn_methods.dtor = mre_hook_dtor;

		mysqlnd_conn_data_set_methods(&mre_conn_methods);
	}
	{
		MYSQLND_CLASS_METHODS_TYPE(mysqlnd_stmt) *cur =
			mysqlnd_stmt_get_methods();

		mre_stmt_methods = *cur;
		mre_stmt_methods_orig = *cur;
		mre_orig_stmt_prepare = cur->prepare;
		mre_orig_stmt_execute = cur->execute;
		mre_orig_stmt_get_result = cur->get_result;
		mre_orig_stmt_fetch = cur->fetch;

		mre_stmt_methods.prepare = mre_hook_stmt_prepare;
		mre_stmt_methods.execute = mre_hook_stmt_execute;
		mre_stmt_methods.get_result = mre_hook_stmt_get_result;
		mre_stmt_methods.fetch = mre_hook_stmt_fetch;

		mysqlnd_stmt_set_methods(&mre_stmt_methods);
	}
	mre_orig_fetch_into = mysqlnd_result_get_methods()->fetch_into;
	mre_orig_fetch_row = mysqlnd_result_get_methods()->fetch_row;

	mre_plugin_id = mysqlnd_plugin_register();
}

void mre_mysql_mshutdown(void)
{
	/* restore only when nobody chained on top of us */
	if (mysqlnd_conn_data_get_methods() == &mre_conn_methods) {
		mysqlnd_conn_data_set_methods(&mre_conn_methods_orig);
	}
	if (mysqlnd_stmt_get_methods() == &mre_stmt_methods) {
		mysqlnd_stmt_set_methods(&mre_stmt_methods_orig);
	}
	mre_plugin_id = (unsigned int)-1;
}

void mre_mysql_reassert(void)
{
	/* another mysqlnd plugin may have replaced the tables after MINIT */
	if (mysqlnd_conn_data_get_methods() != &mre_conn_methods) {
		mysqlnd_conn_data_set_methods(&mre_conn_methods);
	}
	if (mysqlnd_stmt_get_methods() != &mre_stmt_methods) {
		mysqlnd_stmt_set_methods(&mre_stmt_methods);
	}
}

#endif /* MRE_HAVE_MYSQLND */
