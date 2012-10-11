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
  | Authors: Brian Shire <shire@php.net>                                 |
  +----------------------------------------------------------------------+

 */

/* $Id: lpc_bin.c 307333 2011-01-10 09:02:13Z pajoye $ */

/* Creates a binary architecture specific output to a string or file containing
 * the current cache contents for both fies and user variables.  This is accomplished
 * via the lpc_copy_* functions and "swizzling" pointer values to a position
 * independent value, and unswizzling them on restoration.
 */

#include "lpc_globals.h"
#include "lpc_bin.h"
#include "lpc_zend.h"
#include "lpc_pool.h"
#include "ext/standard/md5.h"
#include "ext/standard/crc32.h"

#define LPC_BINDUMP_DEBUG 1

#if LPC_BINDUMP_DEBUG

#define SWIZZLE(bd, ptr)  \
    do { \
        if((long)bd < (long)ptr && (ulong)ptr < ((long)bd + bd->size)) { \
            printf("SWIZZLE: %x ~> ", ptr); \
            ptr = (void*)((long)(ptr) - (long)(bd)); \
            printf("%x in %s on line %d", ptr, __FILE__, __LINE__); \
        } else if((long)ptr > bd->size) { /* not swizzled */ \
            lpc_error("pointer to be swizzled is not within allowed memory range! (%x < %x < %x) in %s on %d" TSRMLS_CC, (long)bd, ptr, ((long)bd + bd->size), __FILE__, __LINE__); \
            return; \
        } \
        printf("\n"); \
    } while(0);

#define UNSWIZZLE(bd, ptr)  \
    do { \
      printf("UNSWIZZLE: %x -> ", ptr); \
      ptr = (void*)((long)(ptr) + (long)(bd)); \
      printf("%x in %s on line %d", ptr, __FILE__, __LINE__); \
    } while(0);

#else    /* !LPC_BINDUMP_DEBUG */

#define SWIZZLE(bd, ptr) \
    do { \
        if((long)bd < (long)ptr && (ulong)ptr < ((long)bd + bd->size)) { \
            ptr = (void*)((long)(ptr) - (long)(bd)); \
        } else if((ulong)ptr > bd->size) { /* not swizzled */ \
            lpc_error("pointer to be swizzled is not within allowed memory range! (%x < %x < %x) in %s on %d" TSRMLS_CC, (long)bd, ptr, ((long)bd + bd->size), __FILE__, __LINE__); \
            return NULL; \
        } \
    } while(0);

#define UNSWIZZLE(bd, ptr) \
    do { \
      ptr = (void*)((long)(ptr) + (long)(bd)); \
    } while(0);

#endif
#if 0
static void *lpc_bd_alloc(size_t size TSRMLS_DC);
static void lpc_bd_free(void *ptr TSRMLS_DC);
static void *lpc_bd_alloc_ex(void *ptr_new, size_t size TSRMLS_DC);
#endif
typedef void (*lpc_swizzle_cb_t)(lpc_bd_t *bd, zend_llist *ll, void *ptr TSRMLS_DC);

#if LPC_BINDUMP_DEBUG
#define lpc_swizzle_ptr(bd, ll, ptr) _lpc_swizzle_ptr(bd, ll, (void*)ptr, __FILE__, __LINE__ TSRMLS_CC)
#else
#define lpc_swizzle_ptr(bd, ll, ptr) _lpc_swizzle_ptr(bd, ll, (void*)ptr, NULL, 0 TSRMLS_CC)
#endif

static void _lpc_swizzle_ptr(lpc_bd_t *bd, zend_llist *ll, void **ptr, const char* file, int line TSRMLS_DC);
static void lpc_swizzle_function(lpc_bd_t *bd, zend_llist *ll, zend_function *func TSRMLS_DC);
static void lpc_swizzle_class_entry(lpc_bd_t *bd, zend_llist *ll, zend_class_entry *ce TSRMLS_DC);
static void lpc_swizzle_hashtable(lpc_bd_t *bd, zend_llist *ll, HashTable *ht, lpc_swizzle_cb_t swizzle_cb, int is_ptr TSRMLS_DC);
static void lpc_swizzle_zval(lpc_bd_t *bd, zend_llist *ll, zval *zv TSRMLS_DC);
static void lpc_swizzle_op_array(lpc_bd_t *bd, zend_llist *ll, zend_op_array *op_array TSRMLS_DC);
static void lpc_swizzle_property_info(lpc_bd_t *bd, zend_llist *ll, zend_property_info *pi TSRMLS_DC);
static void lpc_swizzle_function_entry(lpc_bd_t *bd, zend_llist *ll, const zend_function_entry *fe TSRMLS_DC);
static void lpc_swizzle_arg_info_array(lpc_bd_t *bd, zend_llist *ll, const zend_arg_info* arg_info_array, uint num_args TSRMLS_DC);

static lpc_bd_t* lpc_swizzle_bd(lpc_bd_t* bd, zend_llist *ll TSRMLS_DC);
static int lpc_unswizzle_bd(lpc_bd_t *bd, int flags TSRMLS_DC);
#if 0
/* {{{ lpc_bd_alloc
 *  callback for copy_* functions */
static void *lpc_bd_alloc(size_t size TSRMLS_DC) {
    return lpc_bd_alloc_ex(NULL, size TSRMLS_CC);
} /* }}} */


/* {{{ lpc_bd_free
 *  callback for copy_* functions */
