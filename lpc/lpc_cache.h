/*
  +----------------------------------------------------------------------+
  | LPC                                                                  |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006-2011 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt.                                 |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Daniel Cowgill <dcowgill@communityconnect.com>              |
  |          Rasmus Lerdorf <rasmus@php.net>                             |
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Community Connect Inc. in 2002
   and revised in 2005 by Yahoo! Inc. to add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.

 */

/* $Id: lpc_cache.h 307048 2011-01-03 23:53:17Z kalle $ */

#ifndef LPC_CACHE_H
#define LPC_CACHE_H

/*
 * This module defines the shared memory file cache. Basically all of the
 * logic for storing and retrieving cache entries lives here.
 */

#include "lpc.h"
#include "lpc_compile.h"
#include "lpc_pool.h"
#include "lpc_main.h"
#include "fopen_wrappers.h"
#include "TSRM.h"

#define LPC_CACHE_ENTRY_FILE   1
#define LPC_CACHE_KEY_FILE     2
#define LPC_CACHE_KEY_FPFILE   3

#define LPC_REQUEST_SCRIPT     1
#define LPC_NOSTAT             2
#define LPC_PATH_IGNORED       3
#define LPC_PATH_USED          4

#ifdef PHP_WIN32
typedef unsigned __int64 lpc_ino_t;
typedef unsigned __int64 lpc_dev_t;
#else
typedef ino_t lpc_ino_t;
typedef dev_t lpc_dev_t;
#endif

/* {{{ struct definitions: for the lpc_cache_key_t and lpc_cache_entry_t types */

typedef struct _lpc_cache_t lpc_cache_t; /* opaque cache type */

/* 
 * A note on the strategy for file identification used in LPC.  Unlike in APC where cached entries
 * are shared across scripts and even UIDs / applications, and therefore the system must allow
 * some relatively stringent checks on cached opcode integrity, in LPC, the cache is by default
 * specific to the request script and therefore to the UID of that script is running under a CGI/
 * FastCGI SAPI.  The path is also specfic to the  request script, so again a more relaxed
 * treatment of relative and include_path-based filenames also applies.  In this version the
 * standard php_resolve_path() function is used to convert the filename to canonical form, which
 * does require VFAT caching of the script directories, so I may look at this again.
 */
typedef struct _lpc_cache_key_t {
    char         *filename;
    int           filename_length;
    time_t        mtime;                 /* the mtime of this cached entry */
	size_t        filesize;
	ino_t         inode;
	dev_t         dev_id;
	int           type;
    php_stream   *fp;
} lpc_cache_key_t;

#define LPC_REQUEST_SCRIPT 1
#define LPC_NOSTAT         2
#define LPC_FSTAT          3

typedef struct _lpc_cache_entry_t {
    char *filename;             /* absolute path to source file */
    zend_op_array* op_array;    /* op_array allocated in shared memory */
    lpc_function_t* functions;  /* array of lpc_function_t's */
    lpc_class_t* classes;       /* array of lpc_class_t's */
    long halt_offset;           /* value of __COMPILER_HALT_OFFSET__ for the file */
    unsigned char type;
    size_t mem_size;
    lpc_pool *pool;
} lpc_cache_entry_t;
/* }}} */

/*
 * lpc_cache_create creates the local memory compiler cache wrapper for the file-based
 * CacheDB cache.  The scope of this in-memory wrapper is the request, so this function 
 * should be called just once during RINIT.  Returns a pointer to the cache object.
 */
extern zend_bool lpc_cache_create(TSRMLS_D);

/*
 * lpc_cache_destroy is the DTOR for a cache object.  This function should be
 * called during RSHUTDOWN.
 */
extern void lpc_cache_destroy(TSRMLS_D);

/*
 * lpc_cache_clear empties a cache and triggers the emptying of the file-based CacheDB copy. 
 */
extern void lpc_cache_clear(TSRMLS_D);

/*
 * lpc_cache_make_file_key creates and returns a key object given a relative or absolute
 * filename and an optional list of auxillary paths to search. include_path is searched if
 * the filename cannot be found relative to the current working directory.
 */
extern lpc_cache_key_t* lpc_cache_make_file_key(char* filename TSRMLS_DC);

/*
 * lpc_cache_retrieve loads the entry for a filename key and returns a pointer to the 
 * entry record within a serial pool containing the retrieved entry if it exists and NULL 
 * otherwise. It is the responsibiliy of the caller to call the pool DTOR when done with it.
 * Note that executing this DTOR replaces the equivalent APC cache release function.
 */
extern lpc_cache_entry_t* lpc_cache_retrieve(lpc_cache_key_t *key TSRMLS_DC);

/*
 * lpc_cache_make_entry creates an lpc_cache_entry_t object for given a filename and the 
 * compilation results returned by the PHP compiler.
 */
extern lpc_cache_entry_t* lpc_cache_make_entry(const char* filename,
                                                    zend_op_array* op_array,
                                                    lpc_function_t* functions,
                                                    lpc_class_t* classes,
                                                    lpc_context_t* ctxt
                                                    TSRMLS_DC);

/*
 * lpc_cache_insert adds an entry to the cache, using a filename as a key. Unlike APC, the
 * filename is used as-is, but the path as at the insert request is also stored as metadata.
 * This path can change from one insert to the next as include_path can be set programmatically
 * during execution.  The reason for this approach is to miminise the need for directory and 
 * file access when retrieving cached compiled files.
 *
 * HOWEVER, note the path is revalidated during the find operation and if the path has changed
 * then the cache is considered to be invalidated.   
 */
extern int lpc_cache_insert(lpc_cache_key_t *key,
                            lpc_cache_entry_t* value, 
							lpc_context_t* ctxt TSRMLS_DC);

/* Not sure why this is in lpc_cache.c as the implementation is in lpc_main.c */
zend_bool lpc_compile_cache_entry(lpc_cache_key_t *key, 
                                  zend_file_handle* h, 
                                  int type, zend_op_array** op_array_pp, 
                                  lpc_cache_entry_t** cache_entry_pp TSRMLS_DC);

/*
 * Give information on the cache content
 */
extern zval* lpc_cache_info(zend_bool limited TSRMLS_DC);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */



