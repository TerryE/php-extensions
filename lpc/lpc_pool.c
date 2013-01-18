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

#include <zlib.h>
#include <assert.h>

#include "lpc.h"
#include "lpc_pool.h"
#include "lpc_debug.h"
#include "Zend/zend_types.h"
#include "ext/standard/crc32.h"

/* 
 * The APC REALPOOL and BDPOOL implementations used a hashtable to track individual 
 * elements and for the DTOR.  However, since the element DTOR is private, a simple
     * linked list is maintained as a prefix to individual elements enabling the pool
 * DTOR to chain down this to cleanup.
 *
 * Also note that the LPC pool implementation in debug mode uses Zend file-line relaying when
 * calling the underlying emalloc, ... routines so that the standard Zend leakage reports give
 * meaningful leakage reporting.  
 */

/* {{{ Private defines and types */
#define POOL_TYPE_STR() (pool->type == LPC_EXECPOOL ? "Exec" : "Serial")

#define FREE(p) if (p) free(p)  
#define POOL_TAG_HASH_INITIAL_SIZE    0x800
#define POOL_INTERN_HASH_INITIAL_SIZE 0x800
#define END_MARKER "\xEE\0\xEE\00"
#define CHECK(p) if(!(p)) goto error

typedef void *pointer;
#define ADD_BYTEOFF(p,o) (((zend_uchar*)p)+o)
#define GET_BYTEOFF(p,q) (((zend_uchar*)p)-((zend_uchar*)q))
#define ADD_PTROFF(p,o) (((size_t*)p)+o)
#define GET_PTROFF(p,q) (((size_t*)p)-((size_t*)q))
#define POINTER_MASK ((size_t)(sizeof(pointer)-1))
#define ROUNDUP(s) (((zend_uint)s+(sizeof(pointer)-1)) & (~(zend_uint)(sizeof(pointer)-1)))

typedef struct _pool_storage_header {
    uint  size;
    uint  allocated;
    uint  reloc_vec;
    uint  intern_vec;
    uint  count;
} pool_storage_header;

/* }}} */

/* {{{ static function prototypes */
static int offset_compare(const void *a, const void *b);
static zend_uchar *make_pool_rbvec(lpc_pool *pool);
static void missed_tag_check(lpc_pool *pool);
static void *relocate_pool(lpc_pool *pool, zend_bool copy_out);
static zend_uchar* generate_interned_strings(lpc_pool *pool);
static int pool_compress(zend_uchar *outbuf, zend_uchar *inbuf, zend_uint insize TSRMLS_DC);
static void pool_uncompress(zend_uchar *outbuf, zend_uint outsize, 
                            zend_uchar *inbuf, zend_uint insize TSRMLS_DC);
/* }}} */

/* {{{ Pool-specific module init and shutdown -- called from request init and shutdown */
/* {{{ lpc_pool_init */
int lpc_pool_init(uint max_module_len TSRMLS_DC)
{ENTER(lpc_pool_init)
    uint num_quanta;
    TSRMLS_FETCH_GLOBAL_VEC()

    zend_llist_init(&gv->exec_pools, sizeof(lpc_pool), (void (*)(void *)) lpc_pool_destroy, 0);

    num_quanta = max_module_len ? 
                    (max_module_len + gv->storage_quantum - 1) / gv->storage_quantum :
                    1;
    gv->pool_buffer_size = gv->storage_quantum * num_quanta;

    if (gv->reuse_serial_buffer) {
        /* need to check malloc return as this can fail and return a NULL pointer */
        gv->pool_buffer = malloc(gv->pool_buffer_size);
        if (!gv->pool_buffer) {
            lpc_error("Out of memory.  Cannot allocate %u bytes for pool buffer storage"
                      TSRMLS_CC,gv->pool_buffer_size);
            return 0;
        }
    }
    return 1;
}

/* }}} */

/* {{{ lpc_pool_shutdown*/
void lpc_pool_shutdown(TSRMLS_D)
{ENTER(lpc_pool_shutdown)
    if (zend_llist_count(&LPCG(exec_pools))) {
        zend_llist_destroy(&LPCG(exec_pools));
        memset(&LPCG(exec_pools),0, sizeof(zend_llist));
        }
    if (LPCG(reuse_serial_buffer) && LPCG(pool_buffer)) {
        free(LPCG(pool_buffer));
        LPCG(pool_buffer) = NULL;
    }
}
/* }}} */

/* {{{ Pool allocators
       Note that there aren't free functions as the only pool-based free operation is in the 
       pool DTOR */
/* {{{ _lpc_pool_alloc */
void _lpc_pool_alloc(void **dest, lpc_pool* pool, uint size ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_alloc)
    void *storage;

    if (pool->type == LPC_EXECPOOL) {

        storage = emalloc_rel(size);

    } else if (pool->type == LPC_SERIALPOOL) {
       /*
        * Unlike APC, LPC explicitly rounds up any sizes to the next sizeof(void *) boundary
        * because there's quite a performance hit for unaligned access on CPUs such as x86 which
        * permit it, and it would also be nice for this code to work on those such ARM which
        * don't without barfing.
        */
        zend_uint rounded_size = ROUNDUP(size);

        if (rounded_size >= pool->available) {
            /*
             * If the pool is full then bailout with a POOL_OVERFLOW.  The upper level will retry
             * the copy with a larger allocation;
             */ 
            LPCGP(bailout_status) = LPC_POOL_OVERFLOW;
            zend_bailout();
        }

        storage = (zend_uchar *) pool->storage + pool->allocated;
        pool->allocated  += rounded_size;
        pool->available  -= rounded_size;

    } else {/* LPC_RO_SERIALPOOL */
        lpc_error("Allocation operations are not permitted on a readonly Serial Pool" TSRMLS_PC);
        return;
    }

#ifdef LPC_DEBUG
    if (LPCGP(debug_flags)&LPC_DBG_ALLOC)  /* Storage Allocation */
        lpc_debug("%s alloc: 0x%08lx allocated 0x%04x bytes at %s:%d" TSRMLS_PC, 
                   POOL_TYPE_STR(), storage, size ZEND_FILE_LINE_RELAY_CC);
#endif

    pool->size += size;
    pool->count++;

    if (dest) {
        *dest = storage;
        if (!is_exec_pool()) _lpc_pool_tag_ptr(dest, pool ZEND_FILE_LINE_RELAY_CC);
    }

    return;
}
/* }}} */

/* {{{ _lpc_pool_alloc_zval 
       Special allocator that uses the Zend fast ZVAL storage allocator in exec pools */
