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


/* {{{ forward references */
static lpc_pool* lpc_unpool_create(lpc_pool_type type, lpc_malloc_t, lpc_free_t, lpc_protect_t, lpc_unprotect_t TSRMLS_DC);
static lpc_pool* lpc_realpool_create(lpc_pool_type type, lpc_malloc_t, lpc_free_t, lpc_protect_t, lpc_unprotect_t TSRMLS_DC);
/* }}} */

/* {{{ lpc_pool_create */
lpc_pool* lpc_pool_create(lpc_pool_type pool_type, 
                            lpc_malloc_t allocate, 
                            lpc_free_t deallocate,
                            lpc_protect_t protect,
                            lpc_unprotect_t unprotect
			    TSRMLS_DC)
{
    if(pool_type == LPC_UNPOOL) {
        return lpc_unpool_create(pool_type, allocate, deallocate,
                                            protect, unprotect TSRMLS_CC);
    }

    return lpc_realpool_create(pool_type, allocate, deallocate, 
                                          protect,  unprotect TSRMLS_CC);
}
/* }}} */

/* {{{ lpc_pool_destroy */
void lpc_pool_destroy(lpc_pool *pool TSRMLS_DC)
{
    lpc_free_t deallocate = pool->deallocate;
    lpc_pcleanup_t cleanup = pool->cleanup;

    cleanup(pool TSRMLS_CC);
    deallocate(pool TSRMLS_CC);
}
/* }}} */

/* {{{ lpc_unpool implementation */

typedef struct _lpc_unpool lpc_unpool;

struct _lpc_unpool {
    lpc_pool parent;
    /* lpc_unpool is a lie! */
};

static void* lpc_unpool_alloc(lpc_pool* pool, size_t size TSRMLS_DC) 
{
    lpc_unpool *upool = (lpc_unpool*)pool;

    lpc_malloc_t allocate = upool->parent.allocate;

    upool->parent.size += size;
    upool->parent.used += size;

    return allocate(size TSRMLS_CC);
}

static void lpc_unpool_free(lpc_pool* pool, void *ptr TSRMLS_DC)
{
    lpc_unpool *upool = (lpc_unpool*) pool;

    lpc_free_t deallocate = upool->parent.deallocate;

    deallocate(ptr TSRMLS_CC);
}

static void lpc_unpool_cleanup(lpc_pool* pool TSRMLS_DC)
{
}

static lpc_pool* lpc_unpool_create(lpc_pool_type type, 
                    lpc_malloc_t allocate, lpc_free_t deallocate,
                    lpc_protect_t protect, lpc_unprotect_t unprotect
		    TSRMLS_DC)
{
    lpc_unpool* upool = allocate(sizeof(lpc_unpool) TSRMLS_CC);

    if (!upool) {
        return NULL;
    }

    upool->parent.type = type;
    upool->parent.allocate = allocate;
    upool->parent.deallocate = deallocate;

    upool->parent.protect = protect;
    upool->parent.unprotect = unprotect;

    upool->parent.palloc = lpc_unpool_alloc;
    upool->parent.pfree  = lpc_unpool_free;

    upool->parent.cleanup = lpc_unpool_cleanup;

    upool->parent.used = 0;
    upool->parent.size = 0;

    return &(upool->parent);
}
/* }}} */


/*{{{ lpc_realpool implementation */

/* {{{ typedefs */
typedef struct _pool_block
{
    size_t              avail;
    size_t              capacity;
    unsigned char       *mark;
    struct _pool_block  *next;
    unsigned             :0; /* this should align to word */
    /* data comes here */
}pool_block;

/*
   parts in ? are optional and turned on for fun, memory loss,
   and for something else that I forgot about ... ah, debugging

                 |--------> data[]         |<-- non word boundary (too)
   +-------------+--------------+-----------+-------------+-------------->>>
   | pool_block  | ?sizeinfo<1> | block<1>  | ?redzone<1> | ?sizeinfo<2>
   |             |  (size_t)    |           | padded left |
   +-------------+--------------+-----------+-------------+-------------->>>
 */

typedef struct _lpc_realpool lpc_realpool;

struct _lpc_realpool
{
    struct _lpc_pool parent;

    size_t     dsize;
    void       *owner;

    unsigned long count;

    pool_block *head;
    pool_block first; 
};

/* }}} */

/* {{{ redzone code */
static const unsigned char decaff[] =  {
    0xde, 0xca, 0xff, 0xc0, 0xff, 0xee, 0xba, 0xad,
    0xde, 0xca, 0xff, 0xc0, 0xff, 0xee, 0xba, 0xad,
    0xde, 0xca, 0xff, 0xc0, 0xff, 0xee, 0xba, 0xad,
    0xde, 0xca, 0xff, 0xc0, 0xff, 0xee, 0xba, 0xad
};

/* a redzone is at least 4 (0xde,0xca,0xc0,0xff) bytes */
#define REDZONE_SIZE(size) \
    ((ALIGNWORD((size)) > ((size) + 4)) ? \
        (ALIGNWORD((size)) - (size)) : /* does not change realsize */\
        ALIGNWORD((size)) - (size) + ALIGNWORD((sizeof(char)))) /* adds 1 word to realsize */

#define SIZEINFO_SIZE ALIGNWORD(sizeof(size_t))

#define MARK_REDZONE(block, redsize) do {\
       memcpy(block, decaff, redsize );\
    } while(0)

#define CHECK_REDZONE(block, redsize) (memcmp(block, decaff, redsize) == 0)

/* }}} */

#define INIT_POOL_BLOCK(rpool, entry, size) do {\
    (entry)->avail = (entry)->capacity = (size);\
    (entry)->mark =  ((unsigned char*)(entry)) + ALIGNWORD(sizeof(pool_block));\
    (entry)->next = (rpool)->head;\
    (rpool)->head = (entry);\
} while(0)

/* {{{ create_pool_block */
static pool_block* create_pool_block(lpc_realpool *rpool, size_t size TSRMLS_DC)
{
    lpc_malloc_t allocate = rpool->parent.allocate;

    size_t realsize = sizeof(pool_block) + ALIGNWORD(size);

    pool_block* entry = allocate(realsize TSRMLS_CC);

    if (!entry) {
        return NULL;
    }

    INIT_POOL_BLOCK(rpool, entry, size);
    
    rpool->parent.size += realsize;

    rpool->count++;

    return entry;
}
/* }}} */

/* {{{ lpc_realpool_alloc */
static void* lpc_realpool_alloc(lpc_pool *pool, size_t size TSRMLS_DC)
{
    lpc_realpool *rpool = (lpc_realpool*)pool;
    unsigned char *p = NULL;
    size_t realsize = ALIGNWORD(size);
    size_t poolsize;
    unsigned char *redzone  = NULL;
    size_t redsize  = 0;
    size_t *sizeinfo= NULL;
    pool_block *entry = NULL;
    unsigned long i;
    
    if(LPC_POOL_HAS_REDZONES(pool)) {
        redsize = REDZONE_SIZE(size); /* redsize might be re-using word size padding */
        realsize = size + redsize;    /* recalculating realsize */
    } else {
        redsize = realsize - size; /* use padding space */
    }

    if(LPC_POOL_HAS_SIZEINFO(pool)) {
        realsize += ALIGNWORD(sizeof(size_t));
    }

    /* upgrade the pool type to reduce overhead */
    if(rpool->count > 4 && rpool->dsize < 4096) {
        rpool->dsize = 4096;
    } else if(rpool->count > 8 && rpool->dsize < 8192) {
        rpool->dsize = 8192;
    }

    /* minimize look-back, a value of 8 seems to give similar fill-ratios (+2%)
     * as looping through the entire list. And much faster in allocations. */
    for(entry = rpool->head, i = 0; entry != NULL && (i < 8); entry = entry->next, i++) {
        if(entry->avail >= realsize) {
            goto found;
        }
    }

    poolsize = ALIGNSIZE(realsize, rpool->dsize);

    entry = create_pool_block(rpool, poolsize TSRMLS_CC);

    if(!entry) {
        return NULL;
    }

found:
    p = entry->mark;

    if(LPC_POOL_HAS_SIZEINFO(pool)) {
        sizeinfo = (size_t*)p;
        p += SIZEINFO_SIZE;
        *sizeinfo = size;
    }

    redzone = p + size;

    if(LPC_POOL_HAS_REDZONES(pool)) {
        MARK_REDZONE(redzone, redsize);
    }

#ifdef VALGRIND_MAKE_MEM_NOACCESS
    if(redsize != 0) {
        VALGRIND_MAKE_MEM_NOACCESS(redzone, redsize);
    }
#endif

    entry->avail -= realsize;
    entry->mark  += realsize;
    pool->used   += realsize;

#ifdef VALGRIND_MAKE_MEM_UNDEFINED
    /* need to write before reading data off this */
    VALGRIND_MAKE_MEM_UNDEFINED(p, size);
#endif

    return (void*)p;
}
/* }}} */

/* {{{ lpc_realpool_check_integrity */
/*
 * Checking integrity at runtime, does an
 * overwrite check only when the sizeinfo
 * is set.
 *
 * Marked as used in gcc, so that this function
 * is accessible from gdb, eventhough it is never
 * used in code in non-debug builds.
 */
