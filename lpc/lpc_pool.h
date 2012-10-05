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

/* $Id: lpc_pool.h 307048 2011-01-03 23:53:17Z kalle $ */

#ifndef LPC_POOL_H
#define LPC_POOL_H

#include "lpc.h"

/* #define LPC_POOL_DEBUG 1 */

typedef enum {
    LPC_UNPOOL         = 0x0,
    LPC_SMALL_POOL     = 0x1,
    LPC_MEDIUM_POOL    = 0x2,
    LPC_LARGE_POOL     = 0x3,
    LPC_POOL_SIZE_MASK = 0x7,   /* waste a bit */
#if LPC_POOL_DEBUG
    LPC_POOL_REDZONES  = 0x08,
    LPC_POOL_SIZEINFO  = 0x10,
    LPC_POOL_OPT_MASK  = 0x18
#endif
} lpc_pool_type;

#if LPC_POOL_DEBUG
#define LPC_POOL_HAS_SIZEINFO(pool) ((pool->type & LPC_POOL_SIZEINFO)!=0)
#define LPC_POOL_HAS_REDZONES(pool) ((pool->type & LPC_POOL_REDZONES)!=0)
#else
/* let gcc optimize away the optional features */
#define LPC_POOL_HAS_SIZEINFO(pool) (0)
#define LPC_POOL_HAS_REDZONES(pool) (0)
#endif


typedef struct _lpc_pool lpc_pool;

typedef void  (*lpc_pcleanup_t)(lpc_pool *pool TSRMLS_DC);
typedef void* (*lpc_palloc_t)(lpc_pool *pool, size_t size TSRMLS_DC);
typedef void  (*lpc_pfree_t) (lpc_pool *pool, void* p TSRMLS_DC);
typedef void* (*lpc_protect_t)  (void *p);
typedef void* (*lpc_unprotect_t)(void *p);

struct _lpc_pool {
    lpc_malloc_t    allocate;
    lpc_free_t      deallocate;

    lpc_palloc_t    palloc;
    lpc_pfree_t     pfree;

    lpc_pcleanup_t  cleanup;

    size_t          size;
    size_t          used;
};

#define lpc_pool_alloc(pool, size)  ((void *) pool->palloc(pool, size TSRMLS_CC))
#define lpc_pool_free(pool, ptr) 	((void)   pool->pfree (pool, ptr TSRMLS_CC))

extern lpc_pool* lpc_pool_create(lpc_malloc_t allocate, lpc_free_t deallocate TSRMLS_DC);
extern void lpc_pool_destroy(lpc_pool* pool TSRMLS_DC);
void* LPC_ALLOC lpc_pstrdup(const char* s, lpc_pool* pool TSRMLS_DC);
void* LPC_ALLOC lpc_pmemcpy(const void* p, size_t n, lpc_pool* pool TSRMLS_DC);

#endif