void _lpc_pool_alloc_zval(void **dest, lpc_pool* pool ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_alloc_zval)
    if (pool->type == LPC_EXECPOOL) {
        zval *z;
        ALLOC_ZVAL(z);
        if (dest) *dest = z;
#ifdef LPC_DEBUG
        if (LPCGP(debug_flags)&LPC_DBG_ALLOC)  /* Storage Allocation */
            lpc_debug("Exec alloc: 0x%08lx allocated zval" TSRMLS_PC, z);
#endif
    } else {
        return _lpc_pool_alloc(dest, pool, sizeof(zval) ZEND_FILE_LINE_RELAY_CC);
    }
}
/* }}} */

/* {{{ _lpc_pool_alloc_ht
       Special allocator that uses the Zend fast HT storage allocator in exec pools*/
void _lpc_pool_alloc_ht(void **dest, lpc_pool* pool ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_alloc_ht)
    if (pool->type == LPC_EXECPOOL) {
        HashTable *ht;
        ALLOC_HASHTABLE(ht);
        if (dest) *dest = ht;
#ifdef LPC_DEBUG
        if (LPCGP(debug_flags)&LPC_DBG_ALLOC)  /* Storage Allocation */
            lpc_debug("Exec alloc: 0x%08lx allocated HashTable" TSRMLS_PC, ht);
#endif
    } else {
        return _lpc_pool_alloc(dest, pool, sizeof(HashTable) ZEND_FILE_LINE_RELAY_CC);
    }
}
/* }}} */

/* {{{ _lpc_pool_strdup */
void _lpc_pool_strdup(const char **d, const char* s, 
                      zend_bool dynamic, lpc_pool* pool ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_strdup)
        uint sn = 0, dummy;
        if (s && is_copy_out() && !LPC_IS_SERIAL_INTERNED(s)) { 
            sn = strlen(s);
        } 
        _lpc_pool_nstrdup(d, &dummy, s, sn, dynamic, pool ZEND_FILE_LINE_RELAY_CC);
}
/* }}} */

/* {{{ _lpc_pool_strndup 
    Note: (1) length fields:  sn is only used on copy_out(). This follows the PHPstring convention
              of excluding the terminating zero (which is still copied).   On copy in, the length
              is that of the interned string.
          (2) If the dynamic flag is true then the string is estrdup'ed.  If false then a direct
              reference is made into the string in the interned table. */
void _lpc_pool_nstrdup(const char **d, uint *dn, 
                       const char *s,  uint sn, zend_bool dynamic, lpc_pool* pool ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_nstrdup)
    if (s && d) {
        void *intern_d;
        if (is_copy_out()) {
            if (LPC_IS_SERIAL_INTERNED(s)) { /* this might be a dup from the pool */
                *d = s;
                *dn = 0;
#ifdef LPC_DEBUG
                if (LPCGP(debug_flags)&LPC_DBG_INTN)  /* Intern tracking */
                    lpc_debug("strndup: dup of copy-out intern:%u at %s:%u" TSRMLS_PC, 
                               LPC_SERIAL_INTERNED_ID(s) ZEND_FILE_LINE_RELAY_CC);
#endif
            } else { /* copy out of non-interned string */
                ulong h =  zend_inline_hash_func(s, sn+1);
                if (zend_hash_quick_find(&LPCGP(intern_hash), s, sn+1, h, &intern_d)==SUCCESS) {
                    *d = *(void**)intern_d; /*find returns a ptr to the previously stored intern */
#ifdef LPC_DEBUG
                    if (LPCGP(debug_flags)&LPC_DBG_INTN)  /* Intern tracking */
                        lpc_debug("strndup: Rep copy-out of %08lx:\"%s\" to intern:%u at %s:%u" 
                                  TSRMLS_PC, s, s, LPC_SERIAL_INTERNED_ID(*(void**)intern_d) 
                                  ZEND_FILE_LINE_RELAY_CC);
#endif
                } else {
                    *d = intern_d = LPC_SERIAL_INTERN(zend_hash_num_elements(&LPCGP(intern_hash)));
                    zend_hash_quick_add(&LPCGP(intern_hash), s, sn+1, h, 
                                        &intern_d, sizeof(void *), NULL);
#ifdef LPC_DEBUG
                    if (LPCGP(debug_flags)&LPC_DBG_INTN)  /* Intern tracking */
                        lpc_debug("strndup: New copy-out of %08lx:\"%s\" to intern:%u at %s:%u" 
                                  TSRMLS_PC, s, s, LPC_SERIAL_INTERNED_ID(intern_d) 
                                  ZEND_FILE_LINE_RELAY_CC);
#endif
                }
                *dn = 0;
            }
        } else { /* is copy_in() */
            zend_uchar *p;
            zend_uint len;  /* note that this includes the terminating zero */
            lpc_pool *sro_pool = &LPCGP(serial_pool);
            assert(LPC_IS_SERIAL_INTERNED(s) && LPC_SERIAL_INTERNED_ID(s) < LPCGP(intern_cnt));
            p = LPCGP(interns)[LPC_SERIAL_INTERNED_ID(s)];

            memcpy((void *)&len, p, sizeof(uint));       /* not uint aligned so must use memcpy! */

            *d = (dynamic) ? (const char *) estrndup_rel(p + sizeof(uint), len-1) :
                             (const char *) (p + sizeof(uint));
            *dn = len - 1;
#ifdef LPC_DEBUG
            if (LPCGP(debug_flags)&LPC_DBG_INTN)  /* Intern tracking */
                lpc_debug("strndup:  %s of intern:%u to %08lx:\"%s\" at %s:%u" 
                          TSRMLS_PC, dynamic ? "emalloc copy-in" : "Copy-in byref", 
                          LPC_SERIAL_INTERNED_ID(s), *d, *d 
                          ZEND_FILE_LINE_RELAY_CC);
#endif
        }
    } else {
        *d  = NULL;
        *dn = 0;
    }
}
/* }}} */
/*
 **HEALTH WARNING** pool_str(n)cmps can compare to strings that may or may not be interned.  Unlike 
 * the equivalent strcmps which return an ordered -1,0,1 these return 1,0,1. Either or both 
 * string scan be interned.  The comparison is complicated in the mixed case in that the intern
 * lookup is different for the in and out cases
 */ 
/* {{{ intern_strcmp
        helper routine to do mixed compare of string to intern */