static void lpc_bd_free(void *ptr TSRMLS_DC) {
    size_t *size;
    if(zend_hash_index_find(&LPCG(lpc_bd_alloc_list), (ulong)ptr, (void**)&size) == FAILURE) {
        lpc_error("lpc_bd_free could not free pointer (not found in list: %x)" TSRMLS_CC, ptr);
        return;
    }
    LPCG(lpc_bd_alloc_ptr) = (void*)((size_t)LPCG(lpc_bd_alloc_ptr) - *size);
    zend_hash_index_del(&LPCG(lpc_bd_alloc_list), (ulong)ptr);
} /* }}} */


/* {{{ lpc_bd_alloc_ex
 *  set ranges or allocate a block of data from an already (e)malloc'd range.
 *  if ptr_new is not NULL, it will reset the pointer to start at ptr_new,
 *  with a range of size.  If ptr_new is NULL, returns the next available
 *  block of given size.
 */
static void *lpc_bd_alloc_ex(void *ptr_new, size_t size TSRMLS_DC) {
    void *rval;

    rval = LPCG(lpc_bd_alloc_ptr);
    if(ptr_new != NULL) {  /* reset ptrs */
      LPCG(lpc_bd_alloc_ptr) = ptr_new;
      LPCG(lpc_bd_alloc_ubptr) = (void*)((unsigned char *) ptr_new + size);
    } else {  /* alloc block */
      LPCG(lpc_bd_alloc_ptr) = (void*)((size_t)LPCG(lpc_bd_alloc_ptr) + size);
#if LPC_BINDUMP_DEBUG
      lpc_notice("lpc_bd_alloc: rval: 0x%x  ptr: 0x%x  ubptr: 0x%x  size: %d" TSRMLS_CC, rval, LPCG(lpc_bd_alloc_ptr), LPCG(lpc_bd_alloc_ubptr), size);
#endif
      if(LPCG(lpc_bd_alloc_ptr) > LPCG(lpc_bd_alloc_ubptr)) {
          lpc_error("Exceeded bounds check in lpc_bd_alloc_ex by %d bytes." TSRMLS_CC, (unsigned char *) LPCG(lpc_bd_alloc_ptr) - (unsigned char *) LPCG(lpc_bd_alloc_ubptr));
          return NULL;
      }
      zend_hash_index_update(&LPCG(lpc_bd_alloc_list), (ulong)rval, &size, sizeof(size_t), NULL);
    }

    return rval;
} /* }}} */
#endif

/* {{{ _lpc_swizzle_ptr */
static void _lpc_swizzle_ptr(lpc_bd_t *bd, zend_llist *ll, void **ptr, const char* file, int line TSRMLS_DC) {
    if(*ptr) {
        if((long)bd < (long)*ptr && (ulong)*ptr < ((long)bd + bd->size)) {
            zend_llist_add_element(ll, &ptr);
#if LPC_BINDUMP_DEBUG
            printf("[%06d] lpc_swizzle_ptr: %x -> %x ", zend_llist_count(ll), ptr, *ptr);
            printf(" in %s on line %d", file, line);
#endif
        } else if((ulong)ptr > bd->size) {
            lpc_error("pointer to be swizzled is not within allowed memory range! (%x < %x < %x) in %s on %d" TSRMLS_CC, (long)bd, *ptr, ((long)bd + bd->size), file, line); \
            return;
        }
    }
} /* }}} */


/* {{{ lpc_swizzle_op_array */
static void lpc_swizzle_op_array(lpc_bd_t *bd, zend_llist *ll, zend_op_array *op_array TSRMLS_DC) {
    uint i;

#ifdef ZEND_ENGINE_2
    lpc_swizzle_arg_info_array(bd, ll, op_array->arg_info, op_array->num_args TSRMLS_CC);
    lpc_swizzle_ptr(bd, ll, &op_array->arg_info);
#else
    if (op_array->arg_types) {
        lpc_swizzle_ptr(bd, ll, &op_array->arg_types);
    }
#endif

    lpc_swizzle_ptr(bd, ll, &op_array->function_name);
    lpc_swizzle_ptr(bd, ll, &op_array->filename);
    lpc_swizzle_ptr(bd, ll, &op_array->refcount);

    /* swizzle op_array */
    for(i=0; i < op_array->last; i++) {
#ifndef ZEND_ENGINE_2_4
        if(op_array->opcodes[i].result.op_type == IS_CONST) {
            lpc_swizzle_zval(bd, ll, &op_array->opcodes[i].result.u.constant TSRMLS_CC);
        }
        if(op_array->opcodes[i].op1.op_type == IS_CONST) {
            lpc_swizzle_zval(bd, ll, &op_array->opcodes[i].op1.u.constant TSRMLS_CC);
        }
        if(op_array->opcodes[i].op2.op_type == IS_CONST) {
            lpc_swizzle_zval(bd, ll, &op_array->opcodes[i].op2.u.constant TSRMLS_CC);
        }
#endif
        switch (op_array->opcodes[i].opcode) {
            case ZEND_JMP:
#ifdef ZEND_ENGINE_2_4
                lpc_swizzle_ptr(bd, ll, &op_array->opcodes[i].op1.jmp_addr);
#else
                lpc_swizzle_ptr(bd, ll, &op_array->opcodes[i].op1.u.jmp_addr);
#endif
            case ZEND_JMPZ:
            case ZEND_JMPNZ:
            case ZEND_JMPZ_EX:
            case ZEND_JMPNZ_EX:
#ifdef ZEND_ENGINE_2_4
                lpc_swizzle_ptr(bd, ll, &op_array->opcodes[i].op2.jmp_addr);
#else
                lpc_swizzle_ptr(bd, ll, &op_array->opcodes[i].op2.u.jmp_addr);
#endif
        }
    }
    lpc_swizzle_ptr(bd, ll, &op_array->opcodes);

    /* break-continue array ptr */
    if(op_array->brk_cont_array) {
        lpc_swizzle_ptr(bd, ll, &op_array->brk_cont_array);
    }

    /* static voriables */
    if(op_array->static_variables) {
        lpc_swizzle_hashtable(bd, ll, op_array->static_variables, (lpc_swizzle_cb_t)lpc_swizzle_zval, 1 TSRMLS_CC);
        lpc_swizzle_ptr(bd, ll, &op_array->static_variables);
    }

#ifdef ZEND_ENGINE_2
    /* try-catch */
    if(op_array->try_catch_array) {
        lpc_swizzle_ptr(bd, ll, &op_array->try_catch_array);
    }
#endif

#ifdef ZEND_ENGINE_2_1 /* PHP 5.1 */
    /* vars */
    if(op_array->vars) {
        for(i=0; (signed int) i < op_array->last_var; i++) {
            lpc_swizzle_ptr(bd, ll, &op_array->vars[i].name);
        }
        lpc_swizzle_ptr(bd, ll, &op_array->vars);
    }
#endif

#ifdef ZEND_ENGINE_2
    /* doc comment */
    if(op_array->doc_comment) {
        lpc_swizzle_ptr(bd, ll, &op_array->doc_comment);
    }
#endif

} /* }}} */


