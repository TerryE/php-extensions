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


#include "lpc.h"
#include "lpc_pool.h"
#include "lpc_debug.h"

#include <assert.h>

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

/* The APC REALPOOL and BDPOOL implementations used a hastable to track individual 
 * elements and for the DTOR.  However, since the element DTOR is private, a simple
 * linked list is maintained as a prefix to individual elements enabling the pool
 * DTOR to chain down this to cleanup.
 */

/* emalloc but relay the calling location reference */
#define pool_emalloc(size) _emalloc((size) ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_EMPTY_CC)

#define BD_ALLOC_UNIT 16384*sizeof(void *)

/* {{{ _lpc_pool_create */
lpc_pool* _lpc_pool_create(lpc_pool_type_t type TSRMLS_DC ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_create)
    lpc_pool *pool;

	switch(type) {
 
		case LPC_LOCALPOOL:
		case LPC_SHAREDPOOL:
		case LPC_SERIALPOOL:
			break;

		default:
#ifdef ZEND_DEBUG
			lpc_error("Invalid pool type %d at %s:%d" TSRMLS_CC, type ZEND_FILE_LINE_RELAY_CC);
#else
			lpc_error("Invalid pool type %d" TSRMLS_CC type);
#endif
			return 0;
	}

    pool = pool_emalloc(sizeof(lpc_pool));

#ifdef APC_DEBUG
	pool->orig_filename = estrdup(__zend_filename);
	pool->orig_lineno   = __zend_lineno;
#endif

	pool->type  = type;
    pool->size  = 0;
    pool->count = 0;

	pool->element_head = NULL;
	pool->element_tail = (type == LPC_SERIALPOOL) ? NULL :  &(pool->element_head);
	pool->bd_storage   = NULL;
	pool->bd_allocated = 0;
	pool->bd_next_free = 0;

    zend_hash_next_index_insert(&LPCG(pools), &pool, sizeof(lpc_pool *), NULL);
    return pool;
}
/* }}} */

/* {{{ _lpc_pool_destroy */
void _lpc_pool_destroy(lpc_pool *pool TSRMLS_DC ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_destroy)
	if (pool->type == LPC_SERIALPOOL) {
		efree(pool->bd_storage);
	} else if (pool->element_head != NULL) {
		void *e, *e_nxt;
		int i;
		/* Walk the linked list freeing the element storage */
		for (e = pool->element_head, i=0 ; e != pool->element_tail; e = e_nxt) {
			e_nxt = *((void **) e);
lpc_debug("freeing pool element %4d at 0x%lx in pool 0x%lx" TSRMLS_CC, i++, (unsigned long) e, (unsigned long) pool); 
			efree(e);
		}
	}

#ifdef APC_DEBUG
	efree(pool->orig_filename);
#endif

    efree(pool);
}
/* }}} */

/* {{{ _lpc_pool_set_size */
void* _lpc_pool_set_size(lpc_pool* pool, size_t size TSRMLS_DC ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_set_size)
 	if (pool->type == LPC_SERIALPOOL && pool->bd_storage == NULL) {
		size_t rounded_size = (size + sizeof(void *) - 1) & (~((size_t) (sizeof(void *)-1)));
		pool->bd_storage   = pool_emalloc(rounded_size);
		pool->bd_allocated = rounded_size;
	}
}
/* }}} */

/* {{{ _lpc_pool_alloc */
void* _lpc_pool_alloc(lpc_pool* pool, size_t size TSRMLS_DC ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_alloc)
	void *element;

 	if (pool->type == LPC_SERIALPOOL) {
		/* Unlike APC, LPC explicitly rounds up any sizes to the next sizeof(void *)
		 * boundary as it would be nice for this code to work on ARM without barfing */
		size_t rounded_size = (size + sizeof(void *) - 1) & (~((size_t) (sizeof(void *)-1)));
		rounded_size += sizeof(off_t);

		/* do an initial allocation of BD_ALLOC_UNIT if size hasn't already been set. */

		if (pool->bd_storage == NULL) {
			pool->bd_storage   = pool_emalloc(BD_ALLOC_UNIT);
			pool->bd_allocated = BD_ALLOC_UNIT;
		}

		/* grow the serial storage by another alloc unit size if necessary */
		if (pool->bd_next_free + rounded_size > pool->bd_allocated) {
			pool->bd_allocated += BD_ALLOC_UNIT;
			pool->bd_storage    = erealloc(pool->bd_storage, pool->bd_allocated);
		}

		element = (unsigned char *) pool->bd_storage + (off_t) pool->bd_next_free;
		pool->bd_next_free += rounded_size;
		*((size_t *) element) = pool->bd_next_free;   /* PIC offset of next element */
		element = ((size_t *) element)+1;             /* bump past offset to element itself */

	} else {  /* LPC_LOCALPOOL or LPC_SHAREDPOOL */

		element = pool_emalloc(sizeof(void *) + size);
		*((void **)pool->element_tail) = element;    /* chain last element to this one */
		pool->element_tail = element;		         /* and update tail pointer */
		element = ((void **) element)+1;             /* bump past link to element itself */

	}

#if LPC_BINDUMP_DEBUG & 0
			lpc_notice("lpc_bd_alloc: rval: 0x%x  offset: 0x%x  pool_size: 0x%x  size: %d" TSRMLS_CC, 
			            (ulong) element, pool->bd_next_free, pool->bd_pool_size, size);
#endif

    pool->size += size;
    pool->count++;

    return element;
}
/* }}} */

/* There is no longer any lpc_pool_free function as the only free operation is in the pool DTOR */

/* {{{ lpc_pstrdup */

void* _lpc_pool_strdup(const char* s, lpc_pool* pool TSRMLS_DC ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_strdup)
    return (s != NULL) ? 
		_lpc_pool_memcpy(s, (strlen(s) + 1), pool TSRMLS_CC ZEND_FILE_LINE_RELAY_CC) : 
		NULL;
}
/* }}} */

/* {{{ lpc_pmemcpy */
void* _lpc_pool_memcpy(const void* p, size_t n, lpc_pool* pool TSRMLS_DC ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_memcpy)
    void* q;

    if (p != NULL && 
		(q = _lpc_pool_alloc(pool, n TSRMLS_CC ZEND_FILE_LINE_RELAY_CC)) != NULL) {
        memcpy(q, p, n);
        return q;
    }
    return NULL;
}
/* }}} */

#define pool_emalloc(size) _emalloc((size) ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_EMPTY_CC)
/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