static int intern_strcmp(const char* intern_s1, const char* s2, uint n, lpc_pool* pool)
{
    if(is_copy_in()) {
        char *s1;
        uint n1;
        if (LPC_SERIAL_INTERNED_ID(intern_s1) >= LPCGP(intern_cnt)) {
            return -1;
        }
        s1 = LPCGP(interns)[LPC_SERIAL_INTERNED_ID(intern_s1)];
        memcpy((void *)&n1, s1, sizeof(uint));         /* not uint aligned so must use memcpy! */
        return n1 != (n+1) || memcmp(s1 + sizeof(uint), s2, n1);
    } else {
        void *intern_s2;
        return zend_hash_find(&LPCGP(intern_hash), s2, n+1, &intern_s2)==FAILURE ||
               intern_s1 != *(void **)intern_s2;      
    }
}
/* }}} */
/* {{{ _lpc_pool_strcmp */
int _lpc_pool_strcmp(const char* s1, const char* s2, lpc_pool* pool ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_strcmp)
    if (s1 && s2) {
        if (LPC_IS_SERIAL_INTERNED(s1)) {
            if (LPC_IS_SERIAL_INTERNED(s2)) {
                return s1 != s2;
            } else {
                return intern_strcmp(s1, s2, strlen(s2), pool); 
            } 
        } else if (LPC_IS_SERIAL_INTERNED(s2)) {
            return intern_strcmp(s2, s1, strlen(s1), pool); 
        } else {
            return strcmp(s1, s2);
        }
    } else {
        return -1;
    }  
}
/* }}} */
/* {{{ _lpc_pool_strncmp 
        PHP-like string comparison of s1 and s2. Compare n bytes of s1 and s2.  Returns 0 if 
        same and -1 if not. */
int _lpc_pool_strncmp(const char* s1, const char* s2, uint n, lpc_pool* pool ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_strncmp)
    if (s1 && s2) {
        if (LPC_IS_SERIAL_INTERNED(s1)) {
            if (LPC_IS_SERIAL_INTERNED(s2)) {
                return s1 != s2;
            } else {
                return intern_strcmp(s1, s2, n, pool); 
            } 
        } else if (LPC_IS_SERIAL_INTERNED(s2)) {
            return intern_strcmp(s2, s1, n, pool); 
        } else {
            return strcmp(s1, s2);
        }
    } else {
        return -1;
    }  
}
/* }}} */

/* {{{ lpc_pmemcpy */
void _lpc_pool_memcpy(void **dest, const void* p, uint n, lpc_pool* pool ZEND_FILE_LINE_DC)
{ENTER(_lpc_pool_memcpy)
    if (dest) {
        _lpc_pool_alloc(dest, pool, n ZEND_FILE_LINE_RELAY_CC);
        memcpy(*dest, p, n);
    } else {
        void* q;
        _lpc_pool_alloc(&q, pool, n ZEND_FILE_LINE_RELAY_CC);
        memcpy(q, p, n);
    }
}
/* }}} */

/* {{{ _lpc_pool_tag_ptr 
       Used to tag any internal pointers within a serial pool to enable its relocation */
void _lpc_pool_tag_ptr(void **ptr, lpc_pool* pool ZEND_FILE_LINE_DC)
{
    size_t offset;
    int i;
    void *dummy = NULL;
   /*
    * Handle obvious no-ops and errors
    */
    if (ptr == NULL) {
#ifdef LPC_DEBUG
        if (LPCGP(debug_flags)&LPC_DBG_ALLOC)  /* Storage Allocation */
            lpc_debug("check: Null relocation ptr call at %s:%u" TSRMLS_PC, 
                       ptr ZEND_FILE_LINE_RELAY_CC);
#endif
        return;
    }

    if (pool->type != LPC_SERIALPOOL || *ptr == NULL || LPC_IS_SERIAL_INTERNED(*ptr)) {
        return;
    }

    assert(((size_t)ptr & POINTER_MASK) == 0);
   /*
    * Internal pointer are tagged, that is if (a) its address is inside the pool,  and (b) it is
    * pointing to an address inside the pool. If the appropriate log flag is set any pointers to
    * outside the pool are also logged.  And since all pointer are size_t aligned, the offset of 
    * pointer (in sizeof pointer units) is used as the index in the tags HT.
    */
    if (GET_BYTEOFF(ptr,pool->storage)  < pool->allocated) {
        if (GET_BYTEOFF(*ptr,pool->storage) < pool->allocated) {
            ulong offset = GET_PTROFF(ptr,pool->storage);
            zend_hash_index_update(&pool->tags, offset, &dummy, sizeof(void *), NULL);
#ifdef LPC_DEBUG
            if (LPCGP(debug_flags)&LPC_DBG_RELC)  /* Relocation Address check */
                lpc_debug("check: 0x%08lx (0x%08lx + 0x%04x) "
                          "Inserting relocation addr to 0x%08lx call at %s:%u"
                          TSRMLS_PC, ptr, pool->storage, offset,  *(void **)ptr ZEND_FILE_LINE_RELAY_CC);
#endif
        } else {
#ifdef LPC_DEBUG
            if (LPCGP(debug_flags)&LPC_DBG_RELO)  /* Relocation outside pool */
                lpc_debug("check: 0x%08lx Relocation addr call to 0x%08lx  outside pool at %s:%u"  
                          TSRMLS_PC, ptr, *(size_t *)ptr ZEND_FILE_LINE_RELAY_CC);
#endif
            return;
        }
    }
}
/* }}} */
/* }}} */

/* {{{ Public pool creation and deletion functions */
/* {{{ lpc_pool_storage 
       The storage for serial pool can be reused to avoid storage fragmentation, but the logic gets
       a bit snotty, so this allocation routine fully encapsulates this logic centrally.  */
