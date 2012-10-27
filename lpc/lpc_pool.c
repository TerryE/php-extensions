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

#define LPC_DEBUG

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

#define BD_ALLOC_UNIT 131072*sizeof(void *)

typedef struct _lpc_shared_trailer {
	uint	count;
    size_t  size;
	size_t  allocated;
	off_t   reloc_vec;
	off_t   entry_off;
} lpc_shared_trailer;

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
		size_t rounded_size = (size + (sizeof(void *)-1)) & (~((size_t) (sizeof(void *)-1)));
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
//////////// TODO: Drop the element offset-linked list.  It isn't used anywhere.
//////////// TODO: Move to a brick based allocation scheme and use a non-aligned brick for strings

		element = (unsigned char *) pool->bd_storage + (off_t) pool->bd_next_free;
		pool->bd_next_free   += sizeof(size_t) + rounded_size;
		*((size_t *) element) = pool->bd_next_free;   /* PIC offset of next element */		
		element = (size_t*)element + 1;               /* bump past offset to element itself */

#ifdef LPC_DEBUG
		lpc_debug("alloc: 0x%08lx allocated 0x%04lx bytes" TSRMLS_CC, element, size);
#endif
	} else {  /* LPC_LOCALPOOL or LPC_SHAREDPOOL */

		element = pool_emalloc(sizeof(void *) + size);
		*((void **)pool->element_tail) = element;    /* chain last element to this one */
		pool->element_tail = element;		         /* and update tail pointer */
		element = ((void **) element)+1;             /* bump past link to element itself */

	}

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
/* {{{ _lpc_pool_tag_ptr 
       Used to tag any internal pointers within a serial pool to enable its relocation */
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

/* {{{ offset_compare
       Simple sort callback to enable sorting of an a vector of offsets into offset order  */
static int offset_compare(const void *a, const void *b)
{
	long diff = *(off_t *)a - *(off_t *)b;
	return (diff > 0) ? 1 : ((diff < 0) ? -1 : 0);
}
/* }}} */

/* {{{ proto int _lpc_pool_unload record pool, record &pool_buffer
       Unload and destroy a serial pool */ 
int _lpc_pool_unload(lpc_pool* pool, void **pool_buffer, void *entry TSRMLS_DC ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_unload)
	int            pool_buffer_len, reloc_vec_len, i, cnt; 
	off_t         *reloc_vec, reloc_off, ro, *q, *rv;
	HashTable     *fix_ht = pool->bd_fixups;
	void         **pool_mem = (void **) pool->bd_storage; /* that is treat the storage an array of pointers */
	unsigned char *reloc_bvec, *p;
	
	lpc_shared_trailer *trailer;
	
	void *dummy = (void *) 1;

	off_t entry_offset = *(char **)entry - (char *)pool->bd_storage;
	
	if (pool->type != LPC_SERIALPOOL ||
	    entry_offset > 0 && entry_offset < pool->bd_allocated ) {
		return FAILURE;
	}

	/* The fixups HashTable of offsets is now complete and the processing takes a few passes including resorting
	 * into offset order, so it's easier just to convert it into a contiguous vector and destroy the HT.
	 * Also a note on offsets.  All pointers are size_t aligned so offsets of the form (void **) - (void **) are
	 * valid and are the byte delta divided by sizeof(size_t) */
	reloc_vec_len = zend_hash_num_elements(fix_ht);
	rv = reloc_vec = emalloc(reloc_vec_len*sizeof(off_t));
	for (zend_hash_internal_pointer_reset(fix_ht); 
		 zend_hash_get_current_key(fix_ht, (char **) &dummy, (ulong *)&reloc_off, 0) == HASH_KEY_IS_LONG; 
		 zend_hash_move_forward(fix_ht)) {
		*rv++ = reloc_off;
	}
	zend_hash_destroy(fix_ht);
	efree(fix_ht);

	/* sort the vector into ascending offset order */
    qsort(reloc_vec, (size_t) reloc_vec_len, sizeof(off_t), offset_compare);

	/* Debug list all pointer in the pool memory to be fixed up */
#ifdef LPC_DEBUG
	for (i = 0; i < reloc_vec_len; i++) {
		lpc_debug("fixup: 0x%08lx points to 0x%08lx" TSRMLS_CC, pool_mem + reloc_vec[i], pool_mem[reloc_vec[i]]);
	}
