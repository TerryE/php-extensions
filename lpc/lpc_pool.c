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

#define LPC_DEBUG

#include <zlib.h>
#include <assert.h>

#include "lpc.h"
#include "lpc_pool.h"
#include "lpc_debug.h"
/* 
 * The APC REALPOOL and BDPOOL implementations used a hastable to track individual 
 * elements and for the DTOR.  However, since the element DTOR is private, a simple
 * linked list is maintained as a prefix to individual elements enabling the pool
 * DTOR to chain down this to cleanup.
 *
 * Also note that the LPC pool implementation in debug mode uses Zend file-line relaying when
 * calling the underlying emalloc, ... routines so that the standard Zend leakage reports give
 * meaningful leakage reporting.  
 */

/* {{{ Pool defines and private structures/typdefs */
/* emalloc but relay the calling location reference */
#define POOL_EMALLOC(size) _emalloc((size) ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_EMPTY_CC)
#define POOL_TAG_PTR(p) _lpc_pool_tag_ptr(p, pool ZEND_FILE_LINE_RELAY_CC);
#define POOL_TYPE_STR() (pool->type == LPC_EXECPOOL ? "Exec" : "Serial")

#define BRICK_ALLOC_UNIT 128*1024*sizeof(void *)
#define POOL_TAG_HASH_INITIAL_SIZE 0x2000
#define NOT_SET ((ulong) -1)

static int size_shift;  /* a true global */

typedef struct _lpc_pool_brick {
	void           *storage;         /* pointer to brick memory */
    size_t          allocated;       /* storage allocated */
	size_t			base_offset;     /* offset from start of pool for start of brick */ 
} lpc_pool_brick;

struct _lpc_pool {
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
};

typedef struct _lpc_pool_header {
    size_t  size;
	size_t  allocated;
	size_t  reloc_vec;
	size_t	first_rec;	
	uint	count;
} lpc_pool_header;
/* }}} */

static int lpc_make_PIC_pool(lpc_pool *pool, unsigned char *rbvec);

static void *lpc_relocate_pool(size_t *pool_storage, size_t pool_size, unsigned char *rbvec, size_t offset);
static int lpc_pool_compress(lpc_pool *pool, unsigned char **outbuf);

/* {{{ _lpc_pool_create */
lpc_pool* _lpc_pool_create(lpc_pool_type_t type TSRMLS_DC ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_create)
    lpc_pool *pool;
	char *s;
	void *dummy = (void *) 1;

	if (type != LPC_EXECPOOL && type != LPC_SERIALPOOL) {
#ifdef ZEND_DEBUG
		lpc_error("Invalid pool type %d at %s:%d" TSRMLS_PC, type ZEND_FILE_LINE_RELAY_CC);
#else
		lpc_error("Invalid pool type %d" TSRMLS_PC, type);
#endif
		return 0;
	}
    size_shift = (sizeof(void*) == 8) ? 3 :2;

   /*
    * Calloc the pool structure and for serial pools, the pool header. Note that the initialisation
    * uses this zeroing and doesn't initialise variables to zero / null.
    */
    pool = POOL_EMALLOC(sizeof(lpc_pool));
	memset(pool, 0, sizeof(lpc_pool));
	pool->type  = type;
#if ZTS
	pool->tsrm_ls       = tsrm_ls;
#endif 
#ifdef LPC_DEBUG
	pool->orig_filename = estrdup(__zend_filename);
	pool->orig_lineno   = __zend_lineno;
	lpc_debug("%s pool created ptr at %s:%u" TSRMLS_CC, POOL_TYPE_STR() ZEND_FILE_LINE_RELAY_CC);
