dnl
dnl $Id: config.m4 TBD $
dnl

PHP_ARG_ENABLE(lpc, whether to enable LPC support,
[  --enable-lpc           Enable LPC support])

AC_ARG_ENABLE(lpc-debug,
[  --enable-lpc-debug     Enable LPC debugging], 
[
  PHP_LPC_DEBUG=yes
], 
[
  PHP_LPC_DEBUG=no
])


if test "$PHP_LPC" != "no"; then
	if test "$PHP_LPC_DEBUG" != "no"; then
		AC_DEFINE(__DEBUG_LPC__, 1, [ ])
	fi

  AC_CACHE_CHECK(for zend_set_lookup_function_hook, php_cv_zend_set_lookup_function_hook,
  [
    orig_cflags=$CFLAGS
    CFLAGS="$INCLUDES $EXTRA_INCLUDES"
    AC_TRY_COMPILE([
#include "main/php.h"
#include "Zend/zend_API.h"
    ], [#ifndef zend_set_lookup_function_hook
	(void) zend_set_lookup_function_hook;
#endif], [
      php_cv_zend_set_lookup_function_hook=yes
    ],[
      php_cv_zend_set_lookup_function_hook=no
    ])
    CFLAGS=$orig_cflags
  ])
  if test "$php_cv_zend_set_lookup_function_hook" = "yes"; then
    AC_DEFINE(LPC_HAVE_LOOKUP_HOOKS, 1, [ ])
  else
    AC_DEFINE(LPC_HAVE_LOOKUP_HOOKS, 0, [ ])
  fi

  AC_MSG_CHECKING(whether we should enable valgrind support)
  AC_ARG_ENABLE(valgrind-checks,
  [  --disable-valgrind-checks
                          Disable valgrind based memory checks],
  [
    PHP_LPC_VALGRIND=$enableval
    AC_MSG_RESULT($enableval)
  ], [
    PHP_LPC_VALGRIND=yes
    AC_MSG_RESULT(yes)
    AC_CHECK_HEADER(valgrind/memcheck.h, 
  		[AC_DEFINE([HAVE_VALGRIND_MEMCHECK_H],1, [enable valgrind memchecks])])
  ])

  lpc_sources="lpc.c php_lpc.c \
               lpc_cache.c \
               lpc_compile.c \
               lpc_debug.c \
               lpc_main.c \
               lpc_zend.c \
               lpc_stack.c \
               lpc_pool.c \
               lpc_bin.c \
               lpc_string.c "

  PHP_NEW_EXTENSION(lpc, $lpc_sources, $ext_shared,, \\$(LPC_CFLAGS))
  PHP_SUBST(LPC_CFLAGS)
  PHP_INSTALL_HEADERS(ext/lpc, [lpc_serializer.h])
  AC_DEFINE(HAVE_LPC, 1, [ ])
fi
dnl vim: set ts=2 