/* {{{ lpc_swizzle_function */
static void lpc_swizzle_function(lpc_bd_t *bd, zend_llist *ll, zend_function *func TSRMLS_DC) {
    lpc_swizzle_op_array(bd, ll, &func->op_array TSRMLS_CC);
#ifdef ZEND_ENGINE_2
    if(func->common.scope) {
        lpc_swizzle_ptr(bd, ll, &func->common.scope);
    }
#endif
} /* }}} */


/* {{{ lpc_swizzle_class_entry */
static void lpc_swizzle_class_entry(lpc_bd_t *bd, zend_llist *ll, zend_class_entry *ce TSRMLS_DC) {

    uint i;

    if(ce->name) {
        lpc_swizzle_ptr(bd, ll, &ce->name);
    }

    if (ce->type == ZEND_USER_CLASS && ZEND_CE_DOC_COMMENT(ce)) {
        lpc_swizzle_ptr(bd, ll, &ZEND_CE_DOC_COMMENT(ce));
    }

#ifndef ZEND_ENGINE_2
    lpc_swizzle_ptr(bd, ll, &ce->refcount);
#endif

    lpc_swizzle_hashtable(bd, ll, &ce->function_table, (lpc_swizzle_cb_t)lpc_swizzle_function, 0 TSRMLS_CC);
#ifdef ZEND_ENGINE_2_4
    if (ce->default_properties_table) {
        int i;

        for (i = 0; i < ce->default_properties_count; i++) {
            if (ce->default_properties_table[i]) {
                lpc_swizzle_ptr(bd, ll, &ce->default_properties_table[i]);
                lpc_swizzle_zval(bd, ll, ce->default_properties_table[i] TSRMLS_CC);
            }
        }
    }
#else
    lpc_swizzle_hashtable(bd, ll, &ce->default_properties, (lpc_swizzle_cb_t)lpc_swizzle_zval, 1 TSRMLS_CC);
#endif

#ifdef ZEND_ENGINE_2
    lpc_swizzle_hashtable(bd, ll, &ce->properties_info, (lpc_swizzle_cb_t)lpc_swizzle_property_info, 0 TSRMLS_CC);
#endif

#ifdef ZEND_ENGINE_2_4
    if (ce->default_static_members_table) {
        int i;

        for (i = 0; i < ce->default_static_members_count; i++) {
            if (ce->default_static_members_table[i]) {
                lpc_swizzle_ptr(bd, ll, &ce->default_static_members_table[i]);
                lpc_swizzle_zval(bd, ll, ce->default_static_members_table[i] TSRMLS_CC);
            }
        }
    }
    ce->static_members_table = ce->default_static_members_table;
#else
    lpc_swizzle_hashtable(bd, ll, &ce->default_static_members, (lpc_swizzle_cb_t)lpc_swizzle_zval, 1 TSRMLS_CC);

    if(ce->static_members != &ce->default_static_members) {
        lpc_swizzle_hashtable(bd, ll, ce->static_members, (lpc_swizzle_cb_t)lpc_swizzle_zval, 1 TSRMLS_CC);
    } else {
        lpc_swizzle_ptr(bd, ll, &ce->static_members);
    }
#endif

    lpc_swizzle_hashtable(bd, ll, &ce->constants_table, (lpc_swizzle_cb_t)lpc_swizzle_zval, 1 TSRMLS_CC);

    if(ce->type == ZEND_INTERNAL_CLASS &&  ZEND_CE_BUILTIN_FUNCTIONS(ce)) {
        for(i=0; ZEND_CE_BUILTIN_FUNCTIONS(ce)[i].fname; i++) {
            lpc_swizzle_function_entry(bd, ll, &ZEND_CE_BUILTIN_FUNCTIONS(ce)[i] TSRMLS_CC);
        }
    }

    lpc_swizzle_ptr(bd, ll, &ce->constructor);
    lpc_swizzle_ptr(bd, ll, &ce->destructor);
    lpc_swizzle_ptr(bd, ll, &ce->clone);
    lpc_swizzle_ptr(bd, ll, &ce->__get);
    lpc_swizzle_ptr(bd, ll, &ce->__set);
    lpc_swizzle_ptr(bd, ll, &ce->__unset);
    lpc_swizzle_ptr(bd, ll, &ce->__isset);
    lpc_swizzle_ptr(bd, ll, &ce->__call);
    lpc_swizzle_ptr(bd, ll, &ce->serialize_func);
    lpc_swizzle_ptr(bd, ll, &ce->unserialize_func);

#ifdef ZEND_ENGINE_2_2
    lpc_swizzle_ptr(bd, ll, &ce->__tostring);
#endif

    if (ce->type == ZEND_USER_CLASS) {
        lpc_swizzle_ptr(bd, ll, &ZEND_CE_FILENAME(ce));
    }
} /* }}} */


