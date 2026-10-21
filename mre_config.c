/*
  +----------------------------------------------------------------------+
  | mre - configuration resolution (Linux env MRE_* wins over ini mre.*) |
  +----------------------------------------------------------------------+
*/

#include "php_mre.h"

#include <stdlib.h>
#include <string.h>

/*
 * Every option exists twice:
 *   - as an ini entry  "mre.xxx"      (declared in mre.c)
 *   - as an env var    "MRE_XXX"      (real Linux environment variable)
 *
 * Precedence: env first, ini second. This makes docker deployments
 * (`-e MRE_LOG_FILE=/proc/1/fd/1`) work without touching php.ini, while
 * classic fpm deployments can still configure through php.ini.
 */

/* read an env var as string; returns NULL when unset or empty */
static const char *mre_env(const char *name)
{
	const char *v = getenv(name);
	if (v && *v != '\0') {
		return v;
	}
	return NULL;
}

static zend_bool mre_env_bool(const char *name, zend_bool fallback)
{
	const char *v = mre_env(name);
	if (!v) {
		return fallback;
	}
	return (zend_bool)(strncasecmp(v, "1", 1) == 0
		|| strncasecmp(v, "true", 4) == 0
		|| strncasecmp(v, "on", 2) == 0
		|| strncasecmp(v, "yes", 3) == 0);
}

static int mre_env_int(const char *name, int fallback)
{
	const char *v = mre_env(name);
	if (!v) {
		return fallback;
	}
	return atoi(v);
}

/*
 * ini value as string; returns NULL when unset or empty.
 *
 * This deliberately does NOT use the INI_STR() macro: on PHP 7.x it expands
 * to zend_ini_string_ex((name), sizeof(name)-1, ...), and applying sizeof()
 * to our `const char *name` parameter truncated every lookup to the pointer
 * size (7 bytes) - "mre.gtx_timeout" matched "mre.gtx" and every ini option
 * silently fell back to its code default (container bug BUG-3).  8.0+
 * switched the macro to strlen(name), so we call the underlying API with the
 * real length on every version.  The 7.x prototype takes a non-const char*,
 * hence the cast; the lookup never modifies the name.
 */
static const char *mre_ini_str(const char *name)
{
#if PHP_VERSION_ID < 80000
	char *v = zend_ini_string_ex((char *)name, strlen(name), 0, NULL);
#else
	const char *v = zend_ini_string_ex(name, strlen(name), 0, NULL);
#endif
	if (v && *v != '\0') {
		return v;
	}
	return NULL;
}

static zend_bool mre_ini_bool(const char *name, zend_bool fallback)
{
	const char *v = mre_ini_str(name);
	if (!v) {
		return fallback;
	}
	return (zend_bool)(strncasecmp(v, "1", 1) == 0
		|| strncasecmp(v, "true", 4) == 0
		|| strncasecmp(v, "on", 2) == 0
		|| strncasecmp(v, "yes", 3) == 0);
}

static int mre_ini_int(const char *name, int fallback)
{
	const char *v = mre_ini_str(name);
	if (!v) {
		return fallback;
	}
	return atoi(v);
}

/* pick config string: env > ini; copies into dst (bounded, NUL-terminated) */
static void mre_cfg_str(char *dst, size_t dst_cap,
                        const char *env_name, const char *ini_name)
{
	const char *v = mre_env(env_name);
	if (!v) {
		v = mre_ini_str(ini_name);
	}
	mre_copy_str(dst, dst_cap, v, v ? strlen(v) : 0);
}

static void mre_cfg_bool(zend_bool *dst,
                         const char *env_name, const char *ini_name,
                         zend_bool fallback)
{
	if (mre_env(env_name) != NULL) {
		*dst = mre_env_bool(env_name, fallback);
	} else {
		*dst = mre_ini_bool(ini_name, fallback);
	}
}

static void mre_cfg_int(int *dst,
                        const char *env_name, const char *ini_name,
                        int fallback)
{
	if (mre_env(env_name) != NULL) {
		*dst = mre_env_int(env_name, fallback);
	} else {
		*dst = mre_ini_int(ini_name, fallback);
	}
}

/*
 * Resolve the full configuration into MRE_G(cfg).
 * Called on every RINIT; cheap (a few getenv/ini lookups).
 */
void mre_config_refresh(void)
{
	mre_config *cfg = &MRE_G(cfg);

	mre_cfg_bool(&cfg->enable, "MRE_ENABLE", "mre.enable", 0);
	mre_cfg_bool(&cfg->capture_mysql, "MRE_CAPTURE_MYSQL", "mre.capture_mysql", 1);
	mre_cfg_bool(&cfg->capture_redis, "MRE_CAPTURE_REDIS", "mre.capture_redis", 1);
	mre_cfg_bool(&cfg->gtx, "MRE_GTX", "mre.gtx", 0);

	{
		const char *fmt = mre_env("MRE_LOG_FORMAT");
		if (!fmt) {
			fmt = mre_ini_str("mre.log_format");
		}
		cfg->json_format = (fmt == NULL)
			? 1 /* default json-lines */
			: (strncasecmp(fmt, "text", 4) != 0); /* anything else = json */
	}

	mre_cfg_int(&cfg->redis_port, "MRE_REDIS_PORT", "mre.redis_port", 6379);
	if (cfg->redis_port <= 0 || cfg->redis_port > 65535) {
		cfg->redis_port = 6379;
	}

	mre_cfg_int(&cfg->sample_rows, "MRE_SAMPLE_ROWS", "mre.sample_rows", 1);
	if (cfg->sample_rows < 0) {
		cfg->sample_rows = 0;
	}
	if (cfg->sample_rows > 100) {
		cfg->sample_rows = 100;
	}

	mre_cfg_int(&cfg->gtx_timeout, "MRE_GTX_TIMEOUT", "mre.gtx_timeout", 300);
	if (cfg->gtx_timeout < 10) {
		cfg->gtx_timeout = 10;
	}
	if (cfg->gtx_timeout > 86400) {
		cfg->gtx_timeout = 86400;
	}

	mre_cfg_str(cfg->log_file, sizeof(cfg->log_file),
		"MRE_LOG_FILE", "mre.log_file");
	mre_cfg_str(cfg->log_script, sizeof(cfg->log_script),
		"MRE_LOG_SCRIPT", "mre.log_script");
	mre_cfg_str(cfg->state_dir, sizeof(cfg->state_dir),
		"MRE_STATE_DIR", "mre.state_dir");
	mre_cfg_str(cfg->yaml_config, sizeof(cfg->yaml_config),
		"MRE_YAML_CONFIG", "mre.yaml_config");

	MRE_G(cfg_valid) = (zend_bool)1;
}
