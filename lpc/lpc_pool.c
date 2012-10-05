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

/* $Id: lpc_pool.c 307328 2011-01-10 06:21:53Z gopalv $ */


#include "lpc_pool.h"
#include <assert.h>

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

/* {{{ lpc_pool_destroy */
void lpc_pool_destroy(lpc_pool *pool TSRMLS_DC)
{
    lpc_free_t deallocate = pool->deallocate;
    lpc_pcleanup_t cleanup = pool->cleanup;

    cleanup(pool TSRMLS_CC);
    deallocate(pool TSRMLS_CC);
}
/* }}} */

static void* _lpc_pool_alloc(lpc_pool* pool, size_t size TSRMLS_DC) 
{
    lpc_malloc_t allocate = pool->allocate;

    pool->size += size;
    pool->used += size;

    return allocate(size TSRMLS_CC);
}

static void _lpc_pool_free(lpc_pool* pool, void *ptr TSRMLS_DC)
{
    lpc_free_t deallocate = ((lpc_pool*) pool)->deallocate;
    deallocate(ptr TSRMLS_CC);
}

static void _lpc_pool_cleanup(lpc_pool* pool TSRMLS_DC)
{
}

lpc_pool* lpc_pool_create(lpc_malloc_t allocate, lpc_free_t deallocate TSRMLS_DC)
{
    lpc_pool* pool = allocate(sizeof(lpc_pool) TSRMLS_CC);

    if (!pool) {
        return NULL;
    }

    pool->allocate   = allocate;
    pool->deallocate = deallocate;

    pool->palloc     = _lpc_pool_alloc;
    pool->pfree      = _lpc_pool_free;
    pool->cleanup    = _lpc_pool_cleanup;

    pool->used       = 0;
    pool->size       = 0;

    return pool;
}
/* }}} */

/* {{{ lpc_pstrdup */
void* LPC_ALLOC lpc_pstrdup(const char* s, lpc_pool* pool TSRMLS_DC)
{
    return s != NULL ? lpc_pmemcpy(s, (strlen(s) + 1), pool TSRMLS_CC) : NULL;
}
/* }}} */

/* {{{ lpc_pmemcpy */
void* LPC_ALLOC lpc_pmemcpy(const void* p, size_t n, lpc_pool* pool TSRMLS_DC)
{
    void* q;

    if (p != NULL && (q = lpc_pool_alloc(pool, n)) != NULL) {
        memcpy(q, p, n);
        return q;
    }
    return NULL;
}
/* }}} */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