/* {{{ lpc_swizzle_property_info */
static void lpc_swizzle_property_info(lpc_bd_t *bd, zend_llist *ll, zend_property_info *pi TSRMLS_DC) {
    lpc_swizzle_ptr(bd, ll, &pi->name);
    lpc_swizzle_ptr(bd, ll, &pi->doc_comment);

#ifdef ZEND_ENGINE_2_2
    lpc_swizzle_ptr(bd, ll, &pi->ce);
#endif
} /* }}} */


/* {{{ lpc_swizzle_function_entry */
static void lpc_swizzle_function_entry(lpc_bd_t *bd, zend_llist *ll, const zend_function_entry *fe TSRMLS_DC) {
    lpc_swizzle_ptr(bd, ll, &fe->fname);
    lpc_swizzle_arg_info_array(bd, ll, fe->arg_info, fe->num_args TSRMLS_CC);
    lpc_swizzle_ptr(bd, ll, &fe->arg_info);
} /* }}} */


/* {{{ lpc_swizzle_arg_info_array */
static void lpc_swizzle_arg_info_array(lpc_bd_t *bd, zend_llist *ll, const zend_arg_info* arg_info_array, uint num_args TSRMLS_DC) {
    if(arg_info_array) {
        uint i;

        for(i=0; i < num_args; i++) {
            lpc_swizzle_ptr(bd, ll, &arg_info_array[i].name);
            lpc_swizzle_ptr(bd, ll, &arg_info_array[i].class_name);
        }
    }

} /* }}} */


/* {{{ lpc_swizzle_hashtable */
static void lpc_swizzle_hashtable(lpc_bd_t *bd, zend_llist *ll, HashTable *ht, lpc_swizzle_cb_t swizzle_cb, int is_ptr TSRMLS_DC) {
    uint i;
    Bucket **bp, **bp_prev;

    bp = &ht->pListHead;
    while(*bp) {
        bp_prev = bp;
        bp = &(*bp)->pListNext;
        if(is_ptr) {
            swizzle_cb(bd, ll, *(void**)(*bp_prev)->pData TSRMLS_CC);
            lpc_swizzle_ptr(bd, ll, (*bp_prev)->pData);
        } else {
            swizzle_cb(bd, ll, (void**)(*bp_prev)->pData TSRMLS_CC);
        }
        lpc_swizzle_ptr(bd, ll, &(*bp_prev)->pData);
        if((*bp_prev)->pDataPtr) {
            lpc_swizzle_ptr(bd, ll, &(*bp_prev)->pDataPtr);
        }
        if((*bp_prev)->pListLast) {
            lpc_swizzle_ptr(bd, ll, &(*bp_prev)->pListLast);
        }
        if((*bp_prev)->pNext) {
            lpc_swizzle_ptr(bd, ll, &(*bp_prev)->pNext);
        }
        if((*bp_prev)->pLast) {
            lpc_swizzle_ptr(bd, ll, &(*bp_prev)->pLast);
        }
        lpc_swizzle_ptr(bd, ll, bp_prev);
    }
    for(i=0; i < ht->nTableSize; i++) {
        if(ht->arBuckets[i]) {
            lpc_swizzle_ptr(bd, ll, &ht->arBuckets[i]);
        }
    }
    lpc_swizzle_ptr(bd, ll, &ht->pListTail);

    lpc_swizzle_ptr(bd, ll, &ht->arBuckets);
} /* }}} */


/* {{{ lpc_swizzle_zval */
static void lpc_swizzle_zval(lpc_bd_t *bd, zend_llist *ll, zval *zv TSRMLS_DC) {

    if(LPCG(copied_zvals).nTableSize) {
        if(zend_hash_index_exists(&LPCG(copied_zvals), (ulong)zv)) {
          return;
        }
        zend_hash_index_update(&LPCG(copied_zvals), (ulong)zv, (void**)&zv, sizeof(zval*), NULL);
    }

    switch(zv->type & ~IS_CONSTANT_INDEX) {
        case IS_NULL:
        case IS_LONG:
        case IS_DOUBLE:
        case IS_BOOL:
        case IS_RESOURCE:
            /* nothing to do */
            break;
        case IS_CONSTANT:
        case IS_STRING:
            lpc_swizzle_ptr(bd, ll, &zv->value.str.val);
            break;
        case IS_ARRAY:
        case IS_CONSTANT_ARRAY:
            lpc_swizzle_hashtable(bd, ll, zv->value.ht, (lpc_swizzle_cb_t)lpc_swizzle_zval, 1 TSRMLS_CC);
            lpc_swizzle_ptr(bd, ll, &zv->value.ht);
            break;
        case IS_OBJECT:
            break;
        default:
            assert(0); /* shouldn't happen */
    }
} /* }}} */