extern void  lpc_pool_storage(zend_uint arg1, zend_uint compressed_size, 
                              zend_uchar** opt_addr TSRMLS_DC)
{ENTER(lpc_pool_storage)
    zend_uint storage_size;
    int       num_quanta;
    TSRMLS_FETCH_GLOBAL_VEC()

    if(compressed_size && opt_addr) {
        zend_uint record_size = arg1;
       /*
        * The buffer is going to be used for reading in a module from the cache into a RO Serial
        * Pool.  The uncompressed and compressed records are overlapped to save storage, however, 
        * the safe amount of overlap is compression algorithm dependent. 
        */
        switch (gv->compression_algo) {
            case 0:  /* = none */
                storage_size = record_size;
                break;

            case 1:  /* = RLE */
                storage_size = MAX(compressed_size, record_size) + (compressed_size/15);
                break;

            case 2:  /* = GZ */
                storage_size = MAX(compressed_size, record_size) + (record_size >> 11);
                break;
        }
        num_quanta   = (storage_size + sizeof(zend_uint) + gv->storage_quantum - 1) 
                          / gv->storage_quantum;
        storage_size = num_quanta * gv->storage_quantum;
       /*
        * Ensure that the global variables pool_buffer and pool_buffer_size are initialised and
        * allocate storage according to the reuse policy. Last, add 4 byte overflow detector after
        * end of compressed_buffer.
        */
        if (!gv->reuse_serial_buffer || !gv->pool_buffer) {
            gv->pool_buffer      = malloc(storage_size);
            gv->pool_buffer_size = storage_size;

        } else if (storage_size > gv->pool_buffer_size) {

            gv->pool_buffer      = realloc(gv->pool_buffer, storage_size);
            gv->pool_buffer_size = storage_size;

        }

        if (!gv->pool_buffer) {
            lpc_error("Out of memory allocating pool storage" TSRMLS_CC);
            zend_bailout();
        }

        gv->pool_buffer_rec_size = record_size;
        gv->pool_buffer_comp_size= compressed_size;

        *opt_addr = (gv->compression_algo == 0) ? 
                        gv->pool_buffer :
                        gv->pool_buffer + (gv->pool_buffer_size - ROUNDUP(compressed_size+4));
        memcpy(*opt_addr + compressed_size, END_MARKER, 4);

    } else if (arg1 == (zend_uint) -1) {
       /*
        * Free the pool buffer if not reusing
        */
        if (gv->pool_buffer && !gv->reuse_serial_buffer) {
            free(gv->pool_buffer);
            gv->pool_buffer      = NULL;
            gv->pool_buffer_size = 0;
        }
    } else {        
       /*
        * The buffer is going to be used for creating a new compiled module in a Serial Pool for 
        * writing into the cache.  A non-zero request means grow the buffer by the requested amount.
        * Again reuse according to reuse policy.
        */
        zend_uint request_size = arg1;

        storage_size = (request_size) ? gv->pool_buffer_size + request_size :
                                        gv->storage_quantum;
        num_quanta   = (storage_size + gv->storage_quantum - 1) / gv->storage_quantum;
        storage_size = num_quanta * gv->storage_quantum;

        if (!gv->reuse_serial_buffer || !gv->pool_buffer) {
            gv->pool_buffer      = malloc(storage_size);
            gv->pool_buffer_size = storage_size;

        } else if (storage_size > gv->pool_buffer_size) {
            gv->pool_buffer      = realloc(gv->pool_buffer, storage_size);
            gv->pool_buffer_size = storage_size;

        }

        if (!gv->pool_buffer) {
            lpc_error("Out of memory allocating pool storage" TSRMLS_CC);
            zend_bailout();
        }

        gv->pool_buffer_rec_size = 0;
        gv->pool_buffer_comp_size= 0;

    }
}
/* }}} */

/* {{{ _lpc_pool_create */
extern lpc_pool* lpc_pool_create(lpc_pool_type_t type, void** arg1 TSRMLS_DC)
{ENTER(lpc_pool_create)   
    char *s;
    void *dummy;
    TSRMLS_FETCH_GLOBAL_VEC()

    if (type == LPC_EXECPOOL) {
       /*
        * This just a simple wrapper around the emalloc storage allocator to unify the API for
        * serial and exec pools. 
        */
        lpc_pool pool = {LPC_EXECPOOL,};     /* this is just a blank template */
#if ZTS
        pool.tsrm_ls  = tsrm_ls;
        pool.gv       = ((zend_lpc_globals *)(*((void ***)tsrm_ls))[TSRM_UNSHUFFLE_RSRC_ID(lpc_globals_id)]);
#endif 
#ifdef LPC_DEBUG
        if (LPCG(debug_flags)&LPC_DBG_LOAD)  /* Load/Unload Info */
            lpc_debug("Exec pool created" TSRMLS_CC);
#endif
       /*
        * To enable strdups by reference, the interns vector is copied into emalloced memory.  This
        * is then freed as a single block as part of the exec pool DTOR.
        */
        if (gv->interns) {
            zend_uchar **p  = gv->interns;
            zend_uint   *q  = (zend_uint *)(gv->interns[0]);
            zend_uint    total_size = q[0], cnt = q[1], off = 0, len = 0, i;

            pool.intern_copy = emalloc(total_size - 2*sizeof(uint));
            q += 2;
            memcpy(pool.intern_copy, q, total_size - 2*sizeof(uint));

            for (i=0; i < gv->intern_cnt; i++) {
                memcpy(&len, ADD_BYTEOFF(q, off), sizeof(zend_uint));  /* not uint aligned */
                p[i] = pool.intern_copy + off;
                off += len + sizeof(zend_uint);;
            }
        }
       /*
        * zend llists take a copy of the data element (the exec pool) and this is the one used
        */
	    zend_llist_add_element(&gv->exec_pools, &pool);
        return zend_llist_get_first(&gv->exec_pools);

    } else if (type == LPC_SERIALPOOL) {
       /*
        * This is the R/W serial allocator used as the destination pool for copy-out.
        */ 
        pool_storage_header *hdr;
        lpc_pool *pool = &gv->serial_pool;

        memset(pool, 0, sizeof(lpc_pool));
        pool->type    = LPC_SERIALPOOL;
#if ZTS
        pool->tsrm_ls = tsrm_ls;
        pool->gv      = ((zend_lpc_globals *)(*((void ***)tsrm_ls))[TSRM_UNSHUFFLE_RSRC_ID(lpc_globals_id)]);

#endif 
#ifdef LPC_DEBUG
        if (LPCG(debug_flags)&LPC_DBG_LOAD)  /* Load/Unload Info */
            lpc_debug("Serial pool created" TSRMLS_CC);
#endif       
       /*
        * Use the pool global variables to allocate the appropriate pool storage. 
        */
        pool->storage   = (void *) gv->pool_buffer;
        pool->available = gv->pool_buffer_size - sizeof (zend_uint);
        pool->allocated = 0;
        
        zend_hash_init(&pool->tags, POOL_TAG_HASH_INITIAL_SIZE, NULL, NULL, 1);
        zend_hash_init(&gv->intern_hash, POOL_INTERN_HASH_INITIAL_SIZE, NULL, NULL, 1);
       /*
        * Allocate the pool header.  Use a stack destination to prevent pointer tagging.
        */ 
        pool_alloc(dummy, sizeof(pool_storage_header));
        hdr            = pool->storage;      /* the header is the zeroth entry */
        memset(pool->storage , 0, gv->pool_buffer_size);
        return pool;

    } else if (type == LPC_RO_SERIALPOOL && gv->pool_buffer_comp_size) {
       /*
        * This is the Readonly serial allocator used as the source pool for copy-in.
        */
        void **first_rec = arg1; 
        lpc_pool *pool = &gv->serial_pool;
        pool_storage_header *hdr;
        zend_uchar *p, **q;
        zend_uint   record_size = gv->pool_buffer_rec_size;
        zend_uint i;

        memset(pool, 0, sizeof(lpc_pool));
        pool->type    = LPC_RO_SERIALPOOL;
    #if ZTS
        pool->tsrm_ls = tsrm_ls;
        pool->gv      = ((zend_lpc_globals *)(*((void ***)tsrm_ls))[TSRM_UNSHUFFLE_RSRC_ID(lpc_globals_id)]);
    #endif 
       /* 
        * Allocate pick up the pool buffer which was allocated / reused in lpc_prepare() and 
        * initialize the pool context. Set up the interns pointer vector so that the GV intern_cnt[i]
        * points to the i'th interned string. Reuse the tail end of the pool_buffer if enough room,
        * otherwise emalloc the storage.
        */

        if (gv->compression_algo) {
            zend_uchar *comp_buf = gv->pool_buffer + 
                                  (gv->pool_buffer_size - ROUNDUP(gv->pool_buffer_comp_size+4));
            pool_uncompress(gv->pool_buffer, gv->pool_buffer_rec_size,
                            comp_buf, gv->pool_buffer_comp_size TSRMLS_CC);
        }

        pool->storage     = (void *) gv->pool_buffer;
        pool->allocated   = record_size;
        pool->available   = 0;

        hdr              = (pool_storage_header *) pool->storage;
        assert(record_size == hdr->allocated);
        pool->count      = hdr->count;
        pool->size       = hdr->size;

        if (hdr->intern_vec) {
            zend_uint *p   = (zend_uint *)ADD_PTROFF(pool->storage, hdr->intern_vec);
            gv->intern_cnt = p[1];
            gv->interns    = (gv->intern_cnt < (gv->pool_buffer_size-record_size)/sizeof(size_t)) ?
                                 (zend_uchar **) ADD_BYTEOFF(pool->storage, ROUNDUP(record_size)) :
                                 emalloc(gv->intern_cnt*sizeof(zend_uchar *));
            gv->interns[0] =  (zend_uchar *)p;  /* use 0'th entry as tmp ptr to the intern vecs */
        } else {
            gv->interns = NULL;
         }
        
        relocate_pool(pool, 0);

    #ifdef LPC_DEBUG
        if (gv->debug_flags&LPC_DBG_LOAD)  /* Load/Unload Info */
            lpc_debug("Shared pool (compressed %u -> %u uncompressed bytes) recreated" 
                      TSRMLS_CC, gv->pool_buffer_comp_size, record_size); 
    #endif

        if (first_rec) {
            *first_rec = (void *) (gv->pool_buffer + ROUNDUP(sizeof(pool_storage_header)));
        }
        return pool;

    } else {
        lpc_error("Invalid pool type %d" TSRMLS_CC, type);
        return NULL;
    }
}
/* }}} */

