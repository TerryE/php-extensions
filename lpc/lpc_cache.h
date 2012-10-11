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
#include "TSRM.h"

#define LPC_CACHE_ENTRY_FILE   1

#define LPC_CACHE_KEY_FILE     1
#define LPC_CACHE_KEY_FPFILE   3

#ifdef PHP_WIN32
typedef unsigned __int64 lpc_ino_t;
typedef unsigned __int64 lpc_dev_t;
#else
typedef ino_t lpc_ino_t;
typedef dev_t lpc_dev_t;
#endif

/* {{{ struct definition: lpc_cache_key_t */
#define T lpc_cache_t*
typedef struct lpc_cache_t lpc_cache_t; /* opaque cache type */

typedef union _lpc_cache_key_data_t {
    struct {
        lpc_dev_t device;             /* the filesystem device */
        lpc_ino_t inode;              /* the filesystem inode */
    } file;
    struct {
        const char *fullpath;
        int fullpath_len;
    } fpfile;
} lpc_cache_key_data_t;

typedef struct lpc_cache_key_t lpc_cache_key_t;
struct lpc_cache_key_t {
    lpc_cache_key_data_t data;
    time_t mtime;                 /* the mtime of this cached entry */
    unsigned char type;
    unsigned char md5[16];        /* md5 hash of the source file */
};


typedef struct lpc_keyid_t lpc_keyid_t;

struct lpc_keyid_t {
    unsigned int h;
    unsigned int keylen;
    time_t mtime;
#ifdef ZTS
    THREAD_T tid;
#else
    pid_t pid;
#endif
};
/* }}} */

/* {{{ struct definition: lpc_cache_entry_t */
typedef union _lpc_cache_entry_value_t {
    struct {
        char *filename;             /* absolute path to source file */
        zend_op_array* op_array;    /* op_array allocated in shared memory */
        lpc_function_t* functions;  /* array of lpc_function_t's */
        lpc_class_t* classes;       /* array of lpc_class_t's */
        long halt_offset;           /* value of __COMPILER_HALT_OFFSET__ for the file */
    } file;
} lpc_cache_entry_value_t;

typedef struct lpc_cache_entry_t lpc_cache_entry_t;
struct lpc_cache_entry_t {
    lpc_cache_entry_value_t data;
    unsigned char type;
    int ref_count;
    size_t mem_size;
    lpc_pool *pool;
};
/* }}} */

/*
 * lpc_cache_create creates the shared memory compiler cache. This function
 * should be called just once (ideally in the web server parent process, e.g.
 * in apache), otherwise you will end up with multiple caches (which won't
 * necessarily break anything). Returns a pointer to the cache object.
 *
 * size_hint is a "hint" at the total number of source files that will be
 * cached. It determines the physical size of the hash table. Passing 0 for
 * this argument will use a reasonable default value.
 *
 * gc_ttl is the maximum time a cache entry may speed on the garbage
 * collection list. This is basically a work around for the inherent
 * unreliability of our reference counting mechanism (see lpc_cache_release).
 *
 * ttl is the maximum time a cache entry can idle in a slot in case the slot
 * is needed.  This helps in cleaning up the cache and ensuring that entries 
 * hit frequently stay cached and ones not hit very often eventually disappear.
 */
extern T lpc_cache_create(TSRMLS_D);

/*
 * lpc_cache_destroy releases any OS resources associated with a cache object.
 * Under apache, this function can be safely called by the child processes
 * when they exit.
 */
extern void lpc_cache_destroy(lpc_cache_t *cache TSRMLS_DC);

/*
 * lpc_cache_clear empties a cache. This can safely be called at any time,
 * even while other server processes are executing cached source files.
 */
extern void lpc_cache_clear(T cache TSRMLS_DC);

/*
 * lpc_cache_insert adds an entry to the cache, using a filename as a key.
 * Internally, the filename is translated to a canonical representation, so
 * that relative and absolute filenames will map to a single key. Returns
 * non-zero if the file was successfully inserted, 0 otherwise. If 0 is
 * returned, the caller must free the cache entry by calling
 * lpc_cache_free_entry (see below).
 *
 * key is the value created by lpc_cache_make_file_key for file keys.
 *
 * value is a cache entry returned by lpc_cache_make_entry (see below).
 */
