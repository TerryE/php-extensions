/*
  +----------------------------------------------------------------------+
  | LPC                                                                  |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006-2011 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
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

#ifndef LPC_HASHTABLE_H
#define LPC_HASHTABLE_H

/*
 * This module encapsulates most of the complexity involved in deep-copying
 * the Zend compiler data structures. The routines are allocator-agnostic, so
 * the same function can be used for copying to and from shared memory.
 */
#include "zend_compile.h"
#include "lpc.h"
#include "lpc_pool.h"

/* {{{ Enum typedefs for op-array and common */
typedef enum {
    CHECK_SKIP_ELT      = 0x0,  /* Skip the current HashTable Entry */
    CHECK_ACCEPT_ELT    = 0x1,  /* Accept the current HashTable Entry */
} lpc_check_t;
#if 0
typedef enum {
    NO_PTRS    = 0x0,  /* HashTable doesn't hold pointers */
    HOLDS_PTRS = 0x1,  /* HashTable does hold pointers */
} lpc_ptr_flag_t;

typedef enum {
    COPY_FUNCTION  = 0x0,
    COPY_ZVAL_PTR  = 0x1,
    COPY_PROP_INFO = 0x02
} lpc_ht_copy_t;

typedef struct _lpc_hashtable_copy_args_t {
    lpc_ht_copy_fun_t copy_fn;
    lpc_ptr_flag_t    holds_ptrs;
    uint              data_size;
} lpc_hashtable_copy_args_t;
#endif
#define COPY_HT(fld, copyfunc, size_type, checkfunc, optarg) \
    lpc_copy_hashtable(&dst->fld, &src->fld, pool, (lpc_ht_copy_fun_t) copyfunc, sizeof(size_type) ,\
    (lpc_ht_check_copy_fun_t) checkfunc, src, optarg)

#define COPY_HT_P(fld, copyfunc, size_type, checkfunc, optarg) \
    lpc_copy_hashtable(dst->fld, src->fld, pool, (lpc_ht_copy_fun_t) copyfunc, sizeof(size_type) ,\
    (lpc_ht_check_copy_fun_t) checkfunc, src, optarg)

/* }}} */

/* {{{ LPC abstract function typedefs */
typedef void* (*lpc_ht_copy_fun_t)(void*, const void*, lpc_pool*);
typedef lpc_check_t (*lpc_ht_check_copy_fun_t)(Bucket*, const void *, const void *);
typedef void  (*lpc_ht_fixup_fun_t)(Bucket*, zend_class_entry*, zend_class_entry*, lpc_pool*);
/* }}} */

/* {{{ Top-level copy functions for op-array and common */
extern void lpc_copy_hashtable(HashTable* dst, const HashTable* src, lpc_pool* pool, 
                              lpc_ht_copy_fun_t copy_fn, size_t rec_size,
                              lpc_ht_check_copy_fun_t check_fn,
                              const void *cf_arg1, const void *cf_arg2);
extern void lpc_fixup_hashtable(HashTable *ht, lpc_ht_fixup_fun_t fixup, 
                                zend_class_entry *src, zend_class_entry *dst, 
                                lpc_pool *pool);
/* }}} */
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