#endif

	/* Now run along the relocation vector converting all pointers into internal pool locations in the pool into
     * the corresponding offset from the start of the pool by subtracting the pool memory addr.  This is now PIC */ 
	for (i = 0; i < reloc_vec_len; i++) {
		q   = (void *)(pool_mem + reloc_vec[i]);
		*q -= (off_t) pool_mem;
	}

	/* Convert relocation vector into delta offsets, d(i) = p(i-1) - p(i), in the range 1..reloc_vec_len-1
	 * and the easiest way to do this in place is to work backwards from the end. Note d(1) = p(1) - 0 */
	for (i = reloc_vec_len-1; i > 1; i--) {
		reloc_vec[i] -= reloc_vec[i-1];
	}

	/* The final relocation vector stored in the pool will be delta encoded one per byte.  In real PHP
	 * opcode arrays, pointers are a LOT denser than every 127 longs (1016 bytes), but the coding needs to 
	 * cope with pathelogical cases, so a simple high-bit multi-byte escape is used; hence delta offsets
	 * up to 131,064 bytes are encoded in 2 bytes, etc..  So this pass to works out the COUNT of any
	 * extra encoding bytes needed to enable a vector of the correct size to be in the pool.  The +1 is 
     * for the '\0' terminator. */
	for (i = 0, cnt = reloc_vec_len + 1; i < reloc_vec_len; i++) {
		for (ro=reloc_vec[i]; ro>=0x80; ro >>= 7, cnt++) { }
	}
	
	/* Now allocate the byte relocation vector and the fixed length trailer.  The pool is now complete */ 
	reloc_bvec = (unsigned char *) lpc_pool_alloc(pool, cnt);
	trailer    = (lpc_shared_trailer *) lpc_pool_alloc(pool, sizeof(lpc_shared_trailer));

	/* Loop over relocation vector again, now generating the relocation byte vector */
	for (i = 0, p = reloc_bvec; i < reloc_vec_len; i++) {
		if (reloc_vec[i]<128) { /* the typical case */
			*p++ = (unsigned char) (reloc_vec[i]);
		} else {                /* handle pathelogical cases */
			ro = reloc_vec[i];
			do { /* This could be optimzed, but keep it simple as it rarely happens */
				*p++ = 0x80 | ((unsigned char)(ro & 0x7f));
				ro >>= 7;
			} while (ro > 0);
		}
	}
	*p = '\0'; /* add an end marker so the reverse process knows when to terminate */

	/* fill in the trailer content.  Note that the the reloc_vec location has to be stored as an offset
	 * because must be relocatable and it can't be relocated using itself.  Also only the memory used,
     * that is up to bd_next_free will be allocated */
	trailer->reloc_vec = (off_t) ((void **)reloc_bvec - (void **)pool_mem);
	trailer->count     = pool->count;
	trailer->size      = pool->size;
	trailer->allocated = pool->bd_next_free;
	trailer->entry_off = entry_offset;

	/* We're now done with the reloc_vec, so destroy it */
	efree(reloc_vec);

#ifdef LPC_DEBUG
	/* As a safety measure scan the pool for any "untagged" internal aligned pointers that have missed 
     * the PIC adjustment.  These scan will miss any unaligned pointers which x86 tolerates at a 
	 * performance hit, but since PHP is multi-architecture (e.g ARM barfs on unaligned pointers), all
     * opcode array pointer are correctly aligned.  */
	if (1) {
		off_t ptr_max = trailer->allocated / sizeof(pointer);
		int i, j;

		lpc_debug("=== Missed Report ====" TSRMLS_CC);
		for (i = 0, j = 0, q = (off_t *)pool_mem; i < ptr_max; i++, q++) {
			off_t offset = *q - (off_t)pool_mem;
			if (offset > 0 && (offset < trailer->allocated)) {
				lpc_debug("missed: 0x%08lx points to 0x%08lx" TSRMLS_CC, q, *q);
				j++;
			}
		}
		lpc_debug("=== Missed Totals ==== %u from %u" TSRMLS_CC, j, i);
	}
#endif /* LPC_DEBUG */

	/* Allocate an output serial buffer and copy the sized contents. then destroy the pool */

	pool_buffer_len = trailer->allocated;
	*pool_buffer    = (void *) emalloc(pool_buffer_len);
	memcpy(*pool_buffer, pool_mem, pool_buffer_len);

	lpc_pool_destroy(pool);

	/* The serialised PIC version is passed back to the calling program */

	return pool_buffer_len;
}

void *_lpc_pool_load(void* pool_buffer, size_t pool_buffer_length TSRMLS_DC ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_load)
	lpc_pool* pool;
	void *entry;
	/////////////////////// TODO: this content
	return entry;
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
