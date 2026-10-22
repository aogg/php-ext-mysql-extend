/*
  +----------------------------------------------------------------------+
  | mre - mysql-redis-extend                                             |
  |                                                                      |
  | Stage A module entry: lifecycle, ini entries, env config,            |
  | MINFO, and hook installation for mysqlnd capture and the             |
  | streams RESP sniffer.                                                |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_mre.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* userland functions (debug / ops helpers)                             */
/* ------------------------------------------------------------------ */
ZEND_BEGIN_ARG_INFO_EX(arginfo_mre_yaml_dump, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_mre_gtx_void, 0, 0, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(mre_yaml_dump)
{
	mre_yaml_dump_to_zval(return_value);
}

static const zend_function_entry mre_functions[] = {
	PHP_FE(mre_yaml_dump, arginfo_mre_yaml_dump)
	PHP_FE(mre_gtx_begin, arginfo_mre_gtx_void)
	PHP_FE(mre_gtx_commit, arginfo_mre_gtx_void)
	PHP_FE(mre_gtx_rollback, arginfo_mre_gtx_void)
	PHP_FE(mre_gtx_status, arginfo_mre_gtx_void)
	PHP_FE_END
};

/* ------------------------------------------------------------------ */
/* module globals                                                       */
/* ------------------------------------------------------------------ */
ZEND_DECLARE_MODULE_GLOBALS(mre)

static PHP_GINIT_FUNCTION(mre)
{
	memset(mre_globals, 0, sizeof(zend_mre_globals));
	/* nothing else: all defaults come from mre_config_refresh() */
}

static PHP_GSHUTDOWN_FUNCTION(mre)
{
	/* mre_globals is a pointer under ZTS, a struct otherwise */
	(void)mre_globals;
	mre_yaml_config_free();
	mre_log_close();
}

/* ------------------------------------------------------------------ */
/* ini entries (values are read through INI_* / env in mre_config.c)    */
/* ------------------------------------------------------------------ */
PHP_INI_BEGIN()
	PHP_INI_ENTRY("mre.enable",        "0",    PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("mre.log_file",      "",     PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("mre.log_format",    "json", PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("mre.log_script",    "",     PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("mre.capture_mysql", "1",    PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("mre.capture_redis", "1",    PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("mre.redis_port",    "6379", PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("mre.sample_rows",   "1",    PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("mre.state_dir",     "",     PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("mre.yaml_config",   "",     PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("mre.gtx",           "0",    PHP_INI_ALL, NULL)
	PHP_INI_ENTRY("mre.gtx_timeout",   "300",  PHP_INI_ALL, NULL)
PHP_INI_END()

/* ------------------------------------------------------------------ */
/* lifecycle                                                            */
/* ------------------------------------------------------------------ */
PHP_MINIT_FUNCTION(mre)
{
	REGISTER_INI_ENTRIES();

	/* resolve config once so hooks see values before the first RINIT */
	mre_config_refresh();

	/* install capture hooks */
	mre_redis_minit();
	mre_mysql_minit();

	/* load the yaml rule file when configured (stage B consumes it) */
	mre_yaml_config_ensure();

	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(mre)
{
	mre_redis_mshutdown();
	mre_mysql_mshutdown();
	mre_rules_free();
	mre_yaml_config_free();
	mre_log_close();
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}

PHP_RINIT_FUNCTION(mre)
{
	/* per-request unique id "<pid>-<seq>" for every log line and the
	 * log_script $context (fpm: one pid serves many requests serially) */
	mre_request_id_next();

	/* env > ini every request (env may change between fpm requests) */
	mre_config_refresh();

	/* load / reload yaml rules when the file changed */
	mre_yaml_config_ensure();

	/* stage B: per-request script cache + gtx check epoch */
	mre_script_rinit();
	mre_gtx_rinit();

	/* keep our hooks in place if someone re-registered over them */
	mre_redis_rehook();
	mre_mysql_reassert();

	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(mre)
{
	/* executing the log script after the executor shuts down is unsafe */
	mre_script_rshutdown();
	return SUCCESS;
}

/* ------------------------------------------------------------------ */
/* phpinfo()                                                            */
/* ------------------------------------------------------------------ */
static void mre_info_value_row(const char *name, const char *value)
{
	php_info_print_table_row(2, name, value ? value : "");
}

PHP_MINFO_FUNCTION(mre)
{
	const mre_config *cfg;
	char portbuf[16], samplebuf[16];

	if (!MRE_G(cfg_valid)) {
		mre_config_refresh();
	}
	cfg = &MRE_G(cfg);

	php_info_print_table_start();
	php_info_print_table_header(2, "mre support", "enabled");
	php_info_print_table_row(2, "version", PHP_MRE_VERSION);

	snprintf(portbuf, sizeof(portbuf), "%d", cfg->redis_port);
	snprintf(samplebuf, sizeof(samplebuf), "%d", cfg->sample_rows);

	mre_info_value_row("enable (MRE_ENABLE)",
		cfg->enable ? "1" : "0");
	mre_info_value_row("log_file (MRE_LOG_FILE)", cfg->log_file);
	mre_info_value_row("log_format (MRE_LOG_FORMAT)",
		cfg->json_format ? "json" : "text");
	mre_info_value_row("capture_mysql (MRE_CAPTURE_MYSQL)",
		cfg->capture_mysql ? "1" : "0");
	mre_info_value_row("capture_redis (MRE_CAPTURE_REDIS)",
		cfg->capture_redis ? "1" : "0");
	mre_info_value_row("redis_port (MRE_REDIS_PORT)", portbuf);
	mre_info_value_row("sample_rows (MRE_SAMPLE_ROWS)", samplebuf);
	mre_info_value_row("state_dir (MRE_STATE_DIR)", cfg->state_dir);
	mre_info_value_row("yaml_config (MRE_YAML_CONFIG)", cfg->yaml_config);
	mre_info_value_row("log_script (MRE_LOG_SCRIPT)", cfg->log_script);
	{
		char gtxbuf[32];
		snprintf(gtxbuf, sizeof(gtxbuf), "%d", cfg->gtx_timeout);
		mre_info_value_row("gtx (MRE_GTX)", cfg->gtx ? "1" : "0");
		mre_info_value_row("gtx_timeout (MRE_GTX_TIMEOUT)", gtxbuf);
	}
	mre_info_value_row("effective rules",
		(MRE_G(rules_count) > 0) ? "loaded (see mre_yaml_dump())" : "none");
#if MRE_HAVE_MYSQLND
	mre_info_value_row("mysqlnd capture", "available");
#else
	mre_info_value_row("mysqlnd capture", "not available (built without mysqlnd headers)");
#endif
	php_info_print_table_end();
}

/* ------------------------------------------------------------------ */
/* module entry                                                         */
/* ------------------------------------------------------------------ */
zend_module_entry mre_module_entry = {
	STANDARD_MODULE_HEADER_EX,
	NULL,
	NULL,
	"mre",
	mre_functions,
	PHP_MINIT(mre),
	PHP_MSHUTDOWN(mre),
	PHP_RINIT(mre),
	PHP_RSHUTDOWN(mre),
	PHP_MINFO(mre),
	PHP_MRE_VERSION,
	PHP_MODULE_GLOBALS(mre),
	PHP_GINIT(mre),
	PHP_GSHUTDOWN(mre),
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_MRE
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(mre)
#endif