/* {{{ lpc_pool_serialize
       Convert a serial pool to compressed serial format for output */
zend_uchar* lpc_pool_serialize(lpc_pool* pool, zend_uint* compressed_size, 
                               zend_uint* record_size)
{ENTER(lpc_pool_serialize)
    int           length, size, offset, buffer_length;
    zend_uchar   *storage, *reloc_bvec, *interned_bvec;
    HashTable    *ht = &pool->tags;
    pool_storage_header *hdr;
    TSRMLS_FETCH_FROM_POOL()

    if (pool->type != LPC_SERIALPOOL) {
        return NULL;
    }

    reloc_bvec = make_pool_rbvec(pool);

    interned_bvec = generate_interned_strings(pool);

    storage        = (zend_uchar *)pool->storage;
   /*
    * Fill in the header. (This was the first allocated block in the pool storage.) Note that the
    * reloc_bvec location has to be stored as an offset because must be relocatable and it can't be
    * relocated using itself. Also only the memory used, that is up to allocated will be included.
    */
    hdr             = (pool_storage_header *)storage;   
    hdr->reloc_vec  = GET_PTROFF(reloc_bvec, storage);
    hdr->count      = pool->count;
    hdr->size       = pool->size;
    hdr->allocated  = pool->allocated;
    hdr->intern_vec = (interned_bvec == NULL) ? 0 : GET_PTROFF(interned_bvec, storage);
    *record_size    = hdr->allocated;

    relocate_pool(pool, 1);

#ifdef LPC_DEBUG
    missed_tag_check(pool);
#endif

    pool->storage  = NULL;

    if (LPCG(compression_algo) == 0) {
        *compressed_size = hdr->allocated;
    } else {
       /*
        * Top-end justify the allocated section of the storage and detach from pool as this is all
        * we now need, so the pool itself can now be destroyed.  Then compress back to the bottom
        * end of the storage. (This allows the compression to be done in-place to save storage, 
        * whilst minimising the chance of throwing a LPC_POOL_OVERFLOW.)  The compressed buffer is 
        * then returned to the calling routine.
        */
        pool_storage_header h=*hdr;
        length        = pool->allocated + pool->available;
        size          = pool->allocated;
        offset        = length - size;

        memmove(storage + offset, storage, size);   /* overlapped buffers so use memmove! */
        *compressed_size = pool_compress(storage, storage + offset, size  TSRMLS_CC);
#ifdef LPC_DEBUG
        if (LPCG(debug_flags)&LPC_DBG_LOAD)  /* Load/Unload Info */
            lpc_debug("unload:  buffer %u bytes (%u + %u + %u compressed to %u bytes) unloaded"
                      TSRMLS_CC, size, h.reloc_vec*sizeof(size_t),
                      (h.intern_vec-h.reloc_vec)*sizeof(size_t),
                      size-(h.intern_vec*sizeof(size_t)), *compressed_size);
#endif
    }
    pool->storage = NULL;
    return (zend_uchar*) storage;
}
/* }}} */

