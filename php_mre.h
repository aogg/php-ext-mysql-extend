/*
  +----------------------------------------------------------------------+
  | mre - mysql-redis-extend                                             |
  | Capture MySQL (mysqlnd) and Redis (PHP streams / RESP) traffic.      |
  +----------------------------------------------------------------------+
  | Stage A: core extension skeleton, env/ini config, mysqlnd capture,   |
  | stream RESP sniffing, unified log writer.                            |
  +----------------------------------------------------------------------+
*/

#ifndef PHP_MRE_H
#define PHP_MRE_H

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

/* ------------------------------------------------------------------ */
/* Version                                                             */
/* ------------------------------------------------------------------ */
#define PHP_MRE_VERSION "0.1.0-a1"

/* ------------------------------------------------------------------ */
/* Includes                                                            */
/* ------------------------------------------------------------------ */
#include "php.h"
#include "php_ini.h"

#include <stdio.h>
#include <time.h>

#ifdef PHP_WIN32
#include <direct.h> /* _mkdir */
#else
#include <sys/stat.h>  /* mkdir */
#include <sys/file.h>  /* flock */
#include <unistd.h>    /* fileno */
#endif

/*
 * mysqlnd is part of php-src itself. config.m4 sets HAVE_MRE_MYSQLND when
 * ext/mysqlnd/mysqlnd.h was found. Without it the extension still builds,
 * only MySQL capture is compiled out.
 */
#if defined(HAVE_MRE_MYSQLND)
# define MRE_HAVE_MYSQLND 1
# include "ext/mysqlnd/mysqlnd.h"
# include "ext/mysqlnd/mysqlnd_ext_plugin.h"
#else
# define MRE_HAVE_MYSQLND 0
#endif

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */
#define MRE_PATH_MAX        1024  /* config path values                */
#define MRE_REQUEST_ID_MAX  64    /* "<pid>-<seq>", incl. NUL          */
#define MRE_LOG_LINE_MAX    16384 /* one log line, stack allocated     */
#define MRE_JSON_VAL_MAX    8192  /* escaped content cap inside a line */
#define MRE_EXTRA_KV_MAX    12    /* max extra key/value pairs         */
#define MRE_EXTRA_VAL_MAX   560   /* max rendered value per extra item */

#define MRE_MAX_SQL_LOG     4096  /* SQL text cap in logs              */
#define MRE_PARAM_MAX       32    /* max captured bind params          */
#define MRE_PARAM_TEXT_MAX  256   /* one bind param value cap          */
#define MRE_PARAMS_FRAG_MAX 2048  /* params json/text fragment budget  */
#define MRE_ROW_CELL_MAX    64    /* one sampled row cell cap          */
#define MRE_ROW_CELLS_MAX   16    /* max cells sampled per row         */
#define MRE_ROW_DUMP_MAX    512   /* sampled row total cap             */

#define MRE_REDIS_CMD_MAX   2048  /* redis command dump cap            */
#define MRE_REDIS_ARG_MAX   256   /* one redis arg dump cap            */
#define MRE_REDIS_REPLY_MAX 512   /* redis reply dump cap              */

/* log record type / dir values */
#define MRE_TYPE_MYSQL  "mysql"
#define MRE_TYPE_REDIS  "redis"
#define MRE_DIR_REQ     "req"
#define MRE_DIR_RESP    "resp"

/* ------------------------------------------------------------------ */
/* Config (resolved per request: env MRE_* wins over ini mre.*)        */
/* ------------------------------------------------------------------ */
typedef struct {
	zend_bool enable;                              /* MRE_ENABLE       */
	zend_bool capture_mysql;                       /* MRE_CAPTURE_MYSQL*/
	zend_bool capture_redis;                       /* MRE_CAPTURE_REDIS*/
	zend_bool gtx;                                 /* MRE_GTX          */
	int       json_format;                         /* MRE_LOG_FORMAT   */
	int       redis_port;                          /* MRE_REDIS_PORT   */
	int       sample_rows;                         /* MRE_SAMPLE_ROWS  */
	int       gtx_timeout;                         /* MRE_GTX_TIMEOUT  */
	char      log_file[MRE_PATH_MAX];              /* MRE_LOG_FILE     */
	char      log_script[MRE_PATH_MAX];            /* MRE_LOG_SCRIPT   */
	char      state_dir[MRE_PATH_MAX];             /* MRE_STATE_DIR    */
	char      yaml_config[MRE_PATH_MAX];           /* MRE_YAML_CONFIG  */
} mre_config;