#endif

	if (type == LPC_SERIALPOOL) {
		lpc_pool_header *hdr;
	   /*
		* Allocate first brick to the serial storage and then the pool header.
		*/ 
		pool->brick_count        = 1;
		pool->available          = BRICK_ALLOC_UNIT;
		pool->brickvec           = emalloc(sizeof(lpc_pool_brick));
		pool->brick              = pool->brickvec;
		pool->brick->storage     = emalloc(BRICK_ALLOC_UNIT);
		pool->brick->base_offset = 0;
		pool->brick->allocated   = 0;
		_lpc_pool_alloc((void**) &hdr, pool, sizeof(lpc_pool_header) ZEND_FILE_LINE_RELAY_CC);
		memset(hdr, 0, sizeof(lpc_pool_header));
		hdr->first_rec           = pool->brick->allocated;
	}

	zend_hash_index_update(&LPCG(pools), (ulong) pool, &dummy, sizeof(void *), NULL);
    return pool;
}
/* }}} */

/* {{{ _lpc_pool_destroy */
void _lpc_pool_destroy(lpc_pool *pool ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_destroy)
	TSRMLS_FETCH_FROM_POOL();
	lpc_debug("destroy %s pool 0x%08lx (%u) at %s:%u" TSRMLS_PC, 
	          POOL_TYPE_STR(), pool, (uint) (pool->size) ZEND_FILE_LINE_RELAY_CC);

	if (pool->type == LPC_SERIALPOOL) {
		uint i; 
		if (pool->tag.hash) efree(pool->tag.hash);
		for (i=0; i < (pool->brick_count); i++) if ((pool->brick[i]).storage) efree((pool->brick[i]).storage);
		if (pool->brickvec) efree(pool->brickvec);
		if (pool->reloc) efree(pool->reloc);
	}

#ifdef LPC_DEBUG
	efree(pool->orig_filename);
#endif
	zend_hash_index_del(&LPCG(pools), (ulong) pool);
    efree(pool);
}
/* }}} */

/* {{{ _lpc_pool_is_exec */
int _lpc_pool_is_exec(lpc_pool* pool)
{
	return pool->type == LPC_EXECPOOL;
}
/* }}} */

/* {{{ _lpc_pool_get_entry_rec */
void * _lpc_pool_get_entry_rec(lpc_pool* pool)
{
	lpc_pool_header* hdr;
	if (pool->type == LPC_EXECPOOL) return NULL;	
	hdr = (lpc_pool_header*) pool->brick->storage;
	return  (void *)((char *)hdr + hdr->first_rec); 
}
/* }}} */

/* {{{ _lpc_pool_alloc */
void _lpc_pool_alloc(void **dest, lpc_pool* pool, size_t size ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_alloc)
	void *storage;

 	if (pool->type == LPC_SERIALPOOL) {
	   /*
	 	* Unlike APC, LPC explicitly rounds up any sizes to the next sizeof(void *) boundary
		* because there's quite a performance hit for unaligned access on CPUs such as x86 which
		* permit it, and it would alos be nice for this code to work on those such ARM which
		* don't without barfing.
		*/
		size_t rounded_size = (size + (sizeof(void *)-1)) & (~((size_t) (sizeof(void *)-1)));

		if (rounded_size < pool->available) {
			storage = (unsigned char *) pool->brick->storage + pool->brick->allocated;
			pool->brick->allocated  += rounded_size;
			pool->available         -= rounded_size;

 		} else {
			/*
			 * Add another allocation brick to the serial storage (this rarely happens), but in the
			 * case of loarge op_arrays the unit can exceed the brick size so allocate a brick large
			 * enough to take the array.
			 */ 
			lpc_pool_brick *p, *lastp;
			size_t          allocate = (rounded_size > BRICK_ALLOC_UNIT) ? rounded_size : BRICK_ALLOC_UNIT;

			pool->brick_count++;	
			pool->brickvec   = erealloc(pool->brickvec, pool->brick_count * sizeof (lpc_pool_brick));
			p                = pool->brickvec + (pool->brick_count-1); /* #1 === [0] ... */
			lastp            = pool->brickvec + (pool->brick_count-2);
			pool->brick		 = p;

			p->base_offset   = lastp->base_offset + lastp->allocated;
			p->storage       = storage = emalloc(allocate);
            p->allocated     = rounded_size;
			pool->available  = allocate - rounded_size;
		}

	} else if (pool->type == LPC_EXECPOOL) {
		storage = POOL_EMALLOC(sizeof(void *) + size);

    } else {/* LPC_RO_SERIALPOOL */
		lpc_error("Allocation operations are not permitted on a readonly Serial Pool" TSRMLS_PC);
		return;
	}