/* {{{ lpc_pool_destroy */
void lpc_pool_destroy(lpc_pool *pool)
{ENTER(lpc_pool_destroy)
    zend_lpc_globals *gv = pool->gv;
    TSRMLS_FETCH_FROM_POOL()

#ifdef LPC_DEBUG
    if (gv->debug_flags&LPC_DBG_LOAD)  /* Load/Unload Info */
        lpc_debug("Destroy %s pool 0x%08lx (%u)" TSRMLS_PC, 
                  POOL_TYPE_STR(), pool, (uint) pool->size);
#endif
   /* The pool can contain the following dynamic elements which need garbage collected on destruction:
    *    The tags and intern_hash HashTables used in serial pools
    *    The interns pointer vector used in R/O serial pools
    * Once destroyed, the pool record is zeroed. 
    */
    if (gv->interns) {
        /* Only free if it points outside the pool buffer */ 
        size_t offset = GET_BYTEOFF(gv->interns, pool->storage);
        if (offset >= (size_t) gv->pool_buffer_size) {
            efree(gv->interns);
        }
        gv->interns = NULL;
    }

    if (!pool->tags.inconsistent && pool->tags.arBuckets) {
        zend_hash_destroy(&pool->tags);
        }

    if (!gv->intern_hash.inconsistent && gv->intern_hash.arBuckets) {
        zend_hash_destroy(&gv->intern_hash);
        }
    /* free the pool storage if not in reuse mode */
    if (pool->storage && !gv->reuse_serial_buffer) {
        free(pool->storage);
        gv->pool_buffer = NULL;
    }

    if (pool->intern_copy) {
        efree(pool->intern_copy);
        pool->intern_copy = NULL;
    }

    memset(pool, 0, sizeof(lpc_pool));
    gv->pool_buffer_comp_size = 0;
 
}
/* }}} */
/* }}} Pool creation and deletion functions */

/* {{{ Internal helper functions */
/* {{{ offset_compare
       Simple sort callback to enable sorting of an a vector of offsets into offset order  */
static int offset_compare(const void *a, const void *b)
{
    long diff = *(ulong *)a - *(ulong *)b;
    return (diff > 0) ? 1 : ((diff < 0) ? -1 : 0);
}
/* }}} */

/* {{{ make_pool_rbvec */
static zend_uchar *make_pool_rbvec(lpc_pool *pool)
{ENTER(make_pool_rbvec)
    HashTable       *ht = &pool->tags;
    int              i, reloc_vec_len, cnt;
    ulong           *reloc_vec, *p; 
    size_t           ro;
    size_t           max_offset=pool->allocated/sizeof(size_t);
    zend_uchar      *reloc_bvec, *q;
    pool_storage_header *hdr;
    TSRMLS_FETCH_FROM_POOL()
   /*
    * The fixups hash of offsets is now complete and the processing takes a few passes of the
    * offsets sorted into ascending offset order, so it's easier just to convert it into an ordered
    * contiguous vector of the ulong indexes, reloc_vec, which is allocated from the headroom in the
    * storage pool.  
    *
    * This is then sorted into ascending order using a standard qsort before being compressed down
    * into the byte-compressed version, reloc_bvec. Since each entry of the compressed version is
    * smaller, reloc_bvec can also be allocated on the same headroom, overlaying reloc_vec. The
    * reason for extra size_t filler in front of reloc_vec is a safety to avoid collision at start
    * of compression.
    */
    reloc_vec     = (ulong*)ADD_BYTEOFF(pool->storage, pool->allocated);
    reloc_vec_len = zend_hash_num_elements(&pool->tags);
    if (reloc_vec_len*sizeof(ulong) > pool->available) {
        LPCG(bailout_status) = LPC_POOL_OVERFLOW;
        zend_bailout();
    }
 
    zend_hash_internal_pointer_reset(ht);
    for (i = 0, p = reloc_vec; i < reloc_vec_len; i++) {
        zend_hash_get_current_key_ex(ht, NULL, NULL, p, 0, NULL);
        zend_hash_move_forward(ht);
        if (*p < max_offset) {
            p++;
        } else {
#ifdef LPC_DEBUG
            if (LPCGP(debug_flags)&LPC_DBG_RELC)  /* Relocation address check */
                lpc_debug("Relocation notice: external pointer %08lx at offset %08x in serial pool"
                          TSRMLS_CC, *q, (uint) GET_BYTEOFF(q,pool->storage));
#endif
        }
    }

    reloc_vec_len = p - reloc_vec;

    zend_hash_destroy(ht);
    memset(ht, 0, sizeof(HashTable));

    qsort(reloc_vec, reloc_vec_len, sizeof(ulong), offset_compare);

   /*
    * Convert relocation vector into delta offsets, d(i) = p(i-1) - p(i), in the range 1..
    * reloc_vec_len-1 and the easiest way to do this in place is to work backwards from the end. 
    * Note d(1) = p(1) - 0 
    */
    for (i = reloc_vec_len-1; i > 0; i--) {
        reloc_vec[i] -= reloc_vec[i-1];
    }
   /*
    * The final relocation vector stored in the pool will be delta encoded one per byte. In real
    * PHP opcode arrays, pointers are a LOT denser than every 127 longs (1016 bytes), but the coding
    * needs to cope with pathological cases, so a simple high-bit multi-byte escape is used; hence
    * delta offsets up to 131,064 bytes are encoded in 2 bytes, etc.. This pass to works out the
    * COUNT of any extra encoding bytes needed to enable a vector of the correct size to be in the
    * pool. The +1 is for the '\0' terminator. 
    */
    for (i = 0, cnt = reloc_vec_len + 1; i < reloc_vec_len; i++) {
        for (ro=reloc_vec[i]; ro>=0x80; ro >>= 7, cnt++) { }
    }
   /*
    * Note that any further allocations MUST have a destination address outside the pool so as not
    * to trigger further address tagging.  This of course includes the reloc_bvec itself.
    */ 
    pool_alloc(reloc_bvec, cnt);

    /* Loop over relocation vector again, this time generating the relocation byte vector */
    for (i = 0, q = reloc_bvec; i < reloc_vec_len; i++) {
        if (reloc_vec[i]<=0x7f) { /* the typical case */
            *q++ = (zend_uchar) (reloc_vec[i]);
        } else {                  /* handle the pathological cases */
            ro = reloc_vec[i];
           /*
            * Emit multi-bytes lsb first with 7 bits sig. and high-bit set to indicate follow-on.
            */
            *q++ = (zend_uchar)(ro & 0x7f) | 0x80;
            if (ro <= 0x3fff) {
                *q++ = (zend_uchar)((ro>>7) & 0x7f);
            } else if (ro <= 0x1fffff) {
                *q++ = (zend_uchar)((ro>>7) & 0x7f) | 0x80;
                *q++ = (zend_uchar)((ro>>14) & 0x7f);    
            } else if (ro <= 0xfffffff) {
                *q++ = (zend_uchar)((ro>>7) & 0x7f) | 0x80;
                *q++ = (zend_uchar)((ro>>14) & 0x7f) | 0x80;
                *q++ = (zend_uchar)((ro>>21) & 0x7f);    
            } else {
                lpc_error("Fatal: invalid offset %lu found during internal copy"
                          TSRMLS_CC,reloc_vec[i]);
            }
        }
    }
    *q++ = '\0'; /* add an end marker so the reverse process can terminate */
    assert((zend_uchar *)q == reloc_bvec+cnt);

    return reloc_bvec;
}