extern int lpc_cache_insert(T cache, lpc_cache_key_t key,
                            lpc_cache_entry_t* value, lpc_context_t* ctxt, time_t t TSRMLS_DC);

extern int *lpc_cache_insert_mult(lpc_cache_t* cache, lpc_cache_key_t* keys,
                            lpc_cache_entry_t** values, lpc_context_t *ctxt, time_t t, int num_entries TSRMLS_DC);

/*
 * lpc_cache_find searches for a cache entry by filename, and returns a
 * pointer to the entry if found, NULL otherwise.
 *
 * key is a value created by lpc_cache_make_file_key for file keys.
 */
extern lpc_cache_entry_t* lpc_cache_find(T cache, lpc_cache_key_t key, time_t t TSRMLS_DC);

/*
 * lpc_cache_release decrements the reference count associated with a cache
 * entry. Calling lpc_cache_find automatically increments the reference count,
 * and this function must be called post-execution to return the count to its
 * original value. Failing to do so will prevent the entry from being
 * garbage-collected.
 *
 * entry is the cache entry whose ref count you want to decrement.
 */
extern void lpc_cache_release(T cache, lpc_cache_entry_t* entry TSRMLS_DC);

/*
 * lpc_cache_make_file_key creates a key object given a relative or absolute
 * filename and an optional list of auxillary paths to search. include_path is
 * searched if the filename cannot be found relative to the current working
 * directory.
 *
 * key points to caller-allocated storage (must not be null).
 *
 * filename is the path to the source file.
 *
 * include_path is a colon-separated list of directories to search.
 *
 * and finally we pass in the current request time so we can avoid
 * caching files with a current mtime which tends to indicate that
 * they are still being written to.
 */
extern int lpc_cache_make_file_key(lpc_cache_key_t* key,
                                   const char* filename,
                                   const char* include_path,
                                   time_t t
                                   TSRMLS_DC);

/*
 * lpc_cache_make_file_entry creates an lpc_cache_entry_t object given a filename
 * and the compilation results returned by the PHP compiler.
 */
extern lpc_cache_entry_t* lpc_cache_make_file_entry(const char* filename,
                                                    zend_op_array* op_array,
                                                    lpc_function_t* functions,
                                                    lpc_class_t* classes,
                                                    lpc_context_t* ctxt
                                                    TSRMLS_DC);


zend_bool lpc_compile_cache_entry(lpc_cache_key_t key, zend_file_handle* h, int type, time_t t, zend_op_array** op_array_pp, lpc_cache_entry_t** cache_entry_pp TSRMLS_DC);

/* {{{ struct definition: slot_t */
typedef struct slot_t slot_t;
struct slot_t {
    lpc_cache_key_t key;        /* slot key */
    lpc_cache_entry_t* value;   /* slot value */
    slot_t* next;               /* next slot in linked list */
    time_t creation_time;       /* time slot was initialized */
    time_t deletion_time;       /* time slot was removed from cache */
    time_t access_time;         /* time slot was last accessed */
};
/* }}} */

/* {{{ struct definition: cache_header_t
   Any values that must be shared among processes should go in here. */
typedef struct cache_header_t cache_header_t;
struct cache_header_t {
    slot_t* deleted_list;       /* linked list of to-be-deleted slots */
    time_t start_time;          /* time the above counters were reset */
    int num_entries;            /* Statistic on the number of entries */
    size_t mem_size;            /* Statistic on the memory size used by this cache */
    lpc_keyid_t lastkey;        /* the key that is being inserted (user cache) */
};
/* }}} */

/* {{{ struct definition: lpc_cache_t */
struct lpc_cache_t {
    void* addr;                   /* process (local) address of now private previously shared cache */
    cache_header_t* header;       /* cache header (stored in SHM) */
    slot_t** slots;               /* array of cache slots (stored in SHM) */
    int num_slots;                /* number of slots in cache */
};
/* }}} */

extern zval* lpc_cache_info(T cache, zend_bool limited TSRMLS_DC);

#undef T
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