#ifdef LPC_DEBUG
		lpc_debug("%s alloc: 0x%08lx allocated 0x%04lx bytes at %s:%d" TSRMLS_PC, 
		           POOL_TYPE_STR(), storage, size ZEND_FILE_LINE_RELAY_CC);
#endif
    pool->size += size;
    pool->count++;

	if (dest) {
		*dest = storage;
		POOL_TAG_PTR(dest);
	}

    return;
}
/* }}} */

/* There is no longer any lpc_pool_free function as the only free operation is in the pool DTOR */

/* {{{ _lpc_pool_alloc_zval 
	   This is a special allocator that uses the Zend fast storage allocator in exec pools */

void _lpc_pool_alloc_zval(void **dest, lpc_pool* pool ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_alloc_zval)
    if (pool->type == LPC_EXECPOOL) {
		zval *z;
		ALLOC_ZVAL(z);
		if (dest) *dest = z;
#ifdef LPC_DEBUG
		lpc_debug("Exec alloc: 0x%08lx allocated zval" TSRMLS_PC, z);
#endif
	} else {
 		return _lpc_pool_alloc(dest, pool, sizeof(zval) ZEND_FILE_LINE_RELAY_CC);
	}
}
/* }}} */

/* {{{ _lpc_pool_alloc_zval
	   This is a special allocator that uses the Zend fast storage allocator in exec pools*/
void _lpc_pool_alloc_ht(void **dest, lpc_pool* pool ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_alloc_zval)
    if (pool->type == LPC_EXECPOOL) {
		HashTable *ht;
		ALLOC_HASHTABLE(ht);
		if (dest) *dest = ht;
#ifdef LPC_DEBUG
		lpc_debug("Exec alloc: 0x%08lx allocated HashTable" TSRMLS_PC, ht);
#endif
	} else {
 		return _lpc_pool_alloc(dest, pool, sizeof(HashTable) ZEND_FILE_LINE_RELAY_CC);
	}
}
/* }}} */

/* {{{ lpc_pstrdup */
void _lpc_pool_strdup(void **dest, const char* s, lpc_pool* pool ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_strdup)
    if (s) { 
		_lpc_pool_memcpy(dest, s, (strlen(s) + 1), pool ZEND_FILE_LINE_RELAY_CC); 
	}
}
/* }}} */

/* {{{ lpc_pmemcpy */
void _lpc_pool_memcpy(void **dest, const void* p, size_t n, lpc_pool* pool ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_memcpy)
    void* q;

    if (dest) {
		_lpc_pool_alloc(dest, pool, n ZEND_FILE_LINE_RELAY_CC);
		q = *dest;
	} else {
		_lpc_pool_alloc(&q, pool, n ZEND_FILE_LINE_RELAY_CC);
	}
    memcpy(q, p, n);
	}
/* }}} */


typedef void *pointer;
#define POINTER_MASK (sizeof(pointer)-1)
/* {{{ _lpc_pool_tag_ptr 
       Used to tag any internal pointers within a serial pool to enable its relocation */
