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
  | Authors: Terry Ellison <Terry@ellisons.org.uk>                       |
  +----------------------------------------------------------------------+

   This software was derived from the APC extension which was initially 
   contributed to PHP by Community Connect Inc. in 2002 and revised in 2005 
   by Yahoo! Inc. See README for further details.

   All other licensing and usage conditions are those of the PHP Group.
*/

#include "lpc.h"
#include "lpc_hashtable.h"
#include "lpc_string.h"
#include "ext/standard/php_var.h"
/* {{{ lpc_copy_hashtable
 *
 * HashTables are the one structure where a complete deep copy is not taken on copy-out. The
 * serial pool variant of HashTables are never used "in anger", that is they will only be
 * serially enumerate on copy-in, so many fields in the HashTable record and buckets can be set
 * to zero. If we went one step further and had separate record variants for the exec and serial
 * formats dropping these unused fields, then this would mean that the check and fixup routines
 * would need to be copy in/out variants. Fortunately output compression of the serial pool means
 * that there is no material performance hit for this simplification.
 *
 * HashTable fields copied on copy-out (all others including arBuckets set to zero):
 *    nTableSize, nNumOfElements, pListHead, pDestructor
 * Bucket fields copied on copy-out (all others including arBuckets set to zero)
 *    h, nKeyLength, pData, pDataPtr, pListNext, arKey
 *
 * Note that all copied array have persistent=0 and bApplyProtection = 0. Copy-in rebuilds
 * the table using standard PHP HashTable functions.
 */
void lpc_copy_hashtable(HashTable* dst, const HashTable* src, lpc_pool* pool, 
                        lpc_ht_copy_fun_t copy_fn, size_t rec_size,
                        lpc_ht_check_copy_fun_t check_fn, 
                        const void *cf_arg1, const void *cf_arg2)
{ENTER(lpc_copy_hashtable)
#define STACKSLOTS 16
    Bucket          **list, *stack_list[STACKSLOTS], *p, **q;
    uint              i, nNumOfElements;
   /*
    * Set up minimal fields in destination HT and that's if the source has no elements
    */
    memset(dst, 0, sizeof(HashTable));
    dst->nTableSize     = src->nTableSize;
    dst->pDestructor    = src->pDestructor;
        
    if (src->nNumOfElements == 0) {
       /*
        * Some zero count HT's still have the arBuckets allocated, some don't. Set this field to 0/1
        * on copy out to flag which, and allocate accordingly on copy in.
        */
        if (is_copy_in()) {
            dst->nTableMask = dst->nTableSize - 1;
            if (src->arBuckets) {
                pool_alloc(dst->arBuckets, dst->nTableSize * sizeof(Bucket*));
                memset(dst->arBuckets, 0, dst->nTableSize * sizeof(Bucket*));
            }
        } else { /* copy-out */
            dst->arBuckets = (src->arBuckets) ? (void *) 1 : NULL;
        }
    } else {
       /*
        * Walk the pListNext chain to build a temporary list of pointers to the buckets that needs
        * to be copied out. As the life of this list is temporary it doesn't belong in the pool. It
        * can be on the stack or emalloced. A fixed stack-allocated list is used if the source HT
        * has < STACKSLOT elements and emalloced otherwise. As most HTs are small, this avoids the
        * emalloc for most HTs. Also If check_fn is set then call it; a return of CHECK_ACCEPT_ELT 
        * means that the entry needs copying.
        */
        list = (src->nNumOfElements < STACKSLOTS) ?
                    stack_list :
                    emalloc(src->nNumOfElements * sizeof(Bucket*));
 
       for (p = src->pListHead, q = list; p != NULL; p = p->pListNext) {
            if ((!check_fn) || check_fn(p, cf_arg1, cf_arg1) == CHECK_ACCEPT_ELT) {
                *q++ = p;
            }
        }
        nNumOfElements = q - list;
        assert(nNumOfElements <= src->nNumOfElements);

        if (is_copy_in()) {
////////////// TODO: add Zend 2.4 insert interned strings for keys, etc.
            dst->nTableMask = dst->nTableSize - 1;

            /* allocate and zero buckets for the new hashtable */
            pool_alloc(dst->arBuckets, dst->nTableSize * sizeof(Bucket*));
            memset(dst->arBuckets, 0, dst->nTableSize * sizeof(Bucket*));

            for (i=0; i<nNumOfElements; i++) {
                void *new_pData;
                Bucket *p = list[i];
		        zend_hash_quick_update(dst, p->arKey, p->nKeyLength, 
                                       p->h, p->pData, rec_size, &new_pData);
                copy_fn(new_pData, p->pData, pool);
            }
        } else { /* copy-out */
            Bucket **lastPtr;
            dst->nNumOfElements = nNumOfElements;
            /* flag arBuckets so that copy-in knows to allocate the arBuckets array */
            dst->arBuckets = (void *) 1;
            for (i=0; i<nNumOfElements; i++) {
#ifdef ZEND_ENGINE_2_4
                    ////// TODO: review interned string changes in PHP 5.4 to work out how this works 
                    ////// TODO: copy out a new bucket with the deinterred key or inplace key
                if (XXXXX_IS_INTERNED(list[i]>arKey) {
                } else {
                }
#else
                /* clone the bucket using *lastPtr so that the correct pool address gets tagged */
                lastPtr = (i==0) ? &(dst->pListHead) : &((*lastPtr)->pListNext);          
                pool_memcpy(*lastPtr, list[i], sizeof(Bucket) + list[i]->nKeyLength - 1);
                p = *lastPtr;
                /* alloc data storage and zero the data ptr to zero because it is recalced on copy-in */ 
                pool_alloc(p->pData, rec_size);
                p->pDataPtr  = NULL;  /* These aren't used in a serial HT, so */
                p->pListLast = NULL;  /* set to NULL to improve O/P compression */
                p->pListNext = NULL;
                p->pNext     = NULL;  /* and avoid false tagging alarms in debug */
                p->pLast     = NULL;

                /* now call the copy_fn to deepcopy the old data to the new */
                copy_fn(p->pData, list[i]->pData, pool);        
#endif
            }
        }
        if (src->nNumOfElements >= STACKSLOTS) {
            efree(list);
        }
    }
}
/* }}} */

/* {{{ lpc_fixup_hashtable */
void lpc_fixup_hashtable(HashTable *ht, lpc_ht_fixup_fun_t fixup, 
                         zend_class_entry *src, zend_class_entry *dst, lpc_pool *pool)
{ENTER(lpc_fixup_hashtable)
   /*
    * Enumerate the hash table applying the fixup function.  This function is based on 
    * zend_hash_apply() and the pListNext chain must be used as this is the only one
    * maintained on copy_out.
    */
    Bucket *p;
    p = ht->pListHead;
    while (p != NULL) {
        fixup(p, src, dst, pool);
        p = p->pListNext;
    }
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