/* {{{ lpc_swizzle_bd */
static lpc_bd_t* lpc_swizzle_bd(lpc_bd_t* bd, zend_llist *ll TSRMLS_DC) {
    int count, i;
    PHP_MD5_CTX context;
    unsigned char digest[16];
    register php_uint32 crc;
    php_uint32 crcinit = 0;
    char *crc_p;
    void ***ptr;
    void ***ptr_list;

    count = zend_llist_count(ll);
    ptr_list = emalloc(count * sizeof(void**));
    ptr = zend_llist_get_first(ll);
    for(i=0; i < count; i++) {
#if LPC_BINDUMP_DEBUG
        printf("[%06d] ", i+1);
#endif
        SWIZZLE(bd, **ptr); /* swizzle ptr */
        if((long)bd < (long)*ptr && (ulong)*ptr < ((long)bd + bd->size)) {  /* exclude ptrs that aren't actually included in the ptr list */
#if LPC_BINDUMP_DEBUG
            printf("[------] ");
#endif
            SWIZZLE(bd, *ptr);  /* swizzle ptr list */
            ptr_list[i] = *ptr;
        }
        ptr = zend_llist_get_next(ll);
    }
    SWIZZLE(bd, bd->entries);

    if(count > 0) {
        bd = erealloc(bd, bd->size + (count * sizeof(void**)));
        bd->num_swizzled_ptrs = count;
        bd->swizzled_ptrs = (void***)((unsigned char *)bd + bd->size -2);   /* extra -1 for null termination */
        bd->size += count * sizeof(void**);
        memcpy(bd->swizzled_ptrs, ptr_list, count * sizeof(void**));
        SWIZZLE(bd, bd->swizzled_ptrs);
    } else {
        bd->num_swizzled_ptrs = 0;
        bd->swizzled_ptrs = NULL;
    }
    ((char*)bd)[bd->size-1] = 0;  /* silence null termination for zval strings */
    efree(ptr_list);
    bd->swizzled = 1;

    /* Generate MD5/CRC32 checksum */
    for(i=0; i<16; i++) { bd->md5[i] = 0; }
    bd->crc=0;
    PHP_MD5Init(&context);
    PHP_MD5Update(&context, (const unsigned char*)bd, bd->size);
    PHP_MD5Final(digest, &context);
    crc = crcinit^0xFFFFFFFF;
    crc_p = (char*)bd;
    for(i=bd->size; i--; ++crc_p) {
      crc = ((crc >> 8) & 0x00FFFFFF) ^ crc32tab[(crc ^ (*crc_p)) & 0xFF ];
    }
    memcpy(bd->md5, digest, 16);
    bd->crc = crc;

    return bd;
} /* }}} */


/* {{{ lpc_unswizzle_bd */
static int lpc_unswizzle_bd(lpc_bd_t *bd, int flags TSRMLS_DC) {
    int i;
    unsigned char md5_orig[16];
    unsigned char digest[16];
    PHP_MD5_CTX context;
    register php_uint32 crc;
    php_uint32 crcinit = 0;
    php_uint32 crc_orig;
    char *crc_p;

    /* Verify the md5 or crc32 before we unswizzle */
    memcpy(md5_orig, bd->md5, 16);
    for(i=0; i<16; i++) { bd->md5[i] = 0; }
    crc_orig = bd->crc;
    bd->crc=0;
    if(flags & LPC_BIN_VERIFY_MD5) {
        PHP_MD5Init(&context);
        PHP_MD5Update(&context, (const unsigned char*)bd, bd->size);
        PHP_MD5Final(digest, &context);
        if(memcmp(md5_orig, digest, 16)) {
            lpc_error("MD5 checksum of binary dump failed." TSRMLS_CC);
            return -1;
        }
    }
    if(flags & LPC_BIN_VERIFY_CRC32) {
        crc = crcinit^0xFFFFFFFF;
        crc_p = (char*)bd;
        for(i=bd->size; i--; ++crc_p) {
          crc = ((crc >> 8) & 0x00FFFFFF) ^ crc32tab[(crc ^ (*crc_p)) & 0xFF ];
        }
        if(crc_orig != crc) {
            lpc_error("CRC32 checksum of binary dump failed." TSRMLS_CC);
            return -1;
        }
    }
    memcpy(bd->md5, md5_orig, 16); /* add back md5 checksum */
    bd->crc = crc_orig;

    UNSWIZZLE(bd, bd->entries);
    UNSWIZZLE(bd, bd->swizzled_ptrs);
    for(i=0; i < bd->num_swizzled_ptrs; i++) {
        if(bd->swizzled_ptrs[i]) {
            UNSWIZZLE(bd, bd->swizzled_ptrs[i]);
            if(*bd->swizzled_ptrs[i] && (*bd->swizzled_ptrs[i] < (void*)bd)) {
                UNSWIZZLE(bd, *bd->swizzled_ptrs[i]);
            }
        }
    }

    bd->swizzled=0;

    return 0;
} /* }}} */