void _lpc_pool_tag_ptr(void **ptr, lpc_pool* pool ZEND_FILE_LINE_DC)
{
	size_t target, offset, pool_offset, ndx;
	lpc_pool_brick *p;
	int i;
   /*
    * Handle obvious no-ops and errors
	*/
	if (ptr == NULL) {
		lpc_debug("check: Null relocation ptr call at %s:%u" TSRMLS_PC, 
			       ptr ZEND_FILE_LINE_RELAY_CC);
		return;
	}

	if (pool->type != LPC_SERIALPOOL || *ptr == NULL) {
		return;
	}

	assert(((ulong)ptr & POINTER_MASK) == 0);
	target = *(size_t *)ptr;
   /*
    * The majority of internal targets are to the current block so scan backwards from the current
    * allocation brick to check to see is the pointer target lies within the current storage
    * allocated to the serial pool. If so, it calculates the true offset relative to the pool
    * storage base Note that this code asserts that all pointers are correctly storage-aligned, so
    * all calculation is done as type size_t (the integer type which can hold a pointer). 
	*/
	pool_offset = NOT_SET;   /* set values are >0 so this is "not set" */

	for( i=0, p = pool->brick; i < pool->brick_count; i++, p--) {
		/* calculate offset into pool storage in sizeof pointer units */;
		offset = ((pointer *)ptr - (pointer *)p->storage); 
		if (offset < (p->allocated>>size_shift)) {
			pool_offset = p->base_offset + offset;
			break;
		}
	}	

	if (pool_offset == NOT_SET) {
#ifdef LPC_DEBUG
		lpc_debug("check: 0x%08lx Relocation addr call outside pool at %s:%u" TSRMLS_PC, 
			       ptr ZEND_FILE_LINE_RELAY_CC);
#endif
		return;
	}
   /*
	* A simple 50% max occupancy hash is used to track tagged pointers. Note that the FIND_NDX while
	* loop MUST terminate since 23,39 and 2^N are coprime. The standard PHP HashTable algo takes
	* up a lot of storage and can have poor insert performance, so this simple hash is faster and 
	* takes up a quarter of the storage. Also as the (pointerless) pool header is the first record
	* in the pool and this offset can't be zero, so zero can be used to denote an empty slot. 
    */
#define FIND_NDX(val) \
	ndx = ((ulong) val * 23) & pool->tag.mask; \
	while (pool->tag.hash[ndx] != 0 && pool->tag.hash[ndx] != val) { \
		ndx = (ndx + 39) & pool->tag.mask; \
	}

	if (!pool->tag.hash) { /* initialize on first use */
		pool->tag.size = POOL_TAG_HASH_INITIAL_SIZE;
		pool->tag.mask = pool->tag.size-1;
		pool->tag.hash = ecalloc(pool->tag.size, sizeof(size_t));
	}

	FIND_NDX(pool_offset);
	if (pool->tag.hash[ndx] == pool_offset) {
#ifdef LPC_DEBUG
		lpc_debug("check: 0x%08lx Duplicate relocation addr call at %s:%u" TSRMLS_PC, 
		           ptr ZEND_FILE_LINE_RELAY_CC);
#endif
	} else {
		pool->tag.hash[ndx] = pool_offset;
		pool->tag.count++;

		if (pool->count > (pool->tag.size>>1)) {
			/* Once the pool is half full, double storage and rehash */
			size_t *tmp = pool->tag.hash, *t = tmp, v; 
			pool->tag.size *= 2;
			pool->tag.hash = ecalloc(pool->tag.size, sizeof(size_t));
			for(i = 0; i < pool->tag.size; i++) {
				if ((v=*t++)>0) { 
					FIND_NDX(v); 
					pool->tag.hash[ndx] = v; 
				}
			}
			pool->tag.mask = pool->tag.size - 1;
			efree(tmp);
		}

#ifdef LPC_DEBUG
		lpc_debug("check: 0x%08lx (0x%08lx + 0x%04lx) Inserting relocation addr to 0x%08lx call at %s:%u"
			      TSRMLS_PC, ptr, p->storage, offset,  *(void **)ptr ZEND_FILE_LINE_RELAY_CC);
#endif
	}

}
/* }}} */

/* {{{ offset_compare
       Simple sort callback to enable sorting of an a vector of offsets into offset order  */
static int offset_compare(const void *a, const void *b)
{
	long diff = *(size_t *)a - *(size_t *)b;
	return (diff > 0) ? 1 : ((diff < 0) ? -1 : 0);
}
/* }}} */

