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
  | Authors: Gopal Vijayaraghavan <gopalv@yahoo-inc.com>                 |
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Yahoo! Inc. in 2008.

   Future revisions and derivatives of this source code must acknowledge
   Yahoo! Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.

*/

#ifndef LPC_POOL_H
#define LPC_POOL_H

#include "zend.h"

/* A pool is a group of dynamically allocated memory objects with a common set of properties:
 *
 *   *  They share the same memory alloc and free methods
 *   *  They are individually be created (and destroyed) by the same pool methods
 *   *  The pool DTOR will also deallocate any remaining elements in the pool.
 *
 * In the APC/LPC implementation, all element destruction is carried out (or should be) by the pool
 * DTOR, and so the element DTOR is private to the pool.
 *
 * In APC the term "REALPOOL" and "UNPOOL" were used for pools allocated in shared memory and PHP 
 * emalloc storage (as the latter was freed by the Execution engine.  A variant of this latter with  
 * a serial "BD" allocator was use for the binary load/dump routines.  The LPC implementation uses
 * two pool types, both of which are maintained in thread-local memory: */
typedef enum {
    LPC_EXECPOOL      = 0x0,  /* The Zend execution environment handles memory recovery */
	LPC_SERIALPOOL    = 0x1,  /* A pool in which all storage is in contiguous blocks */
	LPC_RO_SERIALPOOL = 0x2   /* A pool in which all storage is in contiguous blocks */
} lpc_pool_type_t;
/*
 * The public interface to the pool is encapsulated in the following macros.  Note that all 
 * pool elements must be created through one of the constructors, and that no public element
 * destructor exists.  To keep these simple, all the macros assume that the variable pool
 * exists and is in scope and points to the current pool.  Also since the pool is only 
 * used specific to a given thread, the TSRMLS pointer is moved into the pool to simplify
 * argument overheads.
 */
#define pool_create(type) ((lpc_pool *) _lpc_pool_create(type TSRMLS_CC ZEND_FILE_LINE_CC))
#define pool_destroy()  _lpc_pool_destroy(pool ZEND_FILE_LINE_CC)
#define pool_unload(bp,size) _lpc_pool_unload(pool, bp, size ZEND_FILE_LINE_CC)
#define pool_load(buf,buflen) _lpc_pool_load(buf, buflen TSRMLS_CC ZEND_FILE_LINE_CC);
/*
 * The allocator macros have a different layout since in the case of Serial kind, the pool 
 * must identify any intra-pool pointers, so that it can relocate them on reload as the 
 * base address of the reloaded pool will be different to that of the saved pool. There are
 * also convenience forms for allocation of zvals, HashTables and string storage to dovetail
 * into the zend fast allocators for these.
 */ 
#define pool_alloc(dest, size)  _lpc_pool_alloc((void **)&(dest), pool, size ZEND_FILE_LINE_CC)
#define pool_alloc_zval(dest) _lpc_pool_alloc_zval((void **)&(dest), pool ZEND_FILE_LINE_CC)
#define pool_alloc_ht(dest) _lpc_pool_alloc_ht((void **)&(dest), pool ZEND_FILE_LINE_CC)
#define pool_alloc_unaligned(dest,size) _lpc_pool_alloc_unaligned((void **)&(dest), size, pool ZEND_FILE_LINE_CC)
#define pool_strdup(dst,src)  _lpc_pool_strdup((void **)&(dst), (src), pool ZEND_FILE_LINE_CC)
#define pool_memcpy(dst,src,n) _lpc_pool_memcpy((void **)&(dst),src,n,pool ZEND_FILE_LINE_CC)
#define is_exec_pool() (pool->type == LPC_EXECPOOL)
#define pool_get_entry_rec() _lpc_pool_get_entry_rec(pool)
#define pool_tag_ptr(p) _lpc_pool_tag_ptr((void **)&(p), pool ZEND_FILE_LINE_CC);

#define is_copy_out()  (pool->type != LPC_EXECPOOL)
#define is_copy_in()   (pool->type == LPC_EXECPOOL)

/* The pool is implemented with the following type and extern calls */

typedef struct _lpc_pool_brick lpc_pool_brick;
typedef struct _lpc_pool {
#ifdef ZTS
	void         ***tsrm_ls;		 /* the thread context in ZTS builds */
#endif
	lpc_pool_type_t type;           
    size_t          size;            /* sum of individual element sizes */
	uint			count;           /* count of pool elements*/
#ifdef APC_DEBUG
	char           *orig_filename;   /* plus the file-line creator in debug builds */
	uint            orig_lineno;
#endif
	/* The following fields are only used for serial pools */
	uint			brick_count;
	lpc_pool_brick *brickvec;		 /* array of allocated bricks (typically 1) */
	lpc_pool_brick *brick;           /* current brick -- a simple optimization */
	size_t          available;       /* bytes available in current brick -- ditto */
	struct {                         /* tag hash */
	size_t         *hash;
	size_t          size;
	ulong			mask;
	uint			count;			
	}               tag;
	unsigned char  *reloc;			 /* byte relocation vector */
} lpc_pool;

extern lpc_pool* _lpc_pool_create(lpc_pool_type_t type TSRMLS_DC ZEND_FILE_LINE_DC);
extern lpc_pool* _lpc_pool_load(void* pool_buf, size_t pool_buf_len TSRMLS_DC ZEND_FILE_LINE_DC);
/* All the remaining pool functions pass the pool as an arg and therefore don't need the TSRMLS_DC */
extern void _lpc_pool_destroy(lpc_pool* pool ZEND_FILE_LINE_DC);
extern void _lpc_pool_alloc(void **dest, lpc_pool* pool, size_t size ZEND_FILE_LINE_DC);
extern void _lpc_pool_alloc_zval(void **dest, lpc_pool* pool ZEND_FILE_LINE_DC);
extern void _lpc_pool_alloc_ht(void **dest, lpc_pool* pool ZEND_FILE_LINE_DC);
extern void _lpc_pool_strdup(void **dest, const char* s, lpc_pool* pool ZEND_FILE_LINE_DC);
extern void _lpc_pool_memcpy(void **dest, const void* p, size_t n, lpc_pool* pool ZEND_FILE_LINE_DC);
extern void _lpc_pool_tag_ptr(void **ptr, lpc_pool* pool ZEND_FILE_LINE_DC);
extern int  _lpc_pool_unload(lpc_pool* pool, void** pool_buffer, size_t* pool_size ZEND_FILE_LINE_DC);
extern void* _lpc_pool_get_entry_rec(lpc_pool* pool);

#endif /* LPC_POOL_H */