static LPC_USED int lpc_realpool_check_integrity(lpc_realpool *rpool) 
{
    lpc_pool *pool = &(rpool->parent); 
    pool_block *entry;
    size_t *sizeinfo = NULL;
    unsigned char *start;
    size_t realsize;
    unsigned char   *redzone;
    size_t redsize;

    for(entry = rpool->head; entry != NULL; entry = entry->next) {
        start = (unsigned char *)entry + ALIGNWORD(sizeof(pool_block));
        if((entry->mark - start) != (entry->capacity - entry->avail)) {
            return 0;
        }
    }

    if(!LPC_POOL_HAS_REDZONES(pool) ||
        !LPC_POOL_HAS_SIZEINFO(pool)) {
        (void)pool; /* remove unused warning */
        return 1;
    }

    for(entry = rpool->head; entry != NULL; entry = entry->next) {
        start = (unsigned char *)entry + ALIGNWORD(sizeof(pool_block));

        while(start < entry->mark) {
            sizeinfo = (size_t*)start;
            /* redzone starts where real data ends, in a non-word boundary
             * redsize is at least 4 bytes + whatever's needed to make it
             * to another word boundary.
             */
            redzone = start + SIZEINFO_SIZE + (*sizeinfo);
            redsize = REDZONE_SIZE(*sizeinfo);
#ifdef VALGRIND_MAKE_MEM_DEFINED
            VALGRIND_MAKE_MEM_DEFINED(redzone, redsize);
#endif
            if(!CHECK_REDZONE(redzone, redsize))
            {
                /*
                fprintf(stderr, "Redzone check failed for %p\n", 
                                start + ALIGNWORD(sizeof(size_t)));*/
                return 0;
            }
#ifdef VALGRIND_MAKE_MEM_NOACCESS
            VALGRIND_MAKE_MEM_NOACCESS(redzone, redsize);
#endif
            realsize = SIZEINFO_SIZE + *sizeinfo + redsize;
            start += realsize;
        }
    }

    return 1;
}
/* }}} */

/* {{{ lpc_realpool_free */
/*
 * free does not do anything other than
 * check for redzone values when free'ing
 * data areas.
 */
static void lpc_realpool_free(lpc_pool *pool, void *p TSRMLS_DC)
{
}
/* }}} */

static void lpc_realpool_cleanup(lpc_pool *pool TSRMLS_DC) 
{
    pool_block *entry;
    pool_block *tmp;
    lpc_realpool *rpool = (lpc_realpool*)pool;
    lpc_free_t deallocate = pool->deallocate;

    assert(lpc_realpool_check_integrity(rpool)!=0);

    entry = rpool->head;

    while(entry->next != NULL) {
        tmp = entry->next;
        deallocate(entry TSRMLS_CC);
        entry = tmp;
    }
}

/* {{{ lpc_realpool_create */
static lpc_pool* lpc_realpool_create(lpc_pool_type type, lpc_malloc_t allocate, lpc_free_t deallocate, 
                                                         lpc_protect_t protect, lpc_unprotect_t unprotect
                                                         TSRMLS_DC)
{

    size_t dsize = 0;
    lpc_realpool *rpool;

    switch(type & LPC_POOL_SIZE_MASK) {
        case LPC_SMALL_POOL:
            dsize = 512;
            break;

        case LPC_LARGE_POOL:
            dsize = 8192;
            break;

        case LPC_MEDIUM_POOL:
            dsize = 4096;
            break;

        default:
            return NULL;
    }

    rpool = (lpc_realpool*)allocate((sizeof(lpc_realpool) + ALIGNWORD(dsize)) TSRMLS_CC);

    if(!rpool) {
        return NULL;
    }

    rpool->parent.type = type;

    rpool->parent.allocate = allocate;
    rpool->parent.deallocate = deallocate;

    rpool->parent.size = sizeof(lpc_realpool) + ALIGNWORD(dsize);

    rpool->parent.palloc = lpc_realpool_alloc;
    rpool->parent.pfree  = lpc_realpool_free;

    rpool->parent.protect = protect;
    rpool->parent.unprotect = unprotect;

    rpool->parent.cleanup = lpc_realpool_cleanup;

    rpool->dsize = dsize;
    rpool->head = NULL;
    rpool->count = 0;

    INIT_POOL_BLOCK(rpool, &(rpool->first), dsize);

    return &(rpool->parent);
}


/* }}} */

/* {{{ lpc_pool_init */
void lpc_pool_init()
{
    /* put all ye sanity checks here */
    assert(sizeof(decaff) > REDZONE_SIZE(ALIGNWORD(sizeof(char))));
    assert(sizeof(pool_block) == ALIGNWORD(sizeof(pool_block)));
#if LPC_POOL_DEBUG
    assert((LPC_POOL_SIZE_MASK & (LPC_POOL_SIZEINFO | LPC_POOL_REDZONES)) == 0);
#endif
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