/* {{{ proto int _lpc_pool_unload record pool, record &pool_buffer, long &pool_size
       Unload and destroy a serial pool */ 

int _lpc_pool_unload(lpc_pool* pool, void** pool_buffer, size_t* pool_size ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_unload)
	int            i, n, nmax, cnt, buffer_length;
	int			   reloc_vec_len = pool->tag.count;
	size_t        *reloc_vec = pool->tag.hash;
	size_t        *t, reloc_off, ro, *q, *rv;
	unsigned char *reloc_bvec, *p;
	TSRMLS_FETCH_FROM_POOL();

	if (pool->type != LPC_SERIALPOOL) {
		return FAILURE;
	}

	/* The pool header is the first allocated block in the pool storage */
	lpc_pool_header *hdr = (lpc_pool_header *)(pool->brick->storage);	
   /*
    * The fixups hash of offsets is now complete and the processing takes a few passes of the
    * offsets sorted into ascending offset order, so it's easier just to convert it into an ordered
    * contiguous vector in situ. To avoid confusion, the pointer variable reloc_vec is used to refer
    * to this contigous vector format. 
	*/
	for (n = 0, t = reloc_vec, nmax = pool->tag.size; n < nmax; n++) {
		if ( pool->tag.hash[n] ) {
			*t++ = pool->tag.hash[n];  /* this shuffles the non-zero values to the bottom of the hash */
		}
	}
    qsort(reloc_vec, pool->tag.count, sizeof(size_t), offset_compare);

	/* Convert relocation vector into delta offsets, d(i) = p(i-1) - p(i), in the range 1..reloc_vec_len-1
	 * and the easiest way to do this in place is to work backwards from the end. Note d(1) = p(1) - 0 */
	for (i = reloc_vec_len-1; i > 0; i--) {
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
   /*
    * Now allocate the byte relocation vector and the fixed length header.  Note that this destination 
	* address MUST be outside the pool as this allocation mustn't create another relocation entry. The 
	* pool is now complete.
	*/ 
	pool_alloc(reloc_bvec, cnt);

	/* Loop over relocation vector again, now generating the relocation byte vector */
	for (i = 0, p = reloc_bvec; i < reloc_vec_len; i++) {
		if (reloc_vec[i]<0x7f) { /* the typical case */
			*p++ = (unsigned char) (reloc_vec[i]);
		} else {                /* handle pathelogical cases */
			ro = reloc_vec[i];
		   /*
			* Emit multi-bytes lsb first with 7 bits sig. and high-bit set to indicate follow-on.
			*/
			*p++ = (unsigned char)(ro & 0x7f) | 0x80;
			if (ro < 0x3fff) {
		    	*p++ = (unsigned char)((ro>>7) & 0x7f);
			} else if (ro < 0x1fffff) {
		    	*p++ = (unsigned char)((ro>>7) & 0x7f) | 0x80;
		    	*p++ = (unsigned char)((ro>>14) & 0x7f);    
			} else if (ro < 0xfffffff) {
		    	*p++ = (unsigned char)((ro>>7) & 0x7f) | 0x80;
		    	*p++ = (unsigned char)((ro>>14) & 0x7f) | 0x80;
		    	*p++ = (unsigned char)((ro>>21) & 0x7f);    
 			} else {
				lpc_error("Fatal: invalid offset %lu found during internal copy" TSRMLS_CC,reloc_vec[i]);
			}
		}
	}
	*p++ = '\0'; /* add an end marker so the reverse process can terminate */
	assert(p == reloc_bvec+cnt);
   /*
    * Fill in the header. Note that the the reloc_vec location has to be stored as an offset
    * because must be relocatable and it can't be relocated using itself. Also only the memory
    * used, that is up to allocated will be allocated. We're now done with the pool->tag.hash 
	* a.k.a reloc_vec, so destroy it.
	*/
	hdr->reloc_vec = (size_t) ((size_t *)reloc_bvec - (size_t *)pool->brick->storage) +
                              pool->brick->base_offset;
	hdr->count     = pool->count;
	hdr->size      = pool->size;
	hdr->allocated = pool->brick->base_offset + pool->brick->allocated;
	*pool_size    = hdr->allocated;

	efree(pool->tag.hash);
	pool->tag.hash = NULL;
		
	lpc_make_PIC_pool(pool, reloc_bvec);

	/* Allocate an output serial buffer and copy the sized contents. Then destroy the pool storage*/

	buffer_length = lpc_pool_compress(pool, (unsigned char **) pool_buffer);
    _lpc_pool_destroy(pool ZEND_FILE_LINE_RELAY_CC);
#ifdef LPC_DEBUG
	lpc_debug("unload:  buffer %u bytes (compressed %lu bytes) unloaded at %s:%u"
			      TSRMLS_CC, *pool_size, buffer_length ZEND_FILE_LINE_RELAY_CC);
#endif

	/* The compressed serialised PIC version is passed back to the calling program */

	return buffer_length;
}

