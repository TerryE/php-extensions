dnl $Id$
dnl config.m4 for cachedb extension

PHP_ARG_ENABLE(cachedb, whether to enable CacheDB support,
[  --enable-cachedb           Enable CacheDB support])

AC_ARG_ENABLE(cachedb-debug,
[  --enable-cachedb-debug     Enable CacheDB debugging], 
[
  PHP_CACHEDB_DEBUG=yes
], 
[
  PHP_CACHEDB_DEBUG=no
])

if test "$PHP_CACHEDB" != "no"; then

  if test "$PHP_CACHEDB_DEBUG" != "no"; then
    AC_DEFINE(__DEBUG_CACHEDB__, 1, [ ])
  fi

  AC_DEFINE(HAVE_CACHEDB,1,[Whether CacheDB is present])
  PHP_NEW_EXTENSION(cachedb, php_cachedb.c cachedb.c, $ext_shared)
fi

