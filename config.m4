dnl config.m4 for extension mre
dnl
dnl mre (mysql-redis-extend) has NO external library dependencies.
dnl - YAML is parsed by a built-in minimal parser (no libyaml).
dnl - MySQL capture hooks the in-core mysqlnd API (no libmysqlclient).
dnl - Redis capture hooks PHP streams (no hiredis).
dnl Comments in this file use dnl (m4) so nothing leaks into configure.

PHP_ARG_ENABLE([mre],
  [whether to enable the mre (mysql-redis-extend) extension],
  [AS_HELP_STRING([--enable-mre],
    [Enable mre: capture MySQL (mysqlnd) and Redis (streams RESP sniffing) traffic])],
  [no])

if test "$PHP_MRE" != "no"; then

  dnl ---------------------------------------------------------------------------
  dnl mysqlnd is part of php-src itself (ext/mysqlnd). Detect its headers so the
  dnl extension can still build (without MySQL capture) on unusual builds that
  dnl were compiled --without-mysqlnd.
  dnl ---------------------------------------------------------------------------
  dnl mysqlnd headers are not self-contained (they require php.h first), so a
  dnl plain file check is the reliable detection here. The real compile test
  dnl happens in mre_mysql.c, which includes php.h before the mysqlnd headers.
  PHP_MRE_MYSQLND=0
  if test -f "$phpincludedir/ext/mysqlnd/mysqlnd.h"; then
    PHP_MRE_MYSQLND=1
    AC_DEFINE([HAVE_MRE_MYSQLND], [1],
      [Define to 1 if the ext/mysqlnd headers are available (MySQL capture enabled).])
  fi

  dnl Shared extensions get their own static TSRMLS cache copy (standard practice,
  dnl avoids TLS lookups through the engine global on ZTS builds).
  PHP_NEW_EXTENSION([mre],
    [mre.c \
     mre_config.c \
     mre_util.c \
     mre_log.c \
     mre_yaml.c \
     mre_mysql.c \
     mre_redis.c],
    [$ext_shared],,
    [-DZEND_ENABLE_STATIC_TSRMLS_CACHE=1])
fi
