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
	char     sql[MRE_MAX_SQL_LOG]; /* last executed SQL (for EXECUTE etc.) */
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

/* "EXECUTE <last sql>" content for stmt execute logs */
static void mre_stmt_content(char *buf, size_t cap, size_t *len,
                             const mre_mysql_ctx *ctx)
{
	*len = 0;
	mre_copy_str(buf, cap, "EXECUTE", sizeof("EXECUTE") - 1);
	*len = strlen(buf);
	if (ctx && ctx->sql_len > 0 && *len + 1 < cap) {
		buf[(*len)++] = ' ';
		mre_copy_str(buf + *len, cap - *len, ctx->sql, ctx->sql_len);
		*len = strlen(buf);
	}
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

/* 8.1+: PDO fetches rows through m.fetch_row (zval** row) - wrap it too */
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

/* shared row sampler: format + log one fetched row */
static void mre_mysql_sample_row(MYSQLND_RES *result, zval *row)
{
	mre_mysql_ctx *ctx;
	mre_extra e;
	char rowdump[MRE_ROW_DUMP_MAX + 16];
	size_t rd = 0;
	zend_ulong cell_idx = 0;
	zval *zv;

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

	mre_extra_conn(&e, result->conn);
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

	/* stage B: transaction control interception happens BEFORE the
	 * statement reaches the server */
	if (gtx_on && ctx
			&& mre_gtx_pre_call(conn, &ctx->gtx, query, query_len, qkind)) {
		/* COMMIT intercepted and deferred: report success, tx stays open */
		if (mre_mysql_enabled()) {
			mre_extra e;

			mre_extra_conn(&e, conn);
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

	if (mre_mysql_enabled() && query) {
		mre_extra e;

		mre_extra_conn(&e, conn);
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
	enum_func_status ret = mre_orig_stmt_prepare(stmt, query, query_len);

	if (mre_mysql_enabled() && stmt && stmt->data && query) {
		MYSQLND_STMT_DATA *d = stmt->data;
		mre_extra e;
		mre_mysql_ctx *ctx = d->conn ? mre_ctx_get(d->conn) : NULL;
		const char *resp_word;
		size_t resp_len;

		if (ctx) {
			mre_ctx_remember_sql(ctx, query, query_len);
		}
		mre_extra_conn(&e, d->conn);
		e.has_stmt = (zend_bool)1;
		e.stmt_id = d->stmt_id;
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

static enum_func_status mre_hook_stmt_execute(MYSQLND_STMT * const stmt)
{
	enum_func_status ret;
	MYSQLND_STMT_DATA *d;
	MYSQLND_CONN_DATA *conn;
	mre_mysql_ctx *ctx;
	int qkind = MRE_GTX_Q_NONE;
	zend_bool gtx_on = mre_gtx_enabled();

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
			if (mre_mysql_enabled()) {
				mre_extra e;

				mre_extra_conn(&e, conn);
				e.has_stmt = (zend_bool)1;
				e.stmt_id = d->stmt_id;
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
	} else {
		d = NULL;
		conn = NULL;
		ctx = NULL;
	}

	ret = mre_orig_stmt_execute(stmt);

	if (mre_mysql_enabled() && stmt && stmt->data) {
		MYSQLND_STMT_DATA *d = stmt->data;
		MYSQLND_CONN_DATA *conn = d->conn;
		mre_mysql_ctx *ctx = conn ? mre_ctx_get(conn) : NULL;
		mre_extra e;
		char content[MRE_MAX_SQL_LOG + 16];
		size_t clen = 0;
		const char *resp_word;
		size_t resp_len;

		mre_stmt_content(content, sizeof(content), &clen, ctx);
		mre_extra_conn(&e, conn);
		e.has_stmt = (zend_bool)1;
		e.stmt_id = d->stmt_id;
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

		mre_stmt_methods.prepare = mre_hook_stmt_prepare;
		mre_stmt_methods.execute = mre_hook_stmt_execute;
		mre_stmt_methods.get_result = mre_hook_stmt_get_result;

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
