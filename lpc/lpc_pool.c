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
	void *dummy = (void *) 1;

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
	if (type == LPC_SERIALPOOL) {
		/* create a hash table to collect relocation addresses */
		pool->bd_fixups = emalloc(sizeof(HashTable));
		zend_hash_init(pool->bd_fixups, 512, NULL, NULL, 0);
	}
	
	/* The pools hash table has entries whose keys are the addresss of the existing pools */ 
	zend_hash_index_update(&LPCG(pools), (ulong) pool, &dummy, sizeof(void *), NULL);

    return pool;
}
/* }}} */

static void _lpc_pool_fixup_report(lpc_pool *pool TSRMLS_DC);

/* {{{ _lpc_pool_destroy */
void _lpc_pool_destroy(lpc_pool *pool TSRMLS_DC ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_destroy)
	if (pool->type == LPC_SERIALPOOL) {
		_lpc_pool_fixup_report(pool TSRMLS_CC);
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
	zend_hash_index_del(&LPCG(pools), (ulong) pool);
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

typedef void *pointer;
#define POINTER_MASK (sizeof(pointer)-1)
/* {{{ _lpc_pool_tag_ptr */
void *_lpc_pool_tag_ptr(void *ptr, lpc_pool* pool TSRMLS_DC ZEND_FILE_LINE_DC)
{
	/* Note that this code assumes that all pointers are correctly storage-aligned */
	off_t bd_max = pool->bd_allocated / sizeof(pointer);
	off_t ptr_offset = ((pointer *)ptr - (pointer *)pool->bd_storage);

	if (pool->type != LPC_SERIALPOOL) {
		return pool;
	}
	if (ptr == NULL) {
		lpc_debug("check: Null relocation ptr call at %s:%u" TSRMLS_CC, 
			       ptr ZEND_FILE_LINE_RELAY_CC);
		return pool;

	assert(((ulong)ptr & POINTER_MASK) == 0);
	}
	/* The Lvalue address should lie inside the pool and the address not already been requested */
	if (ptr_offset > 0 && ptr_offset < pool->bd_allocated) {

		if (!zend_hash_index_exists(pool->bd_fixups, (ulong) ptr_offset)) {

			/* HOWEVER the location pointed to can be on any alignment, so this is a byte offset */
			off_t addr_offset = *(char **)ptr - (char *)pool->bd_storage;
			if (addr_offset > 0 && addr_offset < pool->bd_allocated) {

				/* The pointer is internal to the pool and therefore one to be relocated */
				void *dummy = (void *) 1;  /* std dummy value -- only want the key */
				zend_hash_index_update(pool->bd_fixups, (ulong) ptr_offset, &dummy, sizeof(void *), NULL);

				lpc_debug("check: 0x%08lx (0x%08lx + 0x%08lx) Inserting relocation addr to 0x%08lx call at %s:%u"
					 TSRMLS_CC, ptr, pool->bd_storage, ptr_offset,  *(void **)ptr ZEND_FILE_LINE_RELAY_CC);

			} else if (*(void **)ptr != NULL){
				lpc_debug("check: 0x%08lx Addr to external 0x%08lx call at %s:%u"
					 TSRMLS_CC, ptr, *(void **)ptr ZEND_FILE_LINE_RELAY_CC);
			}

		} else {
			lpc_debug("check: 0x%08lx Duplicate relocation addr call at %s:%u" TSRMLS_CC, 
			           ptr ZEND_FILE_LINE_RELAY_CC);
		}

	} else {
		lpc_debug("check: 0x%08lx Relocation addr call outside pool at %s:%u" TSRMLS_CC, 
		           ptr ZEND_FILE_LINE_RELAY_CC);
	}
	return pool;
}

/* }}} */

static int key_compare(const void *a, const void *b TSRMLS_DC) /* {{{ */
{
	long diff = ((Bucket*)a)->h - ((Bucket*)b)->h;
	if (diff > 0) return 1;
	if (diff < 0) return -1;
	return 0;
}
 

static void _lpc_pool_fixup_report(lpc_pool *pool TSRMLS_DC)
{
	off_t reloc_off;
	off_t bd_max = pool->bd_allocated / sizeof(pointer);
	pointer *p = (pointer *) pool->bd_storage;
	void *dummy = (void *)1;
	int i, j = 0;

	/* sort the fixups table into ascending offset order */
	zend_hash_sort(pool->bd_fixups, zend_qsort, key_compare, 0 TSRMLS_CC);

    lpc_debug("=== Check Report ====" TSRMLS_CC);
	/* loop over reported pointers */
	for (zend_hash_internal_pointer_reset(pool->bd_fixups); 
		 zend_hash_get_current_key(pool->bd_fixups, (char **) &dummy, (ulong *)&reloc_off, 0) == HASH_KEY_IS_LONG;
		 zend_hash_move_forward(pool->bd_fixups)) {

		char **ptr = (char **) ((void **)p + reloc_off);
		off_t addr_off = *ptr - (char *)(pool->bd_storage);

		lpc_debug("check:  0x%08lx addr = 0x%08lx %1c" TSRMLS_CC, 
		          ptr, *ptr, 
		          (addr_off < 0 || addr_off > pool->bd_next_free) ? 'E' : ' ');
	}

    lpc_debug("=== Missed Report ====" TSRMLS_CC);

	/* Now flag up any pointers that have been missed */

	for (i = 0; i < bd_max; i++) {

		char **addr = (char **) (p[reloc_off]);
		off_t addr_off = *addr - (char *)(pool->bd_storage);

		if (addr_off > 0 && (addr_off < pool->bd_next_free) &&
		    !zend_hash_index_exists(pool->bd_fixups, (ulong) reloc_off)) {

			lpc_debug("missed: 0x%08lx addr = 0x%08lx" TSRMLS_CC, 
				      addr, *addr);
			j++;
		}
	}
	lpc_debug("=== Missed Totals ==== %u from %u" TSRMLS_CC, j, i);
	zend_hash_destroy(pool->bd_fixups);
	efree(pool->bd_fixups);
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