void mre_config_refresh(void);      /* env > ini into MRE_G(cfg) */

/* ------------------------------------------------------------------ */
/* Extra log fields (rendered into "extra" json object / k=v in text)  */
/* ------------------------------------------------------------------ */

/* one captured bind parameter (prepared statements).  `sval` holds the
 * value text for strings ("raw, truncated with a "...(N)" suffix baked
 * in) and blobs ("0x<hex>" summary, truncated with "...(N bytes)");    */
typedef enum {
	MRE_PARAM_NULL = 0,
	MRE_PARAM_LONG,
	MRE_PARAM_DOUBLE,
	MRE_PARAM_STR,
	MRE_PARAM_BLOB
} mre_param_kind;

typedef struct {
	mre_param_kind kind;
	zend_long      lval;
	double         dval;
	char           sval[MRE_PARAM_TEXT_MAX + 32];
} mre_param;

typedef struct {
	zend_bool has_affected;
	zend_bool has_rows;       /* rows in result set (buffered)        */
	zend_bool has_fetched;    /* rows sampled/fetched                 */
	zend_bool has_fields;
	zend_bool has_error;
	zend_bool has_host;
	zend_bool has_port;
	zend_bool has_stmt;
	zend_bool truncated;
	zend_bool has_cmd;        /* mysql req only: query/prepare/execute */
	char      cmd[10];
	zend_bool has_time_start; /* override the line's top-level "time":
	                           * mysql req lines are written after the
	                           * call finished, so they must carry the
	                           * wall time captured at call start       */
	char      time_start[40];
	zend_bool has_time_end;   /* extra.time_end: response wall time     */
	char      time_end[40];
	zend_bool has_cost;       /* extra.cost_ms: duration in ms          */
	double    cost_ms;
	zend_bool has_prepared;   /* render extra.prepared {sql, params}  */
	unsigned  param_total;    /* bound params reported by mysqlnd     */
	unsigned  param_cap;      /* params captured (<= MRE_PARAM_MAX)   */
	mre_param params[MRE_PARAM_MAX];
	zend_bool has_sql_full;   /* extra.sql_full (? -> literals)       */
	char      sql_full[MRE_MAX_SQL_LOG];
	uint64_t  affected;
	uint64_t  rows;
	uint64_t  fetched;
	uint64_t  fields;
	uint64_t  err_no;
	uint64_t  stmt_id;
	int       port;
	char      err_msg[512];
	char      sqlstate[8];
	char      host[160];
} mre_extra;

void mre_extra_init(mre_extra *e);

/* ------------------------------------------------------------------ */
/* Route resolution (stage B): which file receives one record           */
/* Priority: log_script return > yaml rule > mre.log_file               */
/* ------------------------------------------------------------------ */
zend_bool mre_route_resolve(const char *type, const char *dir,
                            const char *content, size_t content_len,
                            const mre_extra *extra,
                            char *out, size_t out_cap);

void mre_log_write(const char *type, const char *dir,
                   const char *content, size_t content_len,
                   const mre_extra *extra);
void mre_log_write_main(const char *type, const char *content);
                    /* un-routed record to the main log (warnings) */
void mre_log_close(void);           /* MSHUTDOWN / GSHUTDOWN */

/* ------------------------------------------------------------------ */
/* Mini YAML parser (mre_yaml.c) - built-in, no libyaml                */
/* Supports: nested maps, "- " lists, plain/quoted scalars, comments,  */
/* literal block scalars "|". Inline {} [] is NOT supported.           */
/* ------------------------------------------------------------------ */
typedef enum {
	MRE_YAML_SCALAR = 0,
	MRE_YAML_MAP,
	MRE_YAML_LIST
} mre_yaml_kind;

typedef struct _mre_yaml_node {
	mre_yaml_kind kind;
	char *scalar;                       /* SCALAR */
	char **keys;                        /* MAP keys */
	struct _mre_yaml_node **values;     /* MAP values */
	struct _mre_yaml_node **items;      /* LIST items */
	size_t count;                       /* MAP entries / LIST items */
} mre_yaml_node;

/* parse; returns NULL and fills err (NUL-terminated) on failure */
mre_yaml_node *mre_yaml_parse(const char *data, size_t len,
                              char *err, size_t err_len);
void mre_yaml_free(mre_yaml_node *node);

