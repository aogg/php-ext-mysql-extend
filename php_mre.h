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
#define MRE_LOG_LINE_MAX    16384 /* one log line, stack allocated     */
#define MRE_JSON_VAL_MAX    8192  /* escaped content cap inside a line */
#define MRE_EXTRA_KV_MAX    12    /* max extra key/value pairs         */
#define MRE_EXTRA_VAL_MAX   560   /* max rendered value per extra item */

#define MRE_MAX_SQL_LOG     4096  /* SQL text cap in logs              */
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
	int       json_format;                         /* MRE_LOG_FORMAT   */
	int       redis_port;                          /* MRE_REDIS_PORT   */
	int       sample_rows;                         /* MRE_SAMPLE_ROWS  */
	char      log_file[MRE_PATH_MAX];              /* MRE_LOG_FILE     */
	char      state_dir[MRE_PATH_MAX];             /* MRE_STATE_DIR    */
	char      yaml_config[MRE_PATH_MAX];           /* MRE_YAML_CONFIG  */
} mre_config;

void mre_config_refresh(void);      /* env > ini into MRE_G(cfg) */

/* ------------------------------------------------------------------ */
/* Extra log fields (rendered into "extra" json object / k=v in text)  */
/* ------------------------------------------------------------------ */
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
/* Logger (mre_log.c)                                                  */
/* One line per record; append + flock; process-cached handle.         */
/* Never allocates Zend memory: usable from hooks at any time.         */
/* ------------------------------------------------------------------ */
void mre_log_write(const char *type, const char *dir,
                   const char *content, size_t content_len,
                   const mre_extra *extra);
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
size_t mre_json_escape(const char *in, size_t in_len,
                       char *out, size_t out_cap);     /* out NUL-term   */
void mre_copy_str(char *dst, size_t dst_cap,
                  const char *src, size_t src_len);    /* NUL-term copy  */

/* ------------------------------------------------------------------ */
/* Module globals                                                      */
/* ------------------------------------------------------------------ */
ZEND_BEGIN_MODULE_GLOBALS(mre)
	mre_config     cfg;              /* resolved in RINIT */
	zend_bool      cfg_valid;
	FILE          *log_fp;           /* cached append handle  */
	char          *log_fp_path;      /* estrdup'd owner of cache */
	zend_bool      log_open_warned; /* one-time fallback warning */
	mre_yaml_node *yaml_root;        /* parsed yaml (persistent mem) */
	char          *yaml_path;        /* pestrdup'd loaded path */
	time_t         yaml_mtime;
	zend_bool      yaml_warned;     /* warn once per process on parse errors */
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