static int lpc_make_PIC_pool(lpc_pool *pool, unsigned char *rbvec)
{ENTER(lpc_make_PIC_pool)
   /*
	* Now use the relocation byte vector to skip down the serial pool storage to covert any
    * internal pointers to offset form. The vector contains the byte offset (in size_t units) of
    * each pointer to be relocated, with a simple high-bit multi-byte escape to encode the
	* infrequent exceptions where pointers are more than 128*8 bytes apart.  One complication
    * here is that the skip must be aware of the storage bricks and brick boundaries. Another
    * is that byte-unit and size_t-unit arithmetic mustn't get jumbled, hence the naming 
    * convention of the _x suffix to denote size_t unit offsets etc.
	*/
	unsigned char *p = rbvec;
	size_t offset_x, ro_x, this_offset_x, this_allocated_x, this_base_offset_x,  *this_storage, target_offset;
	char **this_ptr, *this_target;
	lpc_pool_brick *b, *bb, *brick; 
	int i, j, k, found, buffer_length, missed_relocs;

	brick              = pool->brickvec;
	this_storage       = (size_t *) brick->storage;
	this_allocated_x   = brick->allocated>>size_shift;
	this_base_offset_x = 0;
	offset_x           = 0;

	while (*p != '\0') {
	   /*
		* Decode the next pool offset from the byte vector
		*/
		if (*p<128) {
		   /*
            * Handle the typical case where the high-bit is clear.
			*/
			offset_x += *p++;
		} else {
		   /*
			* Handle the rare multi-byte encoded cases.  This could be optimzed, but keep it 
			* simple as this rarely happens.
			*/
			size_t ro_x = *p & 0x7f;
			for (i=7; *p++ > 127; i+=7) {
				ro_x += (*p & 0x7f)<<i; 
			}
			offset_x += ro_x;
		}
		this_offset_x = offset_x - this_base_offset_x;
		if (this_offset_x > this_allocated_x) {
			/* The pointer is in next block */
			brick++;
			this_offset_x     -= this_allocated_x;
			this_storage       = (size_t *) brick->storage;
			this_allocated_x   = brick->allocated>>size_shift;
			this_base_offset_x = brick->base_offset>>size_shift;
		}
	   /*
		* "this_offset" is now the index into the current storage brick, e.g. 3 means the 3rd
		* size_t in the brick which points to somewhere else in the pool, nearly always in this
		* brick, or the previous but possibly any. So loop backwards cyclicly around the bricks
		* starting with the current to match the offset. NOTE that unlike the pointer, there is
		* no guarantee that the target will be size_t aligned.
		*/
		this_ptr        = (char **) (this_storage + this_offset_x);
		this_target     = (char *) *this_ptr;
		for(i = 0, b = brick; i < pool->brick_count; i++, b--){
			target_offset = (size_t) (this_target - (char *) b->storage);
			if (target_offset >= 0 && target_offset < b->allocated) {
				*this_ptr  = (char *) ((this_target - (char *)b->storage) + b->base_offset);  
#ifdef LPC_DEBUG
				found = 1;
#endif
				break;
			}
			if (b == pool->brickvec) b += pool->brick_count;
		}
#ifdef LPC_DEBUG
		lpc_debug("check: 0x%08lx (0x%08lx + 0x%08lx). %s to 0x%08lx call"
				  TSRMLS_PC, this_ptr, this_storage, this_offset_x<<size_shift,
				  (found ? "Inserting relocation addr" : "Skiping external reference"),  
				  this_target);
#endif
	}
#ifdef LPC_DEBUG
   /*
	* As an integrity check scan the pool for any "untagged" internal aligned pointers that have 
    * missed the PIC adjustment.  These scan will miss any unaligned pointers which x86 tolerates at 
	* a performance hit, but since PHP is multi-architecture (e.g ARM barfs on unaligned pointers),
    * all opcode array pointer are correctly aligned.
	*/	
	lpc_debug("=== Missed Report ====" TSRMLS_PC);
	for (i = 0, b = brick, missed_relocs = 0; i < pool->brick_count; i++, b--){
		uint max = b->allocated<<size_shift;
		for (j=0; j<max; j++) {
			size_t target = ((size_t *)b->storage)[j];
			for(k = 0, bb = pool->brickvec; k < pool->brick_count; k++, bb++){
				target_offset = (size_t) ((char *)target - (char *) bb->storage);
			
				if (target_offset < bb->allocated) {
					lpc_debug("check: 0x%08lx (0x%08lx + 0x%08lx). misses reference to "
							  "0x%08lx (0x%08lx + 0x%08lx)" TSRMLS_PC,
							   (char*)(b->storage + j), b->storage, j<<size_shift,
							  target, bb->storage, target_offset);
					missed_relocs++;
					break;
				}
			}
		}
	}
	if (missed_relocs) {
		lpc_debug("=== Missed Totals %d listed ====" TSRMLS_PC, missed_relocs);
	}
#endif
}