/* accessors for the next stage (rule engine) */
const mre_yaml_node *mre_yaml_map_get(const mre_yaml_node *map, const char *key);
const char *mre_yaml_scalar(const mre_yaml_node *node);

/* debug helper: expose the currently loaded tree to userland */
zend_bool mre_yaml_dump_to_zval(zval *return_value);

/* yaml config lifecycle (kept in globals, loaded from mre.yaml_config) */
void mre_yaml_config_ensure(void);  /* load / reload when file changed */
void mre_yaml_config_free(void);    /* MSHUTDOWN */

/* ------------------------------------------------------------------ */
/* YAML rule engine (mre_rules.c) - stage B                             */
/* Compiles "rules:" entries to PCRE2 once per yaml (re)load; routes a  */
/* record to the first matching rule's log_file, else default_log_file. */
/* ------------------------------------------------------------------ */
zend_bool mre_rules_sync(void);    /* rebuild when the yaml tree changed */
zend_bool mre_rules_route(const char *type, const char *content,
                          size_t content_len, char *out, size_t cap);
void mre_rules_free(void);         /* MSHUTDOWN */
zend_bool mre_rules_dump_to_zval(zval *rules_out, zval *default_out);
                    /* effective rule list + default_log_file for dump */

/* ------------------------------------------------------------------ */
/* Log-script callback (mre_script.c) - stage B                         */
/* Runs mre.log_script per record; its `return 'path';` selects the     */
/* log file. Any failure degrades to the caller's fallback.             */
/* ------------------------------------------------------------------ */
void mre_script_rinit(void);       /* per request: enable + drop cache  */
void mre_script_rshutdown(void);   /* disable until the next request    */
zend_bool mre_script_route(const char *type, const char *dir,
                           const char *content, size_t content_len,
                           const mre_extra *extra,
                           char *out, size_t cap);

/* ------------------------------------------------------------------ */
/* Cross-fpm global transaction (mre_gtx.c) - stage B                   */
/* ------------------------------------------------------------------ */
/* per connection gtx state, embedded in the mysql connection context   */
typedef struct {
	zend_bool tracked;             /* registered in the open window     */
	zend_bool pending;             /* COMMIT intercepted, deferred      */
	int       epoch;               /* request epoch of last stat check  */
	char      key[128];            /* unique connection key             */
} mre_gtx_ctx;

void mre_gtx_rinit(void);          /* bump per-request check epoch      */

/* userland API (implemented in mre_gtx.c) */
PHP_FUNCTION(mre_gtx_begin);
PHP_FUNCTION(mre_gtx_commit);
PHP_FUNCTION(mre_gtx_rollback);
PHP_FUNCTION(mre_gtx_status);

/* sql classification bitmask (shared by the mysqlnd hooks) */
#define MRE_GTX_Q_NONE     0x00
#define MRE_GTX_Q_BEGIN    0x01
#define MRE_GTX_Q_COMMIT   0x02
#define MRE_GTX_Q_ROLLBACK 0x04
int mre_gtx_sql_kind(const char *sql, size_t len);

#if MRE_HAVE_MYSQLND
/* returns 1 when the call must be swallowed (deferred COMMIT)          */
zend_bool mre_gtx_pre_call(MYSQLND_CONN_DATA *conn, mre_gtx_ctx *gx,
                           const char *query, size_t qlen, int qkind);
void mre_gtx_post_call(MYSQLND_CONN_DATA *conn, mre_gtx_ctx *gx,
                       zend_bool ok, const char *query, int qkind);
zend_bool mre_mysql_raw_send(MYSQLND_CONN_DATA *conn, const char *sql);
void mre_gtx_conn_closed(MYSQLND_CONN_DATA *conn, mre_gtx_ctx *gx);
#endif

/* ------------------------------------------------------------------ */
/* MySQL capture (mre_mysql.c) - mysqlnd methods table override        */
/* ------------------------------------------------------------------ */
#if MRE_HAVE_MYSQLND
void mre_mysql_minit(void);     /* hook conn_data + stmt method tables */
void mre_mysql_mshutdown(void); /* restore original tables */
void mre_mysql_reassert(void);  /* per request: re-apply if chained over */
#else
#define mre_mysql_minit()       /**/
#define mre_mysql_mshutdown()   /**/
#define mre_mysql_reassert()    /**/
#endif