/* {{{ missed_tag_check */
static void missed_tag_check(lpc_pool *pool)
{ENTER(missed_tag_check)
#ifdef LPC_DEBUG
   /*
    * The debug build implements an integrity check of scanning the pool for any "untagged" internal 
    * aligned pointers that have missed the PIC adjustment.  These scan will miss any unaligned 
    * pointers (which x86 tolerates at a performance hit) but since PHP is multi-architecture (e.g. 
    * ARM barfs on unaligned pointers), all opcode array pointers are correctly aligned.
    */  
    zend_uchar **storage      = (zend_uchar **) pool->storage;
    zend_uint    max_offset_x = ((pool_storage_header *)storage)->reloc_vec;
    zend_uint    max_offset   = max_offset_x * sizeof(size_t);
    zend_uint i, missed = 0, externs = 0;

    if (!(LPCGP(debug_flags)&LPC_DBG_RELR)) {
        return;
    }
    lpc_debug("=== Missed Report ====" TSRMLS_PC);

    for (i=0; i<max_offset_x; i++) {
        zend_uchar **ptr    = storage + i;
        size_t       target = (size_t) *ptr;
        zend_uint    target_offset = GET_BYTEOFF(target, storage);

        if (target_offset < max_offset) {
            lpc_debug("checkR: 0x%08lx (0x%08lx + 0x%08x): potential reference to "
                      "0x%08lx (0x%08lx + 0x%08x)" TSRMLS_PC,
                      ptr, storage, i*sizeof(size_t),
                      target, storage, target_offset);
            missed++;
       /*
        * This is an empirical exclusion list. No apologies.  It might miss some but the 
        * detections don't seem to be false-alarms on my system. 
        */
        } else if ( !LPC_IS_SERIAL_INTERNED(target) &&
                    (target > 0x0000000001000000 && target < 0x00007ffffffff000) &&
                     target & 0x00000000fffff000 &&
                    (target < 000000000004000000 || target > 0x00007ff0000000000) ) {
            lpc_debug("checkE: 0x%08lx (0x%08lx + 0x%08x): possible external reference to "
                      "0x%08lx" TSRMLS_PC, ptr, storage, i*sizeof(size_t),
                       target);
            externs++;
        }
    }
    if (missed || externs) {
        lpc_debug("=== Missed Totals %d / %d listed ====" TSRMLS_PC, missed, externs);
    }
#endif
}
/* }}} */

/* {{{ relocate_pool */
static void *relocate_pool(lpc_pool *pool, zend_bool copy_out)
{ENTER(relocate_pool)
   /* The relocation byte vector (rbvec) contains the byte offset (in size_t units) of each
    * pointer to be relocated. As discussed above, these pointers are a LOT denser than every 
    * 127 longs (1016 bytes), but the encoding uses a simple high-bit multi-byte escape to
    * encode exceptions.  Also note that 0 is used as a terminator excepting that the first
    * entry can validly be '0' 
    */  
    pool_storage_header *hdr = pool->storage;    /* The header block is the first allocated */
    zend_uchar  *rbvec       = (zend_uchar*) ADD_PTROFF(pool->storage,hdr->reloc_vec);
    zend_uchar  *p           = rbvec;
    size_t      *q           = (size_t *)pool->storage;
    size_t       maxqval     = pool->allocated;
    TSRMLS_FETCH_FROM_POOL()
   /*
    * Use a do {} while loop rather than a while{} loop because the first byte offset can by zero, 
    * but any other is a terminator 
    */
    do {
        if (p[0]<128) {         /* offset <1K the typical case */
            q += *p++;
        } else if (p[1]<128) {  /* offset <128Kb */
            q += (uint)(p[0] & 0x7f) + (((uint)p[1])<<7);
            p += 2;
        } else if (p[2]<128) {  /* offset <16Mb */
            q += (uint)(p[0] & 0x7f) + ((uint)(p[1] & 0x7f)<<7) + (((uint)p[2])<<14);
            p += 3;
        } else if (p[3]<128) {  /* offset <2Gb Ho-ho */
            q += (uint)(p[0] & 0x7f)      + ((uint)(p[1] & 0x7f)<<7) + 
                ((uint)(p[2] & 0x7f)<<14) + (((uint)p[3])<<21);
            p += 3;
        }
        if (copy_out) {
            if ((unsigned) GET_PTROFF(*q, pool->storage) >= maxqval) {
                lpc_debug("Relocation error: external pointer %08lx at offset %08lx in serial pool"
                          TSRMLS_CC, *q, GET_BYTEOFF(q, pool->storage));
            } else { 
                *q -= (size_t) pool->storage;
            }
        } else { /* copy in */
            if (*q >= maxqval) {
                lpc_error("Relocation error: invalid offset %08lx at offset %08lx in serial pool"
                          TSRMLS_CC, *q, GET_BYTEOFF(q, pool->storage));
            } else { 
                *q += (size_t) pool->storage;
            }
        }
    } while (*p != '\0');
   
    assert(GET_BYTEOFF(q, pool->storage) <= pool->allocated);
}
/* }}} */

