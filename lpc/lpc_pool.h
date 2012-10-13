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

/* $Id: $ */

#ifndef LPC_POOL_H
#define LPC_POOL_H

#include "zend.h"

/* A pool is a group of dynamically allocated memory objects with a common set of properties:
 *   *  They share the same memory alloc and free methods
 *   *  They are individually be created (and destroyed) by the same pool methods
 *   *  The pool DTOR will also deallocate any remaining elements in the pool.
 *
 * In fact in the APC/LPC implementation, all element destruction is carried out (or should    
 * be) by the pool DTOR, and hence the element DTOR is private to the pool.
 *
 * In APC the term "REALPOOL" was used for pools allocated in shared memory.  The concept of 
 * an "UNPOOL" also existed and was considered to be a dummy pool in local memory also
 * a variant of unpool used a serial "BD" allocator.  No DTOR was implemented for unpools 
 * resulting in memory leakage of the unpool contents on destruction.  For REAL and DB pools, a 
 * HashTable was used to track allocation items so that destruction can automatically clean up.
 * 
 * For the LPC implementation I have adopted three pool types */
typedef enum {
    LPC_LOCALPOOL   = 0x0,  /* A pool in the process private address space */
    LPC_SHAREDPOOL  = 0x1,  /* A pool that was in SMA in APC (but implemented as LOCAL in LPC) */
	LPC_SERIALPOOL  = 0x2   /* A variant of LOCAL in which all storage is in a contiguous block */
} lpc_pool_type_t;
/*
 * In LPC there is no functional difference between LOCAL and SHARED pools; this distinction 
 * is only maintained to facilitate regression of this implementation back into APC.
 *
 * The public interface to the pool is encapsulated in the following macros.  Note that all 
 * pool elements must be created through one of the constuctors, and that no public element
 * destructor exists.  Also note that the allocator macros with "trsmls" parameters are framed this
 * way to maintain source compatibility with the existing code use and avoid unnecessary code changes.
 */
#define lpc_pool_create(type) ((lpc_pool *) _lpc_pool_create(type TSRMLS_CC ZEND_FILE_LINE_CC))
#define lpc_pool_destroy(pool)  _lpc_pool_destroy(pool TSRMLS_CC ZEND_FILE_LINE_CC)
#define lpc_pool_set_size(pool, size) _lpc_pool_set_size(pool, size TSRMLS_CC ZEND_FILE_LINE_DC)

#define lpc_pool_alloc(pool, size)  ((void *) _lpc_pool_alloc(pool, size TSRMLS_CC ZEND_FILE_LINE_CC))
#define lpc_pstrdup(s,ptrsmls)  ((void *) _lpc_pool_strdup((s),ptrsmls ZEND_FILE_LINE_CC))
#define lpc_pmemcpy(p,n,ptrsmls) ((void *) _lpc_pool_memcpy(p,n,ptrsmls ZEND_FILE_LINE_CC))

/* The pool is implemented with the following type and extern calls */

typedef struct _lpc_pool {
	lpc_pool_type_t type;
#ifdef APC_DEBUG
	char           *orig_filename;
	uint            orig_lineno;
#endif
	uint			count;              /* count of pool elements*/
    size_t          size;               /* sum of individual element sizes */
	void           *element_head;       /* only used for LOCAL & SHARED implementation */
    void           *element_tail;       /* only used for LOCAL & SHARED implementation */
	void           *bd_storage;         /* only used for SERIAL implementation */
	off_t           bd_next_free;       /* only used for SERIAL implementation */
	size_t          bd_allocated;       /* only used for SERIAL implementation */
} lpc_pool;

extern lpc_pool* _lpc_pool_create(lpc_pool_type_t type TSRMLS_DC ZEND_FILE_LINE_DC);
extern void _lpc_pool_destroy(lpc_pool* pool TSRMLS_DC ZEND_FILE_LINE_DC);
extern void* _lpc_pool_alloc(lpc_pool* pool, size_t size TSRMLS_DC ZEND_FILE_LINE_DC);
extern void* _lpc_pool_strdup(const char* s, lpc_pool* pool TSRMLS_DC ZEND_FILE_LINE_DC);
extern void* _lpc_pool_memcpy(const void* p, size_t n, lpc_pool* pool TSRMLS_DC ZEND_FILE_LINE_DC);

#endif /* LPC_POOL_H */