/* {{{ lpc_bin_checkfilter */
static int lpc_bin_checkfilter(HashTable *filter, const char *key, uint key_len) {
    zval **zptr;

    if(filter == NULL) {
        return 1;
    }

    if(zend_hash_find(filter, (char*)key, key_len, (void**)&zptr) == SUCCESS) {
        if(Z_TYPE_PP(zptr) == IS_LONG && Z_LVAL_PP(zptr) == 0) {
            return 0;
        }
    } else {
        return 0;
    }


    return 1;
} /* }}} */

/* {{{ lpc_bin_fixup_op_array */
static inline void lpc_bin_fixup_op_array(zend_op_array *op_array) {
    ulong i;
    for (i = 0; i < op_array->last; i++) {
        op_array->opcodes[i].handler = zend_opcode_handlers[LPC_OPCODE_HANDLER_DECODE(&op_array->opcodes[i])];
    }
}
/* }}} */

/* {{{ lpc_bin_fixup_class_entry */
static inline void lpc_bin_fixup_class_entry(zend_class_entry *ce) {
    zend_function *fe;
    HashPosition hpos;

    /* fixup the opcodes in each method */
    zend_hash_internal_pointer_reset_ex(&ce->function_table, &hpos);
    while(zend_hash_get_current_data_ex(&ce->function_table, (void**)&fe, &hpos) == SUCCESS) {
        lpc_bin_fixup_op_array(&fe->op_array);
        zend_hash_move_forward_ex(&ce->function_table, &hpos);
    }

    /* fixup hashtable destructor pointers */
    ce->function_table.pDestructor = (dtor_func_t)zend_function_dtor;
#ifndef ZEND_ENGINE_2_4
    ce->default_properties.pDestructor = (dtor_func_t)zval_ptr_dtor_wrapper;
#endif
    ce->properties_info.pDestructor = (dtor_func_t)zval_ptr_dtor_wrapper;
#ifndef ZEND_ENGINE_2_4
    ce->default_static_members.pDestructor = (dtor_func_t)zval_ptr_dtor_wrapper;
    if (ce->static_members) {
        ce->static_members->pDestructor = (dtor_func_t)zval_ptr_dtor_wrapper;
    }
#endif
    ce->constants_table.pDestructor = (dtor_func_t)zval_ptr_dtor_wrapper;
}
/* }}} */