/* {{{ generate_interned_strings */
static zend_uchar* generate_interned_strings(lpc_pool *pool) 
{ENTER(generate_interned_strings)
   /*
    * The interned string hashtable is serialize out to the pool in the format 
    *     <total size><count>{<size><string btyes>}
    * where count and size are 4-byte unsigned values. Memcpy is used to move these because the
    * sizes are only byte-aligned.  This is a PIC format.  
    */
    HashTable *ht = &LPCGP(intern_hash);
    int  i, cnt = zend_hash_num_elements(ht);
    zend_uint size = 0, sn = 0, total_size;
    zend_uchar *s, *d, *interned_vec;

    if (cnt == 0) {
        return NULL;
    }

    zend_hash_internal_pointer_reset(ht);
    for (i = 0; i < cnt; i++) {
        zend_hash_get_current_key_ex(ht, (char **) &s, &sn, NULL,  0, NULL);
        zend_hash_move_forward(ht);
        size += sn;   /* total up the sum of the string lengths */
    }

    total_size = ((cnt+2)*sizeof(uint)) + size;

    pool_alloc(interned_vec, total_size);
    ((zend_uint *)interned_vec)[0] = total_size; /*allocs are size_t aligned so this is OK */
    ((zend_uint *)interned_vec)[1] = cnt;

    zend_hash_internal_pointer_reset(ht);
    for (i = 0, d = interned_vec + (2*sizeof(uint)); i < cnt; i++) {
        zend_hash_get_current_key_ex(ht, (char **) &s, &sn, NULL,  0, NULL);
        zend_hash_move_forward(ht);
        memcpy( d, &sn, sizeof(uint));           /* this one isn't size_t aligned hence memcpy */
        memcpy( d+sizeof(uint), s, sn);
        d += sizeof(uint)+sn;
    }

    assert((d-interned_vec) == (cnt+2)*sizeof(uint) + size);
    zend_hash_destroy(ht);
    memset(ht, 0, sizeof(HashTable));

    return interned_vec;
}
/* }}} */

/* {{{ pool_compress */
static int pool_compress(zend_uchar *outbuf, zend_uchar *inbuf, zend_uint insize TSRMLS_DC)
{ENTER(pool_compress)

    if (LPCG(compression_algo) == 1) {           /* 1 = RLE */
        zend_uchar *p = inbuf, *pend = inbuf+insize, *q = outbuf;
        zend_uchar *s;
        int i, j, outsize;
        uint final_crc;

        while ( p < pend ) {
            for (i = 0, j = 0; *p == 0; i++, p++) {}  /* count zero bytes */
            if (p >= pend) {                          /* then the zeros run past the end */
                i -= (p - pend);                      /* so trim to last true zero */
                p = pend;
            } else {
                for (s = p; *p != 0; j++, p++) {}     /* count non-zero bytes */
                if (p >= pend) {                      /* then the non-zeros run past the end */
                    j -= (p - pend);                  /* so trim to last true non-zero */
                    p = pend;
                }
            }
           /*
            * We now have i zeros followed by j non-zeros. The O/P is a repeat of a prefix byte 0xNM
            * where N is the number of zeros (0..15) and M the number of non-zero bytes (0..15)
            * followed by M non-zero bytes.  Clearly if i>15 then extra 0xf0's are needed and 
            * if j>15 the o/p string will require an extra 0x0f prefix every 16th character.
            */

            for (; i > 15;i -= 15) { *q++ = 0xf0; }  /* emit any extra zero run prefixes */

            for (; j > 15; j -= 15, q += 15, s += 15) {
                *q++ = (i<<4) + 0x0f;                /* emit blocks of prefix + 15 non-zero bytes */
                memcpy(q, s, 15);
                i = 0;
            }

            *q++ = (i<<4) + j;                       /* emit the last block with the prefix set */
            memcpy(q, s, j);                         /* to the residue i/j                      */
            q += j;

            if (q >= p) {                            /* bailout if overlap overflow has occurred */
                LPCG(bailout_status) = LPC_POOL_OVERFLOW;
                zend_bailout();
            }
        }
        outsize = q - outbuf;
        {
           	register zend_uint crc = 0xFFFFFFFF;
            zend_uchar *p=outbuf;        
            for (i = 0; i < outsize; i++) {
                crc = (crc >> 8) ^ crc32tab[(zend_uchar)crc ^ *p++];
            }
            final_crc = crc^0xFFFFFFFF;
            memcpy(p, &final_crc, sizeof(uint));
        }
        return (q+4) - outbuf;

    } else if (LPCG(compression_algo) == 2) {    /* 2 = GZ  */
        ulong outsize = (inbuf - outbuf) + insize; 

        if (outsize < compressBound(insize)) {
            LPCG(bailout_status) = LPC_POOL_OVERFLOW;
            zend_bailout();
        }
        if (compress2(outbuf, &outsize, inbuf, (uLong) insize, 2)==Z_OK) {
            return outsize;
        } else {
            lpc_error("unkown fatal error during compression" TSRMLS_CC);
            zend_bailout();
        }

    } else {                                     /* 0 = none */

        memcpy(outbuf, inbuf, insize);
        return insize;

    }
}
/* }}} */

/* {{{ pool_uncompress */
static void pool_uncompress(zend_uchar *outbuf, zend_uint outsize, 
                            zend_uchar *inbuf, zend_uint insize TSRMLS_DC)
{ENTER(pool_uncompress)

    if (LPCG(compression_algo) == 1) {           /* 1 = RLE */
        zend_uchar *p = inbuf, *pend, *q;
        {
            uint i, final_crc;
           	register zend_uint crc = 0xFFFFFFFF; 
            insize -= 4;
            
            for (i = 0; i < insize; i++) {
                crc = (crc >> 8) ^ crc32tab[(zend_uchar)crc ^ *p++];
            }
            memcpy(&final_crc, p, sizeof(uint));
            if ((crc^0xFFFFFFFF) != final_crc) {
                lpc_error("CRC mismatch on record read: calculated 0x%08x; read 0x%08x" TSRMLS_CC, 
                crc^0xFFFFFFFF, final_crc);
                zend_bailout();
            }
        }
        p    = inbuf;
        pend = inbuf+insize;
        q    = outbuf;
        while ( p < pend ) {
            int num_zeros     = (*p & 0xf0)>>4;
            int num_non_zeros = *p++ & 0x0f;

            memset (q, 0, num_zeros);
            q += num_zeros;
            memcpy (q, p, num_non_zeros);
            p += num_non_zeros;
            q += num_non_zeros;
        }

        CHECK(q == outbuf + outsize);
    } else if (LPCG(compression_algo) == 2) {    /* 2 = GZ  */
        ulong expanded_size = outsize;

        CHECK(uncompress(outbuf, &expanded_size, inbuf, insize) == Z_OK &&
              expanded_size == outsize);

    } else {                                     /* 0 = none */

        if ( outbuf != inbuf) memcpy(outbuf, inbuf, insize);

    }
    return;

error:

    lpc_error("Unknown fatal error during decompression" TSRMLS_CC);
    zend_bailout();

}
/* }}} */
/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 ss=4
 */
