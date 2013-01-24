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
  | Authors: Terry Ellison <Terry@ellisons.org.uk                        |
  +----------------------------------------------------------------------+

   This software includes content derived from the APC extension which was
   initially contributed to PHP by Community Connect Inc. in 2002 and revised 
   in 2005 by Yahoo! Inc. See README for further details.

   All other licensing and usage conditions are those of the PHP Group.
*/ 

#ifndef LPC_CACHE_H
#define LPC_CACHE_H

/*
 * This module defines the shared memory file cache. Basically all of the
 * logic for storing and retrieving cache entries lives here.
 */
#include "zend.h"
//#include "fopen_wrappers.h"
#include "TSRM.h"
#include "lpc_pool.h"

/* {{{ Overview documentation
 
 LPC and APC use very different caching strategies which are optimised for their different usecases.

 *  APC maintains its cache in a SMA that is typically shared across all applications. Here cache
    integrity must be mainted through tight lock management and extra logic layers to ensure the
    transactional consistency of any updates to the cache. Cache entries are shared across
    applications and hence there must be high confidence in their mapping to real files in the
    filesystem to prevent cross application side effects. 

 *  LPC maintains its cache in the filesystem and it is specific to the application and even to the
    request path. Any updates to a cache are only committed on (successful) completion of a request
    by a file-move operation replacing the old version. So entries only need to be identified by a
    convention that is internally consistent within the application, and a relaxed attitude to cache
    consistency is adopted as any updates are discarded on error.

 Whilst the public interface of LPC is aligned to that of APC to minimise unnecessary code changes in
 lpc_main.c, these architectural differences essentially mean that the caching implementation in LPC
 is a complete rewrite. It uses a CacheDB file database with a simple HashTable index maintained
 internally in the cache.  

 The cache is designed to minimise physical I/O (hence the use of CacheDB), as this is the main
 performance impactor on shared hosting configurations. Entries are keyed by the path used by the
 application in the compile request as-is. Whilst, there are pathelogical cases where this might not
 be unique, for example if the application programmatically updates the include_path to one of a set
 of alternatives on a per-request basis (e.g. altering a language directory), so the option of using
 canonical filenames (say by the standard php_resolve_path() function) might be considered in a
 future version.  In the meantime, the application administrator has the option of using the INI
 option lpc.filter to exclude such files from the cache.

 The cache is considered invalid if any of the following occur:

 *  The PHP version number has changed (as this might invalidate stored opcodes)
 *  The filesize or mtime of the request script has changed
 *  The cookie/query parameter associated with INI directives lpc.clear_cookie or
    lpc.clear_parameter is set
 *  Any file fails stat validation based on filesize or mtime.  

 In this case the cacheDB file is closed and deleted, then caching is turned off for the remainder of
 the request with compilation failing back to the default compiler, so that the next request can
 rebuild a clean cache.

 Note that the inode and idev CANNOT be used to establish the identity of a file.  This is because on
 some shared hosting stacks, the requests are load balanced across a farm of web servers, with the
 user files (including in this case the cache and PHP scripts) being loaded from NFS-mounted shared
 NAS infrastructure.  In such configurations the (idev, inode) can vary from server to server causing
 false cache invalid events if these were used.

 Also note that LPC uses a percentage (0..100) for lpc.stat rather than a simple boolean. So if
 lpc.stat = "2" then a random 2% of loads are stat-checked. This provides a low I/O impact safety net
 to detect (at least eventually) any code base changes and trigger the cache rebuild even if the 
 application administrator doesn't force an explicit refresh by deleting the cache file or executing
 a request with one of the clear cookie/parameter options set.
*/

/* }}}*/

/* {{{ struct definitions: for the lpc_cache_key_t and lpc_cache_entry_t types */

typedef struct _lpc_cache_t lpc_cache_t; /* opaque cache type */

typedef enum {
    LPC_CACHE_MISS      = 0x0,  /* A key which doesn't map onto an LPC cache entry, */
    LPC_CACHE_LOOKUP    = 0x1,  /* A key which maps onto an LPC cache entry */
    LPC_CACHE_MISMATCH  = 0x2   /* A key which maps onto an LPC cache entry, but the file details mismatch */
} lpc_cache_type_t;

typedef struct _lpc_cache_key_t {
    char            *filename;              /* This is whatever the execution env passed to the */
                                            /* compile request and NOT a fully resolved name    */
    int              filename_length;       /* length for above */
    time_t           mtime;                 /* the mtime of this cached entry */
    size_t           filesize;
    lpc_cache_type_t type;
} lpc_cache_key_t;
/* }}} */

/* {{{ Public functions */

/*
 * lpc_cache_create creates the local memory compiler cache wrapper for the file-based CacheDB
 * cache. The scope of this in-memory wrapper is the request, so this function should be called
 * just once during RINIT. Returns a status and the maximum module size as an out parameter.
 */
extern zend_bool lpc_cache_create(uint *max_module_len TSRMLS_DC);

/*
 * lpc_cache_destroy is the DTOR for a cache object.  This function should be called during
 * RSHUTDOWN.
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
extern lpc_cache_key_t* lpc_cache_make_key(zend_file_handle* h, const char* include_path TSRMLS_DC);
extern void lpc_cache_free_key(lpc_cache_key_t* key TSRMLS_DC);

/*
 * lpc_cache_retrieve loads the entry for a filename key and returns a pointer to the 
 * serial pool containing the retrieved entry if it exists and NULL otherwise. It is the
 * responsibiliy of the caller to call the pool DTOR when done with it.  Note that
 * executing this DTOR replaces the equivalent APC cache release function.
 */
extern lpc_pool* lpc_cache_retrieve(lpc_cache_key_t *key, void** first_entry TSRMLS_DC);

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
extern void lpc_cache_insert(lpc_cache_key_t *key, zend_uchar *compressed_buffer,
                             zend_uint compressed_length, zend_uint pool_length TSRMLS_DC);
/*
 * Give information on the cache content
 */
extern zval* lpc_cache_info(zend_bool limited TSRMLS_DC);

/*
 * Wrapper around the ZEND_INCLUDE_OR_EVAL instruction handler.  This is part of the 
 * Cache module because of its close coupling to the cache functions.
 */
extern int ZEND_FASTCALL lpc_include_or_eval_handler(ZEND_OPCODE_HANDLER_ARGS);

/* }}} */
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */



