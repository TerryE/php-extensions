/*
  +----------------------------------------------------------------------+
  | LPC                                                                  |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006-2011 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Daniel Cowgill <dcowgill@communityconnect.com>              |
  |          George Schlossnagle <george@omniti.com>                     |
  |          Rasmus Lerdorf <rasmus@php.net>                             |
  |          Arun C. Murthy <arunc@yahoo-inc.com>                        |
  |          Gopal Vijayaraghavan <gopalv@yahoo-inc.com>                 |
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Community Connect Inc. in 2002
   and revised in 2005 by Yahoo! Inc. to add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.
*/

#ifndef LPC_H
#define LPC_H

#define                    APC_DEBUG   1
// #define                  __DEBUG_LPC__ 1

#ifdef APC_DEBUG
#  define ENTER(s) int dummy_to_be_ignored = lpc_debug_enter(#s);
extern int lpc_debug_enter(char *s);
#else 
#  define ENTER(s) 
#endif
#define NOENTER(s) 

/*
 * This module defines utilities and helper functions used elsewhere in LPC.
 */

/* Commonly needed C library headers. */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* UNIX headers (needed for struct stat) */
#include <sys/types.h>
#include <sys/stat.h>
#ifndef PHP_WIN32
#include <unistd.h>
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "php.h"
#include "php_streams.h"

#include "lpc_php.h"
#include "lpc_main.h"
#include "lpc_cache.h"

#define PERSISTENT 1

#ifdef __DEBUG_LPC__
#define LPC_DEBUG 
#endif

/* console display functions */
extern void lpc_error(const char *format TSRMLS_DC, ...);
extern void lpc_warning(const char *format TSRMLS_DC, ...);
extern void lpc_notice(const char *format TSRMLS_DC, ...);
extern void lpc_debug(const char *format TSRMLS_DC, ...);

/* string and text manipulation */
extern char** lpc_tokenize(const char* s, char delim TSRMLS_DC);

/* filesystem functions */

typedef struct lpc_fileinfo_t 
{
    char *fullpath;
    char path_buf[MAXPATHLEN];
    php_stream_statbuf st_buf;
} lpc_fileinfo_t;

extern int lpc_search_paths(const char* filename, const char* path, lpc_fileinfo_t* fileinfo TSRMLS_DC);

/* regular expression wrapper functions */
extern void* lpc_regex_compile_array(char* pattern_list TSRMLS_DC);
extern void lpc_regex_destroy_array(void* p TSRMLS_DC);
extern int lpc_regex_match_array(void* p, const char* input);

#if defined(__GNUC__)
# define LPC_UNUSED __attribute__((unused))
# define LPC_USED __attribute__((used))
# define LPC_ALLOC __attribute__((malloc))
# define LPC_HOTSPOT __attribute__((hot))
#else 
# define LPC_UNUSED
# define LPC_USED
# define LPC_ALLOC 
# define LPC_HOTSPOT 
#endif

/* Only implement regexp filters for PHP versions >= 5.2.2 otherwise it acts as a Noop */
#if HAVE_PCRE || (HAVE_BUNDLED_PCRE && (PHP_MAJOR_VERSION > 5) || (PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION > 2) ||   (PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION == 2 && PHP_RELEASE_VERSION >= 2 ))
#  define  PHP_REXEP_OK
#endif
ZEND_BEGIN_MODULE_GLOBALS(lpc)
    /* configuration parameters */
    zend_bool   enabled;                /* if true, lpc is enabled (defaults to true) */
    char*       filter;                 /* regex filter that prevents caching */
 
    /* module variables */
    zend_bool   initialized;            /* true if module was initialized */
    zend_bool   cache_by_default;       /* true if files should be cached unless filtered out */
                                        /* false if files should only be cached if filtered in */
    long        file_update_protection; /* Age in seconds before a file is eligible to be cached - 0 to disable */
    zend_bool   enable_cli;             /* Flag to override turning LPC off for CLI */
    long        max_file_size;          /* Maximum size of file, in bytes that LPC will be allowed to cache */
    zend_bool   fpstat;                 /* true if fullpath includes should be stat'ed */
    zend_bool   canonicalize;           /* true if relative paths should be canonicalized in no-stat mode */
    zend_bool   report_autofilter;      /* true for auto-filter warnings */
    zend_bool   include_once;           /* Override the ZEND_INCLUDE_OR_EVAL opcode handler to avoid pointless fopen()s [still experimental] */
    HashTable   copied_zvals;           /* my_copy recursion detection list */
    zend_bool   force_file_update;      /* force files to be updated during lpc_compile_file */
    char        canon_path[MAXPATHLEN]; /* canonical path for key data */
    time_t      sapi_request_time;      /* the SAPI request start time is used for any timestamp validation */
#ifdef ZEND_ENGINE_2_4
    long        shm_strings_buffer;
#endif
    lpc_cache_t *lpc_cache;             /* the global compiler cache */
	HashTable   pools;                  /* Table of created pools */
	zend_bool   force_cache_delete;     /* Flag that the file D/B is to be deleted and further loading disabbled */
    char       *clear_cookie;	        /* Name of Cookie which will force a cache clear */
    char       *clear_parameter;        /* Name of Request parameter which will force a cache clear */
ZEND_END_MODULE_GLOBALS(lpc)

/* (the following declaration is defined in php_lpc.c) */
ZEND_EXTERN_MODULE_GLOBALS(lpc)

/* The tsrm_ls pointer is carried in the pool header which is passes down many call chains so 
 * this avoids the need to pass the TSRML_D(C) pointers on a lot of routines */
#ifdef ZTS
# define TSRMLS_P *((void ****)pool)
# define TSRMLS_PC , TSRMLS_P
# define LPCG(v) TSRMG(lpc_globals_id, zend_lpc_globals *, v)
# define TSRMGP(id, type, element)	(((type) (*TSRMLS_P)[TSRM_UNSHUFFLE_RSRC_ID(id)])->element)
# define LPCGP(v) TSRMGP(lpc_globals_id, zend_lpc_globals *, v)
# define TSRMLS_FETCH_FROM_POOL() void ***tsrm_ls = TSRMLS_P
#else
# define TSRMLS_P
# define TSRMLS_PC
# define LPCG(v) (lpc_globals.v)
# define LPCGP(v) (lpc_globals.v)
# define TSRMLS_FETCH_FROM_POOL()
#endif

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