lpc_pool* _lpc_pool_load(void* pool_storage, size_t pool_storage_length TSRMLS_DC ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_load)
	lpc_pool* pool;
	unsigned char *rbvec;
	size_t *q;
	lpc_pool_header *hdr = (lpc_pool_header *) pool_storage;

    size_shift = (sizeof(void*) == 8) ? 3 :2;

	assert(pool_storage_length == hdr->allocated);

    pool                = ecalloc(1, sizeof(lpc_pool));
#ifdef LPC_DEBUG
	pool->orig_filename = estrdup(__zend_filename);
	pool->orig_lineno   = __zend_lineno;
#endif
#if ZTS
	pool->tsrm_ls       = tsrm_ls;
#endif 
	pool->type             = LPC_RO_SERIALPOOL;
	pool->brickvec         = emalloc(sizeof (lpc_pool_brick));
	pool->brick	           = pool->brickvec;
	pool->brick->storage   = pool_storage;
	pool->brick->allocated = pool_storage_length;
	pool->brick->base_offset = 0;
	pool->brick_count      = 1;
	pool->count         = hdr->count;
    pool->size          = hdr->size;
	rbvec               = (unsigned char *)	((void **)pool_storage + hdr->reloc_vec);

	lpc_relocate_pool((size_t *)pool_storage, 
	                  (size_t) (pool_storage_length>>size_shift),
	                  rbvec, 
	                  (size_t) pool_storage);
#ifdef LPC_DEBUG
	lpc_debug("Shared pool (%lu bytes)recreated at %s:%u" TSRMLS_PC, pool_storage_length ZEND_FILE_LINE_RELAY_CC);