/* ------------------------------------------------------------------ */
/* Redis capture (mre_redis.c) - tcp stream factory + RESP sniffing    */
/* ------------------------------------------------------------------ */
void mre_redis_minit(void);     /* wrap the "tcp" transport factory */
void mre_redis_mshutdown(void); /* restore previous factory */
void mre_redis_rehook(void);    /* per request: re-apply if replaced */

/* ------------------------------------------------------------------ */
/* Small utils (mre_util.c)                                            */
/* ------------------------------------------------------------------ */
void mre_iso8601_now(char *buf, size_t cap);           /* 2026-...+08:00 */
double mre_clock_mono_ms(void);
             /* monotonic clock reading in ms (duration measuring;
              * deltas are immune to wall-clock step changes) */
size_t mre_json_escape(const char *in, size_t in_len,
                       char *out, size_t out_cap);     /* out NUL-term   */
void mre_fmt_double(double d, char *out, size_t out_cap);
                       /* shortest round-trippable double text          */
void mre_copy_str(char *dst, size_t dst_cap,
                  const char *src, size_t src_len);    /* NUL-term copy  */
void mre_makedirs(const char *path);                   /* mkdir -p       */

/* request-level id "<pid>-<seq>": RINIT bumps a process-wide counter and
 * re-renders it; get() lazily renders "<pid>-0" for lines written before
 * the first request (e.g. MINIT-time warnings). Used by the log line
 * formats and injected into the log_script $context.                  */
void mre_request_id_next(void);       /* RINIT: bump + re-render */
const char *mre_request_id_get(void); /* current id, never NULL */

/* ------------------------------------------------------------------ */
/* Module globals                                                      */
/* ------------------------------------------------------------------ */
ZEND_BEGIN_MODULE_GLOBALS(mre)
	mre_config     cfg;              /* resolved in RINIT */
	zend_bool      cfg_valid;
	char           request_id[MRE_REQUEST_ID_MAX]; /* per-request "<pid>-<seq>" */
	FILE          *log_fp;           /* cached append handle  */
	char          *log_fp_path;      /* estrdup'd owner of cache */
	zend_bool      log_open_warned; /* one-time fallback warning */
	mre_yaml_node *yaml_root;        /* parsed yaml (persistent mem) */
	char          *yaml_path;        /* pestrdup'd loaded path */
	time_t         yaml_mtime;
	zend_bool      yaml_warned;     /* warn once per process on parse errors */

	/* rule engine cache (mre_rules.c; pointers are malloc'ed, not Zend) */
	void          *rules;            /* mre_rule array */
	size_t         rules_count;
	char          *rules_default_log;/* yaml default_log_file */
	char          *rules_src_path;   /* yaml path rules were built from */
	time_t         rules_src_mtime;
	const void    *rules_src_root;   /* yaml root pointer rules were built from */

	/* log script execution (mre_script.c): the script runs through a
	 * PHP-level try/catch include wrapper per record; no op_array cache
	 * is kept (include + opcache own script compilation) */
	zend_bool      script_available;/* 1 between RINIT and RSHUTDOWN     */
	zend_bool      script_warned;   /* one-time degradation warning      */
	zend_bool      script_busy;     /* re-entrancy guard                 */

	/* global transaction (mre_gtx.c) */
	int            gtx_epoch;       /* bumped per request                */
	char           gtx_reg_path[MRE_PATH_MAX]; /* resolved registry path */
	time_t         gtx_snap_mtime;  /* registry snapshot cache           */
	long           gtx_snap_size;
	zend_bool      gtx_snap_valid;
	int            gtx_snap_state;  /* 0 none 1 active 2 mark_rb 3 mark_c*/
	char           gtx_snap_id[64];
	long           gtx_snap_started;
	long           gtx_snap_timeout;
	long           gtx_snap_owner;
	zend_bool      gtx_snap_expired;/* timeout hit (safety rollback)     */
	char          *gtx_snap_conns;  /* packed conn entries (see mre_gtx.c)*/
	size_t         gtx_snap_nconns;
ZEND_END_MODULE_GLOBALS(mre)

ZEND_EXTERN_MODULE_GLOBALS(mre)

/* use the engine-provided accessor: correct on both ZTS and non-ZTS */
#define MRE_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(mre, v)

#if defined(ZTS) && defined(COMPILE_DL_MRE)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

/* declared / implemented in mre.c */
extern zend_module_entry mre_module_entry;
#define phpext_mre_ptr &mre_module_entry

#endif /* PHP_MRE_H */
