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

   This software includes content derived from the APC extension which was
   initially contributed to PHP by Community Connect Inc. in 2002 and revised 
   in 2005 by Yahoo! Inc. See README for further details.

   All other licensing and usage conditions are those of the PHP Group.
*/

#include "lpc.h"
#include "lpc_hashtable.h"
//#include "lpc_string.h"
#include "ext/standard/php_var.h"
/* {{{ lpc_copy_hashtable
 *
 * The serial pool variant of HashTables are never used "in anger", that is they will only be
 * serially enumerate on copy-in, so many fields in the HashTable record and buckets are unused.
 * Hence a complete deep copy is not taken on copy-out, with the unused fields set to zero, enabling
 * output compression of the serial pool largley to remove any storage overheads of the unused 
 * fields.  Copy-in rebuilds the table using standard PHP HashTable functions.
 * that there is no material performance hit for this simplification.
 *
 * HashTable fields copied on copy-out (all others including arBuckets set to zero):
 *    nTableSize, nNumOfElements, pListHead, pDestructor
 * Bucket fields copied on copy-out (all others including arBuckets set to zero)
 *    h, nKeyLength, pData, pDataPtr, pListNext, arKey
 *
 * Also note that the check_fn is only applied on copy_out  
 */
void lpc_copy_hashtable(HashTable* dst, const HashTable* src, lpc_pool* pool, 
                        lpc_ht_copy_fun_t copy_fn, size_t rec_size,
                        lpc_ht_copy_element check_fn, 
                        const void *cf_arg1, const void *cf_arg2)
{ENTER(lpc_copy_hashtable)
    Bucket    *p, *q;
    uint       i;

    if (is_copy_out()) {
        if (src->arBuckets) {
            uint nNumOfElements = 0;
            Bucket **last_ptr= &dst->pListHead;
            dst->arBuckets = LPC_ALLOCATE_TAG;
            for (i=0, p = src->pListHead; i < src->nNumOfElements; i++, p = p->pListNext) {
                if (!check_fn || check_fn(p, cf_arg1, cf_arg2)) {
#ifdef ZEND_ENGINE_2_4
////// TODO: review interned string changes in PHP 5.4 to work out how this works 
////// TODO: copy out a new bucket with the deinterred key or inplace key
                    if (XXXXX_IS_INTERNED(p->arKey) {
                    } else {
                    }
#else
                    /* use *last_ptr because this is in the pool and so generates a reloc tag */
                    pool_memcpy(*last_ptr, p, sizeof(Bucket) + p->nKeyLength - 1);
                    q            = *last_ptr;
                    q->pDataPtr  = NULL;  /* These aren't used in a serial HT, so */
                    q->pListLast = NULL;  /* set to NULL to improve O/P compression */
                    q->pNext     = NULL;  /* and avoid false tagging alarms in debug */
                    q->pLast     = NULL;
                    last_ptr     = &q->pListNext;

                    /* now allocate dest rec and call the copy_fn to deepcopy in the old data */
                    pool_alloc(q->pData, rec_size);
                    copy_fn(q->pData, p->pData, pool);
                    nNumOfElements++;     
#endif
                }
            }
            *last_ptr           = NULL;
            dst->nNumOfElements = nNumOfElements;
        }
    } else { /* copy-in */
        if (!src->arBuckets) {
/////////// TODO:  validate if gdb !!!
            efree(dst->arBuckets);
            dst->arBuckets = NULL;
        } else {
            for (i=0, p = src->pListHead; i < src->nNumOfElements; i++, p = p->pListNext) {
                void *new_pData;
                zend_hash_quick_update(dst, p->arKey, p->nKeyLength, 
                                       p->h, p->pData, rec_size, &new_pData);
                copy_fn(new_pData, p->pData, pool);
            }
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