#endif

	return pool;
}
static void *lpc_relocate_pool(size_t *pool_storage, size_t pool_size, unsigned char *rbvec, size_t offset)
{ENTER(lpc_relocate_pool)

   /* The relocation byte vector (rbvec) contains the byte offset (in size_t units) of each
    * pointer to be relocated. As discussed above, these pointers are a LOT denser than every 
    * 127 longs (1016 bytes), but the encoding uses a simple high-bit multi-byte escape to
	* encode exceptions.  Also note that 0 is used as a terminator excepting that the first
 	* entry can validly be '0' */  

	size_t *q = pool_storage;
	unsigned char *p = rbvec;
   /*
    * Use a do {} while loop rather than a while{} loop because the first byte offset can by zero, 
	* but any other is a terminator 
	*/
	do {
		if (p[0]<128) { 		/* offset <1K the typical case */
			q += *p++;
		} else if (p[1]<128) {  /* offset <128Kb */
			q += (uint)(p[0] & 0x7f) + (((uint)p[1])<<7);
			p += 2;
		} else if (p[2]<128) {  /* offset <16Mb */
			q += (uint)(p[0] & 0x7f) + ((uint)(p[1] & 0x7f)<<7) + (((uint)p[2])<<14);
			p += 3;
		} else if (p[3]<128) {  /* offset <2Gb Ho-ho */
			q += (uint)(p[0] & 0x7f) + ((uint)(p[1] & 0x7f)<<7) + ((uint)(p[2] & 0x7f)<<14) + (((uint)p[3])<<21);
			p += 3;
		}
		*q += offset;
	} while (*p != '\0');
	assert(pool_size >= (q - pool_storage));
}

#define CHUNK 16384

/* {{{ proto int compress record pool, record &pool_buffer
       Compress from pool bricks to a set of output bricks, if more than 1 reallocate the first to
       be large enough and concatenatein the rest. Return the length and the emalloced buffer in
       the out parameter outbuf. This routine is derived from the zlib.h example zpipe.c.
*/
static int lpc_pool_compress(lpc_pool *pool, unsigned char **outbuf)
{
    int i, j, k, flush, ret, last_length;
    unsigned have;
    z_stream strm;
	uint		    brick_count = 1;
	unsigned char*  compressed_buf;
	unsigned char** brickvec = ecalloc(pool->brick_count + 1, sizeof(char *));

	memset(&strm, 0 , sizeof(strm));
    ret = deflateInit(&strm, 3);
    assert (ret == Z_OK);

	compressed_buf = emalloc(BRICK_ALLOC_UNIT);
    strm.avail_out = BRICK_ALLOC_UNIT;
    strm.next_out  = brickvec[0] = compressed_buf;

    /* compress until end of file */
	i = j = k = 0;
    do {
		flush         = (i == (pool->brick_count-1)) ? Z_FINISH : Z_NO_FLUSH;        
        strm.next_in  = pool->brick[i].storage;
        strm.avail_in = pool->brick[i].allocated;
        ret           = deflate(&strm, flush);
        assert(ret != Z_STREAM_ERROR);  /* state not clobbered */
		if (strm.avail_out == 0) {
			assert( j <= i );
			brickvec[k]     = (unsigned char *)(pool->brickvec[j++].storage);  /* reuse old pool bricks */
			strm.next_out   = brickvec[k++];
			strm.avail_out  = BRICK_ALLOC_UNIT;
		    ret             = deflate(&strm, flush);
		    assert(ret != Z_STREAM_ERROR && strm.avail_out > 0);
		}
		i++;
	} while (flush  != Z_FINISH);

    assert(ret == Z_STREAM_END);        /* stream will be complete */
	last_length = BRICK_ALLOC_UNIT - strm.avail_out;
	deflateEnd(&strm);

	if (k > 0) {
		compressed_buf = erealloc((void *)compressed_buf, ((k-1)*BRICK_ALLOC_UNIT) + last_length);
		for (i=1; i<=k; i++) {
			memcpy(compressed_buf + (i*BRICK_ALLOC_UNIT), 
				   brickvec[i],
				   (i == k) ? last_length : BRICK_ALLOC_UNIT);
		}
	}

	for (i=0; i<pool->brick_count; i++) {
		efree(pool->brick[i].storage);
		pool->brick[i].storage = NULL;
	}

	*outbuf = compressed_buf;
	return (k*BRICK_ALLOC_UNIT) + last_length;
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