/* {{{ lpc_bin_dump */
/* The Binary Dump block (bd) the object returned by lpc_bin_dump(). Rather than use a standard serialiser is 
   built as a stack of records.  
     *  The dumper first scans the cache slots to match files and sums the sizes of the lpc_bd_entry, the 
        lpc_cache_entry_value and the memory used by the file( plus a couple of pointers)
	 *  It then emallocs the lpc_pool structure, and 
     *  It then uses the LPC_BDPOOL type to allocate this memory before using the pool_allocator to build
        the bd block.
     *  So the following header elements are allocated 
   * 
*/
lpc_bd_t* lpc_bin_dump(HashTable *files TSRMLS_DC) {

    int i;
    uint fcount;
    slot_t *sp;
    lpc_bd_entry_t *ep;
    int count=0;
    lpc_bd_t *bd;
    zend_llist ll;
    zend_function *efp, *sfp;
    long size=0;
    lpc_context_t ctxt;
    lpc_pool *pool_ptr;
    lpc_cache_t* lpc_cache = LPCG(lpc_cache);

    zend_llist_init(&ll, sizeof(void*), NULL, 0);

    /* flip the hash for faster filter checking */
    files = lpc_flip_hash(files);
    
    /* get size and entry counts */
    for(i=0; i < lpc_cache->num_slots; i++) {
        sp = lpc_cache->slots[i];
        for(; sp != NULL; sp = sp->next) {
            if(sp->key.type == LPC_CACHE_KEY_FPFILE) {
                if(lpc_bin_checkfilter(files, sp->key.data.fpfile.fullpath, sp->key.data.fpfile.fullpath_len+1)) {
                    size += sizeof(lpc_bd_entry_t*) + sizeof(lpc_bd_entry_t);
                    size += sp->value->mem_size - (sizeof(lpc_cache_entry_t) - sizeof(lpc_cache_entry_value_t));
                    count++;
                }
            } else {
                /* TODO: Currently we don't support LPC_CACHE_KEY_FILE type.  We need to store the path and re-stat on load */
                lpc_warning("Excluding some files from lpc_bin_dump[file].  Cached files must be included using full path with lpc.stat=0." TSRMLS_CC);
            }
        }
    }

    size += sizeof(lpc_bd_t) +1;  /* +1 for null termination */

	/* Create the LPC_BDPOOL and allocate the lpc_bd structure as the header record */ 

    pool_ptr        = lpc_pool_create(LPC_SERIALPOOL);
	ctxt.pool       = pool_ptr;
	bd              = lpc_pool_alloc(ctxt.pool, sizeof(lpc_bd_t)); 
    ctxt.copy       = LPC_COPY_IN_USER;  /* avoid stupid ALLOC_ZVAL calls here, hack */
    bd->num_entries = count;

	/* Next allocate the lpc_bd_entry records, one for each file */
    bd->entries     = lpc_pool_alloc(pool_ptr, sizeof(lpc_bd_entry_t) * count);

	/* Create a macro to map the pool allocator to lpc_bd_alloc() to minimise source changes */
#define lpc_bd_alloc(ntsrmls) _lpc_pool_alloc(ctxt.pool, ntsrmls ZEND_FILE_LINE_CC)

    /* File entries */
    for(i=0; i < lpc_cache->num_slots; i++) {
        for(sp=lpc_cache->slots[i]; sp != NULL; sp = sp->next) {
            if(sp->key.type == LPC_CACHE_KEY_FPFILE) {
                if(lpc_bin_checkfilter(files, sp->key.data.fpfile.fullpath, sp->key.data.fpfile.fullpath_len+1)) {
                    ep = &bd->entries[count];
                    ep->type = sp->key.type;
                    ep->val.file.filename = lpc_bd_alloc(strlen(sp->value->data.file.filename) + 1 TSRMLS_CC);
                    strcpy(ep->val.file.filename, sp->value->data.file.filename);
                    ep->val.file.op_array = lpc_copy_op_array(NULL, sp->value->data.file.op_array, &ctxt TSRMLS_CC);

                    for(ep->num_functions=0; sp->value->data.file.functions[ep->num_functions].function != NULL;) { ep->num_functions++; }
                    ep->val.file.functions = lpc_bd_alloc(sizeof(lpc_function_t) * ep->num_functions TSRMLS_CC);
                    for(fcount=0; fcount < ep->num_functions; fcount++) {
                        memcpy(&ep->val.file.functions[fcount], &sp->value->data.file.functions[fcount], sizeof(lpc_function_t));
                        ep->val.file.functions[fcount].name = lpc_pmemcpy(sp->value->data.file.functions[fcount].name, sp->value->data.file.functions[fcount].name_len+1, ctxt.pool TSRMLS_CC);
                        ep->val.file.functions[fcount].name_len = sp->value->data.file.functions[fcount].name_len;
                        ep->val.file.functions[fcount].function = lpc_bd_alloc(sizeof(zend_function) TSRMLS_CC);
                        efp = ep->val.file.functions[fcount].function;
                        sfp = sp->value->data.file.functions[fcount].function;
                        switch(sfp->type) {
                            case ZEND_INTERNAL_FUNCTION:
                            case ZEND_OVERLOADED_FUNCTION:
                                efp->op_array = sfp->op_array;
                                break;
                            case ZEND_USER_FUNCTION:
                            case ZEND_EVAL_CODE:
                                lpc_copy_op_array(&efp->op_array, &sfp->op_array, &ctxt TSRMLS_CC);
                                break;
                            default:
                                assert(0);
                        }
#ifdef ZEND_ENGINE_2
                        efp->common.prototype = NULL;
                        efp->common.fn_flags = sfp->common.fn_flags & (~ZEND_ACC_IMPLEMENTED_ABSTRACT);
#endif
                        lpc_swizzle_ptr(bd, &ll, &ep->val.file.functions[fcount].name);
                        lpc_swizzle_ptr(bd, &ll, (void**)&ep->val.file.functions[fcount].function);
                        lpc_swizzle_op_array(bd, &ll, &efp->op_array TSRMLS_CC);
                    }


                    for(ep->num_classes=0; sp->value->data.file.classes[ep->num_classes].class_entry != NULL;) { ep->num_classes++; }
                    ep->val.file.classes = lpc_bd_alloc(sizeof(lpc_class_t) * ep->num_classes TSRMLS_CC);
                    for(fcount=0; fcount < ep->num_classes; fcount++) {
                        ep->val.file.classes[fcount].name = lpc_pmemcpy(sp->value->data.file.classes[fcount].name, sp->value->data.file.classes[fcount].name_len + 1, ctxt.pool TSRMLS_CC);
                        ep->val.file.classes[fcount].name_len = sp->value->data.file.classes[fcount].name_len;
                        ep->val.file.classes[fcount].class_entry = lpc_copy_class_entry(NULL, sp->value->data.file.classes[fcount].class_entry, &ctxt TSRMLS_CC);
                        ep->val.file.classes[fcount].parent_name = lpc_pstrdup(sp->value->data.file.classes[fcount].parent_name, ctxt.pool TSRMLS_CC);

                        lpc_swizzle_ptr(bd, &ll, &ep->val.file.classes[fcount].name);
                        lpc_swizzle_ptr(bd, &ll, &ep->val.file.classes[fcount].parent_name);
                        lpc_swizzle_class_entry(bd, &ll, ep->val.file.classes[fcount].class_entry TSRMLS_CC);
                        lpc_swizzle_ptr(bd, &ll, &ep->val.file.classes[fcount].class_entry);
                    }

                    lpc_swizzle_ptr(bd, &ll, &bd->entries[count].val.file.filename);
                    lpc_swizzle_op_array(bd, &ll, bd->entries[count].val.file.op_array TSRMLS_CC);
                    lpc_swizzle_ptr(bd, &ll, &bd->entries[count].val.file.op_array);
                    lpc_swizzle_ptr(bd, &ll, (void**)&ep->val.file.functions);
                    lpc_swizzle_ptr(bd, &ll, (void**)&ep->val.file.classes);

                    count++;
                } else {
                    /* TODO: Currently we don't support LPC_CACHE_KEY_FILE type.  We need to store the path and re-stat on load */
                }
            }
        }
    }

    /* append swizzle pointer list to bd */
    bd = lpc_swizzle_bd(bd, &ll TSRMLS_CC);
    zend_llist_destroy(&ll);

    if(files) {
        zend_hash_destroy(files);
        efree(files);
    }

    lpc_pool_destroy(pool_ptr);
#undef lpc_bd_alloc

    return bd;
} /* }}} */


/* {{{ lpc_bin_load */
int lpc_bin_load(lpc_bd_t *bd, int flags TSRMLS_DC) {

    lpc_bd_entry_t *ep;
    uint i, i2;
    int ret;
    time_t t;
    zend_op_array *alloc_op_array = NULL;
    lpc_function_t *alloc_functions = NULL;
    lpc_class_t *alloc_classes = NULL;
    lpc_cache_entry_t *cache_entry;
    lpc_cache_key_t cache_key;
    lpc_context_t ctxt;

    if (bd->swizzled) {
        if(lpc_unswizzle_bd(bd, flags TSRMLS_CC) < 0) {
            return -1;
        }
    }

    t = lpc_time();

    for(i = 0; i < bd->num_entries; i++) {
        ctxt.pool = lpc_pool_create(LPC_LOCALPOOL);
        if (!ctxt.pool) { /* TODO need to cleanup previous pools */
            lpc_warning("Unable to allocate memory for pool." TSRMLS_CC);
            goto failure;
        }
        ep = &bd->entries[i];
        switch (ep->type) {
            case LPC_CACHE_KEY_FILE:
                /* TODO: Currently we don't support LPC_CACHE_KEY_FILE type.  We need to store the path and re-stat on load (or something else perhaps?) */
                break;
            case LPC_CACHE_KEY_FPFILE:
                ctxt.copy = LPC_COPY_IN_OPCODE;

                if(! (alloc_op_array = lpc_copy_op_array(NULL, ep->val.file.op_array, &ctxt TSRMLS_CC))) {
                    goto failure;
                }
                lpc_bin_fixup_op_array(alloc_op_array);

                if(! (alloc_functions = lpc_php_malloc(sizeof(lpc_function_t) * (ep->num_functions + 1) TSRMLS_CC))) {
                    goto failure;
                }
                for(i2=0; i2 < ep->num_functions; i2++) {
                    if(! (alloc_functions[i2].name = lpc_pmemcpy(ep->val.file.functions[i2].name, ep->val.file.functions[i2].name_len + 1, ctxt.pool TSRMLS_CC))) {
                        goto failure;
                    }
                    alloc_functions[i2].name_len = ep->val.file.functions[i2].name_len;
                    if(! (alloc_functions[i2].function = lpc_php_malloc(sizeof(zend_function) TSRMLS_CC))) {
                        goto failure;
                    }
                    switch(ep->val.file.functions[i2].function->type) {
                        case ZEND_INTERNAL_FUNCTION:
                        case ZEND_OVERLOADED_FUNCTION:
                            alloc_functions[i2].function->op_array = ep->val.file.functions[i2].function->op_array;
                            break;
                        case ZEND_USER_FUNCTION:
                        case ZEND_EVAL_CODE:
                            if (!lpc_copy_op_array(&alloc_functions[i2].function->op_array, &ep->val.file.functions[i2].function->op_array, &ctxt TSRMLS_CC)) {
                                goto failure;
                            }
                            lpc_bin_fixup_op_array(&alloc_functions[i2].function->op_array);
                            break;
                        default:
                            assert(0);
                    }
#ifdef ZEND_ENGINE_2
                    alloc_functions[i2].function->common.prototype=NULL;
                    alloc_functions[i2].function->common.fn_flags=ep->val.file.functions[i2].function->common.fn_flags & (~ZEND_ACC_IMPLEMENTED_ABSTRACT);
#endif
                }
                alloc_functions[i2].name = NULL;
                alloc_functions[i2].function = NULL;

                if(! (alloc_classes = lpc_php_malloc(sizeof(lpc_class_t) * (ep->num_classes + 1) TSRMLS_CC))) {
                    goto failure;
                }
                for(i2=0; i2 < ep->num_classes; i2++) {
                    if(! (alloc_classes[i2].name = lpc_pmemcpy(ep->val.file.classes[i2].name, ep->val.file.classes[i2].name_len+1, ctxt.pool TSRMLS_CC))) {
                        goto failure;
                    }
                    alloc_classes[i2].name_len = ep->val.file.classes[i2].name_len;
                    if(! (alloc_classes[i2].class_entry = lpc_copy_class_entry(NULL, ep->val.file.classes[i2].class_entry, &ctxt TSRMLS_CC))) {
                        goto failure;
                    }
                    lpc_bin_fixup_class_entry(alloc_classes[i2].class_entry);
                    if(! (alloc_classes[i2].parent_name = lpc_pstrdup(ep->val.file.classes[i2].parent_name, ctxt.pool TSRMLS_CC))) {
                        if(ep->val.file.classes[i2].parent_name != NULL) {
                            goto failure;
                        }
                    }
                }
                alloc_classes[i2].name = NULL;
                alloc_classes[i2].class_entry = NULL;

                if(!(cache_entry = lpc_cache_make_file_entry(ep->val.file.filename, alloc_op_array, alloc_functions, alloc_classes, &ctxt TSRMLS_CC))) {
                    goto failure;
                }

                if (!lpc_cache_make_file_key(&cache_key, ep->val.file.filename, PG(include_path), t TSRMLS_CC)) {
                    goto failure;
                }

                if ((ret = lpc_cache_insert(LPCG(lpc_cache), cache_key, cache_entry, &ctxt, t TSRMLS_CC)) != 1) {
                    if(ret==-1) {
                        goto failure;
                    }
                }

                break;
            default:
                break;
       }
    }

    return 0;

failure:
    lpc_pool_destroy(ctxt.pool);
    lpc_warning("Unable to allocate memory for lpc binary load/dump functionality." TSRMLS_CC);
    return -1;
} /* }}} */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
