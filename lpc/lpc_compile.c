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
  | Authors: Daniel Cowgill <dcowgill@communityconnect.com>              |
  |          Rasmus Lerdorf <rasmus@php.net>                             |
  |          Arun C. Murthy <arunc@yahoo-inc.com>                        |
  |          Gopal Vijayaraghavan <gopalv@yahoo-inc.com>                 |
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Community Connect Inc. in 2002
   and revised in 2005 by Yahoo! Inc. to add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.
*/

#include "lpc.h"
#include "lpc_compile.h"
#include "lpc_pool.h"
#include "lpc_zend.h"
#include "lpc_string.h"
#include "ext/standard/php_var.h"
#include "ext/standard/php_smart_str.h"

#ifndef IS_CONSTANT_TYPE_MASK
#define IS_CONSTANT_TYPE_MASK (~IS_CONSTANT_INDEX)
#endif

/* {{{ Internal abstract function typedefs */
typedef void* (*ht_copy_fun_t)(void*, void*, lpc_pool*);
typedef int   (*ht_check_copy_fun_t)(Bucket*, va_list);
typedef void  (*ht_fixup_fun_t)(Bucket*, zend_class_entry*, zend_class_entry*, lpc_pool*);
/* }}} */

extern zend_compile_t *lpc_old_compile_file;

/* {{{ TAG_PTR macro definitions 
 * TAG_PTR() is an LPC introduction.  The reasoning is that ANY pointer within a LPC_SERIALPOOL must be 
 * properly identified as a relocation target.  This applies to any LVALUE within the pool which is the 
 * destination of a pointer to another location in the pool. 
 * NOTE THAT p MUST BE AN LVALUE WHEN USING THIS MACRO */
#define TAG_PTR(p) pool_tag_ptr(p)
#define TAG_SETPTR(p,exp) p = exp; pool_tag_ptr(p)

#define CHECK(p) if(!(p)) goto error

 
/* }}} */

/* {{{ internal function declarations */

void my_copy_new_functions(lpc_function_t* functions, int old_count, lpc_pool* pool);
void my_copy_new_classes(lpc_class_t* cl_array, int old_count, lpc_pool* pool);
long my_file_halt_offset(const char* filename TSRMLS_DC);

static void my_fixup_hashtable( HashTable *ht, ht_fixup_fun_t fixup, zend_class_entry *src, zend_class_entry *dst, lpc_pool*);
static void my_fixup_function( Bucket *p, zend_class_entry *src, zend_class_entry *dst, lpc_pool*);
#ifdef ZEND_ENGINE_2_2
static void my_fixup_property_info( Bucket *p, zend_class_entry *src, zend_class_entry *dst, lpc_pool* pool);
#endif

typedef enum {
    CHECK_SKIP_ELT      = 0x0,  /* Skip the current HashTable Entry */
    CHECK_ACCEPT_ELT    = 0x1,  /* Skip the current HashTable Entry */
} check_t;
/*
 * These functions are filter functions used in lpc_copy_class_entry as my_copy_hashtable callbacks to 
 * determine whether each element within the HashTable should be copied or if it is already defined /
 *  overridden in the 'current' class entry and therefore not to be inherited.  
 */
static check_t my_check_copy_function(Bucket* src, va_list args);
static check_t my_check_copy_default_property(Bucket* p, va_list args);
static check_t my_check_copy_property_info(Bucket* src, va_list args);
static check_t my_check_copy_static_member(Bucket* src, va_list args);
static check_t my_check_copy_constant(Bucket* src, va_list args);

/*
 * The "copy" functions perform deep-copies on a particular data structure (passed as the second argument).
 *
 * Note that my_copy_zval_ptr, lpc_copy_function and my_copy_property_info are used as callbacks passed to 
 * my_copy_hashtable and therefore allocate space for the destination data 
 * structure if the destination argument is null.
 */
static zval**               my_copy_zval_ptr(zval**, const zval**, lpc_pool*);
static zval*                my_copy_zval(zval*, const zval*, lpc_pool*);
static zend_property_info*  my_copy_property_info(zend_property_info* dst, zend_property_info* src, lpc_pool*);

static void                 my_copy_arg_info_array(zend_arg_info**, const zend_arg_info*, uint, lpc_pool*);
static void                 my_copy_hashtable(HashTable*, HashTable*, ht_copy_fun_t, int, lpc_pool*, ht_check_copy_fun_t, ...);

#define NO_PTRS     0
#define HOLDS_PTRS  1

/* }}} */

/* {{{ lpc_compile_cache_entry  */
zend_bool lpc_compile_cache_entry(lpc_cache_key_t *key, zend_file_handle* h, int type,
                                  zend_op_array** op_array, lpc_pool **pool_ptr TSRMLS_DC) 
{ENTER(lpc_compile_cache_entry)
    int num_functions, num_classes;
    lpc_function_t* alloc_functions;
    zend_op_array* alloc_op_array;
    lpc_class_t* alloc_classes;
	lpc_entry_block_t* entry;
	lpc_pool* pool;

   /*
	* The compilation process returns an op_array and adds any new functions and classes that were
    * compiled onto the CG(function_table) and CG(class_table) HashTables. The op_array, function and
    * class entries must be deep copied into the destination pool. These last two sets of entries
	* are determined by high-water marking the two hashs and copying any added entries.
    */ 
    num_functions   = zend_hash_num_elements(CG(function_table));
    num_classes     = zend_hash_num_elements(CG(class_table));

    CHECK(*op_array = lpc_old_compile_file(h, type TSRMLS_CC));

    num_functions   = zend_hash_num_elements(CG(function_table)) - num_functions;
    num_classes     = zend_hash_num_elements(CG(class_table))    - num_classes;
   /*
	* The compile has succeeded so create the pool and allocated the entry block as this is
    * alway the first block allocated in a serial pool, then fill the entry block.
	*/
	pool = pool_create(LPC_SERIALPOOL);
	pool_alloc(entry, sizeof(lpc_entry_block_t));
	pool_strdup(entry->filename, h->filename);

	pool_alloc(entry->op_array, sizeof(zend_op_array));
	lpc_copy_op_array(entry->op_array, *op_array,  pool);

	entry->num_functions = num_functions;
	if (num_functions) {
		pool_alloc(entry->functions, sizeof(lpc_function_t) * num_functions);
	    my_copy_new_functions(entry->functions, num_functions, pool);
	} else {
		entry->functions = NULL;
	}

	entry->num_classes = num_classes;
	if (num_classes) {
		pool_alloc(entry->classes, sizeof(lpc_class_t) * num_classes);
///////// STATUS =
	    my_copy_new_classes(entry->classes, num_classes, pool);
	} else {
		entry->classes = NULL;
	}

	entry->halt_offset = my_file_halt_offset(h->filename TSRMLS_PC);

    lpc_debug("h->opened_path=[%s]  h->filename=[%s]" TSRMLS_CC, h->opened_path?h->opened_path:"null",h->filename);

	*pool_ptr = pool; 
    return SUCCESS;

error:
	/* The old compile file module will have raised any errors, so just ... */
    return FAILURE;
}
/* }}} */

/* {{{ copy functions */

/*
 * copy assistance macros
 */
#define ALLOC_IF_NULL(dst)  if (!(dst)) pool_alloc(dst,sizeof(*dst));
#define BITWISE_COPY(src,dst) assert(src != NULL); memcpy(dst, src, sizeof(*src));

/* {{{ my_copy_zval_ptr */
static zval** my_copy_zval_ptr(zval** pdst, const zval** psrc, lpc_pool* pool)
{ENTER(my_copy_zval_ptr)
	ALLOC_IF_NULL(pdst);
    pool_alloc_zval(*pdst);
    my_copy_zval(*pdst, *psrc, pool);
    return pdst;
}
/* }}} */

/* {{{ my_copy_zval */
static zval* my_copy_zval(zval* dst, const zval* src, lpc_pool* pool)
{ENTER(my_copy_zval)

    assert(dst != NULL);
	BITWISE_COPY(src,dst);

	/* The HashTable copied_zvals is used to detect and avoid circular references in the zvals */ 
    if (LPCGP(copied_zvals).nTableSize) {
	    zval **tmp;
        if(zend_hash_index_find(&LPCGP(copied_zvals), (ulong)src, (void**)&tmp) == SUCCESS) {
            if(Z_ISREF_P((zval*)src)) {
                Z_SET_ISREF_PP(tmp);
            }
            Z_ADDREF_PP(tmp);
            return *tmp;
        }
        zend_hash_index_update(&LPCGP(copied_zvals), (ulong)src, (void**)&dst, sizeof(zval*), NULL);
    }

    /* code uses refcount=2 for consts */
    Z_SET_REFCOUNT_P(dst, Z_REFCOUNT_P((zval*)src));
    Z_SET_ISREF_TO_P(dst, Z_ISREF_P((zval*)src));

    switch (Z_TYPE_P(src) & IS_CONSTANT_TYPE_MASK) {
    case IS_RESOURCE:
    case IS_BOOL:
    case IS_LONG:
    case IS_DOUBLE:
    case IS_NULL:
        break;

    case IS_CONSTANT:
    case IS_STRING:
        if (Z_STRVAL_P(src)) {
            pool_memcpy(Z_STRVAL_P(dst), Z_STRVAL_P(src), Z_STRLEN_P(src)+1);
        }
        break;

    case IS_ARRAY:
    case IS_CONSTANT_ARRAY:
		pool_alloc_ht(Z_ARRVAL_P(dst));
        my_copy_hashtable(Z_ARRVAL_P(dst), Z_ARRVAL_P(src),
                          (ht_copy_fun_t) my_copy_zval_ptr,
                          HOLDS_PTRS, pool, NULL);
        break;

    case IS_OBJECT: 
        dst->type = IS_NULL;
        break;

    default:
        assert(0);
    }

    return dst;
}
/* }}} */

/* {{{ lpc_copy_function */
zend_function* lpc_copy_function(zend_function* dst, zend_function* src, lpc_pool* pool)
{ENTER(lpc_copy_function)
    assert(src != NULL);
	ALLOC_IF_NULL(dst);
	BITWISE_COPY(src,dst);
	/*
     * The union zend_function is defined in zend_compile.h and the first group of fields is common 
     * to all. This includes a type selector with one of the following swithed values and the three
	 * pointers: function_name, (ce) scope, prototype and arg_info.  In the internal/overloaded
     * functions a shallow copy can take place; otherwise they are deep copied by the copy_op_array. 
	 */
    switch (src->type) {
    case ZEND_INTERNAL_FUNCTION:        
    case ZEND_OVERLOADED_FUNCTION:
        dst->op_array = src->op_array;
        break;

    case ZEND_USER_FUNCTION:
    case ZEND_EVAL_CODE:
        lpc_copy_op_array(&dst->op_array, &src->op_array, pool);
        break;

    default:
        assert(0);
    }
    /*
	 * If a method is flagged ZEND_ACC_IMPLEMENTED_ABSTRACT then it MUST have a prototype defined.
     * However, as zend_do_inheritance sets this property correctly, the flag can be cleared here
     * and the prototype nulled. 
	 */
    dst->common.fn_flags = src->common.fn_flags & (~ZEND_ACC_IMPLEMENTED_ABSTRACT);
    dst->common.prototype = NULL;

    return dst;
}
/* }}} */

/* {{{ my_copy_property_info */
static zend_property_info* my_copy_property_info(zend_property_info* dst, zend_property_info* src, lpc_pool* pool)
{ENTER(my_copy_property_info)
	ALLOC_IF_NULL(dst);
	BITWISE_COPY(src,dst);

   if (src->name) {
        /* private members are stored inside property_info as a mangled
         * string of the form:
         *      \0<classname>\0<membername>\0
         */
        pool_memcpy(dst->name, src->name, src->name_length+1);
   }

#if defined(ZEND_ENGINE_2) && PHP_MINOR_VERSION > 0
    if (src->doc_comment) {
        pool_memcpy(dst->doc_comment, src->doc_comment, src->doc_comment_len + 1);
    }
#endif

    return dst;
}
/* }}} */

/* {{{ my_copy_arg_info_array */
static void my_copy_arg_info_array(zend_arg_info** pdst, const zend_arg_info* src, uint num_args, lpc_pool* pool)
{ENTER(my_copy_arg_info_array)
    uint i = 0;
	zend_arg_info* dst;

	/* the arg info is allocated as a contigous block of num_arg x zend_arg_info entries */
    pool_memcpy(*pdst, src, sizeof(*src) * num_args);
	dst = *pdst;

    for(i=0; i < num_args; i++) {
		/* copy the name and class_name string if present */
		if (src[i].name) {
		    pool_memcpy(dst[i].name, src[i].name, src[i].name_len+1);
		}

		if (src[i].class_name) {
		    pool_memcpy(dst[i].class_name, src[i].class_name, src[i].class_name_len+1);
		}
    }
}
/* }}} */

/* {{{ lpc_copy_class_entry */
void lpc_copy_class_entry(zend_class_entry* dst, zend_class_entry* src, lpc_pool* pool)
/* Deep copying a zend_class_entry (see Zend/zend.h) involves various components:
 *  1)  scalars which need to be copies "bit copied"
 *  2)  scalars which shouldn't be inherited on copy
 *  3)  Embeded HashTables and pointers to structures which need to be deep copied
 *  4)  function pointers which need to be relocated.
 *
 * (1) is achieved by doing a base "bit copy" of the entire class entry; (2) by explicitly setting
 * to 0/NULL; (3) are the fields:
 *
 *  char                *name
 *  zend_class_entry    *parent
 *  zend_function_entry *builtin_functions
 *  zend_class_entry   **interfaces
 *  char                *filename
 *  char                *doc_comment
 *  zend_module_entry   *module
 * 
 * and the HahTables: function_table, default_properties, properties_info, default_static_members,
 * *static_members, constants_table
 * 
 *  (4) are the function pointers for : constructor, destructor, clone, __get, __set, __unset,
 *  __isset, __call, __callstatic, __tostring, serialize_func, unserialize_func, create_object,
 *  get_iterator, interface_gets_implemented, get_static_method, serialize, unserialize
 * 
 * Also not that some operations are only done once on copying out from exec to serial pool such as
 * adjusting the interfaces count.
 */
{ENTER(lpc_copy_class_entry)
    uint i = 0;
	int is_copyout = !is_exec_pool();
	TSRMLS_FETCH_FROM_POOL();

	BITWISE_COPY(src,dst);
   /*
    * Set all fields that shouldn't be inherited on copy to 0/NULL 
    */
    dst->name = NULL;
	memset(&dst->function_table, 0, sizeof(dst->function_table));
    memset(&dst->properties_info, 0, sizeof(dst->properties_info));
    memset(&dst->constants_table, 0, sizeof(dst->constants_table));
   /*
	* The interfaces are populated at runtime using ADD_INTERFACE 
	*/
   /* 
	* These will either be set inside my_fixup_hashtable or they will be copied
    * out from parent inside zend_do_inheritance. 
    */
    dst->parent = NULL;
    dst->constructor =  NULL;
    dst->destructor = NULL;
    dst->clone = NULL;
    dst->__get = NULL;
    dst->__set = NULL;
    dst->__unset = NULL;
    dst->__isset = NULL;
    dst->__call = NULL;
#ifdef ZEND_ENGINE_2_2
    dst->__tostring = NULL;
#endif
#ifdef ZEND_ENGINE_2_3
	dst->__callstatic = NULL;
#endif
   /*
	* Unset function proxies 
	*/
    dst->serialize_func = NULL;
    dst->unserialize_func = NULL;

    if (src->name) {
        pool_strdup(dst->name, src->name);
    }
	
 	if (is_copyout) {
	   /*
        * The compiler output count includes inherited interfaces as well, but the count
        * to be saved is count of first <n> real dynamic ones which were zero'd out in
        * zend_do_end_class_declaration
 		*/
		for(i = 0 ; (i < src->num_interfaces) && !src->interfaces[i]; i++) {}
		dst->num_interfaces = (i < src->num_interfaces) ? i : 0;
	    dst->interfaces = NULL;  
	} else {
		dst->num_interfaces = src->num_interfaces;
		pool_alloc(dst->interfaces, sizeof(zend_class_entry*) * src->num_interfaces);
        memset(dst->interfaces, 0,  sizeof(zend_class_entry*) * src->num_interfaces);
  
	} 
   /*
	* Copy and fixup the function table
    */
    my_copy_hashtable(&dst->function_table, &src->function_table,
                         (ht_copy_fun_t) lpc_copy_function,
                         NO_PTRS, pool,
                         (ht_check_copy_fun_t) my_check_copy_function, src);

    my_fixup_hashtable(&dst->function_table, (ht_fixup_fun_t)my_fixup_function, src, dst, pool);
   /*
	* Copy the default properties table. Unfortunately this is different for Zend 2.4 and previous versions 
    */
#ifdef ZEND_ENGINE_2_4
	dst->default_properties_count = src->default_properties_count;
    if (src->default_properties_count) {
        pool_alloc(dst->default_properties_table, 
		           sizeof(zval*) * src->default_properties_count);
        for (i = 0; i < src->default_properties_count; i++) {
            if (src->default_properties_table[i]) {
                my_copy_zval_ptr(&dst->default_properties_table[i], 
				                 (const zval**) &src->default_properties_table[i], pool);
				TAG_PTR(dst->default_properties_table[i]);
            } else {
                dst->default_properties_table[i] = NULL;
            }
        }
    } else {
        dst->default_properties_table = NULL;
    }
#else
    memset(&dst->default_properties, 0, sizeof(dst->default_properties));
    my_copy_hashtable(&dst->default_properties, &src->default_properties,
                      (ht_copy_fun_t) my_copy_zval_ptr,
                      HOLDS_PTRS, pool,
                      (ht_check_copy_fun_t) my_check_copy_default_property, src);
#endif
   /*
	* Copy the properties info table and fixup scope attribute (introduced in Zend 2.2) 
    */
    my_copy_hashtable(&dst->properties_info, &src->properties_info,
                      (ht_copy_fun_t) my_copy_property_info,
                      NO_PTRS, pool,
                      (ht_check_copy_fun_t) my_check_copy_property_info, src);

#ifdef ZEND_ENGINE_2_2
    my_fixup_hashtable(&dst->properties_info, 
	                   (ht_fixup_fun_t)my_fixup_property_info, 
	                   src, dst, pool);
#endif
   /*
	* Copy the default static members table. Again this was changed at Zend 2.4 
    */
#ifdef ZEND_ENGINE_2_4
    dst->default_static_members_count = src->default_static_members_count;

    if (src->default_static_members_count) {

        pool_alloc(dst->default_static_members_table, 
		           (sizeof(zval*) * src->default_static_members_count));

        for (i = 0; i < src->default_static_members_count; i++) {
            if (src->default_static_members_table[i]) {
                my_copy_zval_ptr(&dst->default_static_members_table[i], 
				                 (const zval**)&src->default_static_members_table[i], pool);
            } else {
                dst->default_static_members_table[i] = NULL;
            }
        }
    } else {
        dst->default_static_members_table = NULL;
    }
    TAG_SETPTR(dst->static_members_table, dst->default_static_members_table);
#else
    memset(&dst->default_static_members, 0, sizeof(dst->default_static_members));
    my_copy_hashtable(&dst->default_static_members, &src->default_static_members,
                         (ht_copy_fun_t) my_copy_zval_ptr,
                         HOLDS_PTRS, pool,
                         (ht_check_copy_fun_t) my_check_copy_static_member,
                         src, &src->default_static_members);

    if(src->static_members != &src->default_static_members) {
        pool_alloc_ht(dst->static_members); 
	    my_copy_hashtable(dst->static_members, src->static_members,
                          (ht_copy_fun_t) my_copy_zval_ptr,
                          HOLDS_PTRS, pool,
                          (ht_check_copy_fun_t) my_check_copy_static_member, src, src->static_members);
    } else {
        TAG_SETPTR(dst->static_members, &dst->default_static_members);
    }
#endif
   /*
	* Copy the constants table 
    */
    my_copy_hashtable(&dst->constants_table, &src->constants_table,
                         (ht_copy_fun_t) my_copy_zval_ptr,
                         HOLDS_PTRS, pool,
                         (ht_check_copy_fun_t) my_check_copy_constant, src);

	/* In the case of a user class dup the doc_comment if any */
    if (src->type == ZEND_USER_CLASS && ZEND_CE_DOC_COMMENT(src)) {
        pool_memcpy(ZEND_CE_DOC_COMMENT(dst),
		            ZEND_CE_DOC_COMMENT(src), (ZEND_CE_DOC_COMMENT_LEN(src) + 1));
    } else {
	    ZEND_CE_DOC_COMMENT(dst) = NULL;
	}

	/* For an internal class dup the null terminated list of builtin functions */ 
    if (src->type == ZEND_INTERNAL_CLASS && ZEND_CE_BUILTIN_FUNCTIONS(src)) {
        int n;

        for (n = 0; ZEND_CE_BUILTIN_FUNCTIONS(src)[n].fname != NULL; n++) {}

        pool_memcpy(ZEND_CE_BUILTIN_FUNCTIONS(dst), 
					ZEND_CE_BUILTIN_FUNCTIONS(src), ((n + 1) * sizeof(zend_function_entry)));

        for (i = 0; i < n; i++) {
			zend_function_entry *fsrc = (zend_function_entry *) &ZEND_CE_BUILTIN_FUNCTIONS(src)[i];
			zend_function_entry *fdst = (zend_function_entry *) &ZEND_CE_BUILTIN_FUNCTIONS(dst)[i];

			if (fsrc->fname) {
			   pool_strdup(fdst->fname, fsrc->fname);
			}

			if (fsrc->arg_info) {
				my_copy_arg_info_array((zend_arg_info **) &fdst->arg_info, fsrc->arg_info, fsrc->num_args, pool);
			}
        }
        *(char**)&(ZEND_CE_BUILTIN_FUNCTIONS(dst)[n].fname) = NULL;
    }

    if (src->type == ZEND_USER_CLASS && ZEND_CE_FILENAME(src)) {
        pool_strdup(ZEND_CE_FILENAME(dst), ZEND_CE_FILENAME(src));
    } else {
	    ZEND_CE_FILENAME(dst) = NULL;
	}
}
/* }}} */

/* {{{ my_copy_hashtable */
static LPC_HOTSPOT void my_copy_hashtable(HashTable* dst, HashTable* src,
                                          ht_copy_fun_t copy_fn,
                                          int holds_ptrs, lpc_pool* pool,
                                          ht_check_copy_fun_t check_fn, ...)
{ENTER(my_copy_hashtable)
    Bucket* curr = NULL;
    Bucket* prev = NULL;
    Bucket* newp = NULL;
    Bucket* oldp = NULL;
    int first = 1;
	TSRMLS_FETCH_FROM_POOL();

	ALLOC_IF_NULL(dst);
	BITWISE_COPY(src,dst);

    /* allocate and zero buckets for the new hashtable */
    pool_alloc(dst->arBuckets, dst->nTableSize * sizeof(Bucket*));
    memset(dst->arBuckets, 0, dst->nTableSize * sizeof(Bucket*));

    dst->pInternalPointer = NULL;
    dst->pListHead = NULL;

    for (curr = src->pListHead; curr != NULL; curr = curr->pListNext) {
        int n = curr->h % dst->nTableSize;

        if(check_fn) {
		   /*
			* If defined, call the check_fn to see if the current bucket needs to be copied out.
			* A return of CHECK_SKIP_ELT triggers skipping the rest of the processing for this bucket.
			*/

			check_t check_fn_rtn;
            va_list args;
            va_start(args, check_fn);
			check_fn_rtn = check_fn(curr, args);
			va_end(args);
           
            if(check_fn_rtn == CHECK_SKIP_ELT) {
                dst->nNumOfElements--;   
                continue;
            }
       }

        /* create a copy of the bucket 'curr' */
		oldp = dst->arBuckets[n];
#ifdef ZEND_ENGINE_2_4
/////////////////// TODO: this doesn't make sense in the context of a SERIALPOOL -- review once Zend 2.4 is tested. Also add dst->arBuckets[n] ref to alloc. 
        if (!curr->nKeyLength || IS_INTERNED(curr->arKey) {
            pool_memcpy(newp, curr, sizeof(Bucket));
        } else if (pool->type != LPC_EXECPOOL) {
            char *arKey = lpc_new_interned_string(curr->arKey, curr->nKeyLength TSRMLS_CC);

            if (!arKey) {
                pool_memcpy(newp, curr, (sizeof(Bucket) + curr->nKeyLength));
                newp->arKey = ((char*)newp) + sizeof(Bucket);
            } else {
                pool_memcpy(newp, curr, sizeof(Bucket));
                newp->arKey = arKey;
            }
        } else {
            pool_memcpy(newp, curr, (sizeof(Bucket) + curr->nKeyLength));
            newp->arKey = ((char*)newp) + sizeof(Bucket);
        }        
#else
        pool_memcpy(dst->arBuckets[n], curr, sizeof(Bucket) + curr->nKeyLength - 1);
#endif
		newp = dst->arBuckets[n];
        /* insert 'newp' into the linked list at its hashed index */
        if (oldp) {
            newp->pNext = oldp;
            newp->pLast = NULL;
            newp->pNext->pLast = newp;
        }
        else {
            newp->pNext = newp->pLast = NULL;
        }

        /* copy the bucket data using our 'copy_fn' callback function */
        newp->pData = copy_fn(NULL, curr->pData, pool);

        if (holds_ptrs) {
            memcpy(&newp->pDataPtr, newp->pData, sizeof(void*));
        }
        else {
            newp->pDataPtr = NULL;
        }

        /* insert 'newp' into the table-thread linked list */
        newp->pListLast = prev;
        newp->pListNext = NULL;

        if (prev) {
            prev->pListNext = newp;
        }

        if (first) {
            dst->pListHead = newp;
            first = 0;
        }

        prev = newp;
    }

    dst->pListTail = newp;
   /*
	* In the case of a Serial Pool HashTable, all pointers will typically be internal to the pool 
	* and therefore tagged as potentially relocatable.  It's just easier to do this scan on all
    * the linked lists in the buckets, plus head and tails, plus buck pointers as a separate pass.
	*/
	if (!is_exec_pool()) {
		uint n;
		TAG_PTR(dst->pListHead);
		TAG_PTR(dst->pListTail);
		for (n = 0; n < dst->nTableSize; n++) {
			TAG_PTR(dst->arBuckets[n]);
		}
	    for (curr = dst->pListHead; curr != NULL; curr = curr->pListNext) {
			TAG_PTR(curr->pData);
			TAG_PTR(curr->pDataPtr);
			TAG_PTR(curr->pListNext);
			TAG_PTR(curr->pListLast);
			TAG_PTR(curr->pNext);
			TAG_PTR(curr->pLast);
		}
	}
}
/* }}} */

/* {{{ lpc_copy_op_array */
void lpc_copy_op_array(zend_op_array* dst, zend_op_array* src, lpc_pool* pool)
{ENTER(lpc_copy_op_array)
    int i;
    lpc_fileinfo_t *fileinfo = NULL;
    char canon_path[MAXPATHLEN];
    char *fullpath = NULL;
    lpc_opflags_t *flags;
	TSRMLS_FETCH_FROM_POOL();

    assert(src != NULL);

    /* start with a bitwise copy of the array */
    memcpy(dst, src, sizeof(*src));

    dst->function_name = NULL;
  /*dst->filename = NULL -- nope: my_compile_file() substituted the handle filename so this field can be bit copied */
    dst->refcount = NULL;
    dst->opcodes = NULL;
    dst->brk_cont_array = NULL;
    dst->static_variables = NULL;
    dst->try_catch_array = NULL;
    dst->arg_info = NULL;
    dst->doc_comment = NULL;
#ifdef ZEND_ENGINE_2_1
    dst->vars = NULL;
#endif

    /* copy the arg types array (if set) */
    if (src->arg_info) {
        my_copy_arg_info_array(&dst->arg_info,
                               src->arg_info,
                               src->num_args,
                               pool);
    }
    if (src->function_name) {
        pool_strdup(dst->function_name, src->function_name);
    }

    pool_memcpy(dst->refcount, src->refcount, sizeof(*(src->refcount)));

#ifdef ZEND_ENGINE_2_4
    if (src->literals) {
        zend_literal *p, *q, *end;

        q = src->literals;
        p = dst->literals = (zend_literal*) pool_alloc((sizeof(zend_literal) * src->last_literal));
        end = p + src->last_literal;
        while (p < end) {
            *p = *q;
            my_copy_zval(&p->constant, &q->constant, pool);
			TAG_PTR(p->constant);
            p++;
            q++;
        }
    }
#endif

   /*
	* Memcpy the opcodes into a dst version.  The following code will overwrite any pointers, etc.
	*/
    pool_memcpy(dst->opcodes, src->opcodes, sizeof(zend_op) * src->last);

   /*
    * The LPC flags field is mapped onto a size_t entry in the reserved array. lpc_zend_init()
    * calls zend_get_resource_handle() to allocate this offset and store it in the variable
    * lpc_reserved_offset. This is the same offset for all LPC threads so this can be and is
    * a true global.
 	*/
	dst->reserved[lpc_reserved_offset] = 0;
    flags = (lpc_opflags_t*) &dst->reserved[lpc_reserved_offset];

    for (i = 0; (uint) i < src->last; i++) {
        zend_op *src_zo = &src->opcodes[i];
        zend_op *dst_zo = &dst->opcodes[i];
	   /*
		* Whereas the changes introduced in 2.1 to 2.3 Zend engines were mostly additional 
		* attributes, the zend_op structure changed significantly at 2.4.  The following 
        * macro variants for 2.4 and pre 2.4 encapsulate these changes to permit the use of 
		* a common code base to handle most of the zend_op processing.
		*/
#ifdef ZEND_ENGINE_2_4
//////////  TODO: These 2.4 macros haven't been debugged yet
#  define CHECK_OPTYPE(ot) (ot == IS_CONST || ot == IS_VAR || ot == IS_CV || ot == IS_TMP_VAR || ot == IS_UNUSED)
#  define COPY_ZNODE_IF_CONSTANT(dzo,szo,fld) if (szo->fld ## _type == IS_CONST) \
    dzo->fld.literal = szo->fld.literal - src->literals + dst->literals;
#  define CONST_PZV(zo_op) zo_op.zv
#  define ZO_EV_IS_FETCH_GLOBAL(zo) ((zo->extended_value & ZEND_FETCH_TYPE_MASK) == ZEND_FETCH_GLOBAL)
#  define ZOP_TYPE_IS_CONSTANT_STRING(zo_op) \
	(zo_op ## _type == IS_CONST) && (Z_TYPE_P(zo_op.zv) == IS_STRING)
#  define IS_AUTOGLOBAL_SETFLAG(member) (!strcmp(Z_STRVAL_P(zo->op1.zv), #member)) {flags->member = 1;}
#  define ZOP_TYPE_IS_CONSTANT_ARRAY(zo_op) \
    (zo_op ## _type == IS_CONST && Z_TYPE_P(zo_op.zv) == IS_CONSTANT_ARRAY)
#  define JUMP_ADDR(op) op.jmp_addr
#else
#  ifdef IS_CV
#    define CHECK_OPTYPE(ot) (ot == IS_CONST || ot == IS_VAR || ot == IS_CV || ot == IS_TMP_VAR || ot == IS_UNUSED)
#  else
#    define CHECK_OPTYPE(ot) (ot == IS_CONST || ot == IS_VAR || ot == IS_TMP_VAR || ot == IS_UNUSED)
#  endif
#  define COPY_ZNODE_IF_CONSTANT(dzo,szo,fld) if (szo->fld.op_type == IS_CONST) \
	{ my_copy_zval(&dzo->fld.u.constant, &szo->fld.u.constant, pool); }
#  define CONST_PZV(zo_op) &(zo_op.u.constant)
#  define ZO_EV_IS_FETCH_GLOBAL(zo) (zo->op2.u.EA.type == ZEND_FETCH_GLOBAL)
#  define ZOP_TYPE_IS_CONSTANT_STRING(zo_op) \
	(zo_op.op_type == IS_CONST) && (Z_TYPE(zo_op.u.constant) == IS_STRING)
#  define IS_AUTOGLOBAL_SETFLAG(member) (!strcmp(Z_STRVAL_P(const_pzv), #member)) {flags->member = 1;}
#  define ZOP_TYPE_IS_CONSTANT_ARRAY(zo_op) \
	(zo_op.op_type == IS_CONST && Z_TYPE(zo_op.u.constant) == IS_CONSTANT_ARRAY)
#  define JUMP_ADDR(op) op.u.jmp_addr
#endif

	   /*
        * The only type of pointer-based znodes in the op_array are constants.  These need to be
		* deep-copied.
		*/ 
#ifdef ZEND_ENGINE_2_4
		assert(CHECK_OPTYPE(dst_zo->result_type) && dst_zo->result_type != IS_CONST &&
		       CHECK_OPTYPE(dst_zo->op1_type) &&
		       CHECK_OPTYPE(dst_zo->op2_type));
#endif
		COPY_ZNODE_IF_CONSTANT(dst_zo,src_zo,result);
		COPY_ZNODE_IF_CONSTANT(dst_zo,src_zo,op1);
		COPY_ZNODE_IF_CONSTANT(dst_zo,src_zo,op2);
       /*
		* Use a switch to decode the opcode and decide which of the LPC opflags apply to this op_array copy
		*/
        switch (src_zo->opcode) {

#ifdef ZEND_ENGINE_2_3
            case ZEND_GOTO:
#endif
            case ZEND_JMP:
            case ZEND_JMPZ:
            case ZEND_JMPNZ:
            case ZEND_JMPZ_EX:
            case ZEND_JMPNZ_EX:
#ifdef ZEND_ENGINE_2_3
            case ZEND_JMP_SET:
#endif
                if(flags != NULL) {
                    flags->has_jumps = 1;
                }
                break;
            /* auto_globals_jit was not in php-4.3.* */
            case ZEND_FETCH_R:
            case ZEND_FETCH_W:
            case ZEND_FETCH_IS:
            case ZEND_FETCH_FUNC_ARG:
            case ZEND_FETCH_RW:
            case ZEND_FETCH_UNSET:
                if(PG(auto_globals_jit) && flags != NULL) {
                     /* The fetch is only required if auto_globals_jit=1  */
                    if (ZO_EV_IS_FETCH_GLOBAL(src_zo) &&
                        ZOP_TYPE_IS_CONSTANT_STRING(src_zo->op1)) {
						zval *const_pzv = CONST_PZV(src_zo->op1);
                        if (Z_STRVAL_P(const_pzv)[0] == '_') {
							if IS_AUTOGLOBAL_SETFLAG(_GET)
							else if IS_AUTOGLOBAL_SETFLAG(_POST)
							else if IS_AUTOGLOBAL_SETFLAG(_COOKIE)
							else if IS_AUTOGLOBAL_SETFLAG(_SERVER)
							else if IS_AUTOGLOBAL_SETFLAG(_ENV)
							else if IS_AUTOGLOBAL_SETFLAG(_FILES)
							else if IS_AUTOGLOBAL_SETFLAG(_REQUEST)
							else if IS_AUTOGLOBAL_SETFLAG(_SESSION) 
						} else if(zend_is_auto_global( Z_STRVAL_P(const_pzv), Z_STRLEN_P(const_pzv) TSRMLS_CC)){
                            flags->unknown_global = 1;
                        }
#ifdef ZEND_ENGINE_2_4
                        else if  IS_AUTOGLOBAL_SETFLAG(GLOBALS) {
                            flags->unknown_global = 1;
                        }
#endif
                    }
                }
                break;

            case ZEND_RECV_INIT:
                if (ZOP_TYPE_IS_CONSTANT_ARRAY(src_zo->op2) && flags != NULL) {
                    flags->deep_copy = 1;
                }
                break;

            default:
                if (( ZOP_TYPE_IS_CONSTANT_ARRAY(src_zo->op1) || 
					  ZOP_TYPE_IS_CONSTANT_ARRAY(src_zo->op2) ) && 
                    flags != NULL) {
                     flags->deep_copy = 1;
                }
                break;
        }

//////////// TODO: Review the treatment of relative path files on the include path.  Do we convert to absolute?
//////////// TODO: For now disable this code path and revisit later.

        /* This code breaks lpc's rule#1 - cache what you compile */
        if (/*DEBUG*/ 0 && /*DEBUG*/ (LPCG(fpstat)==0) && LPCG(canonicalize)) {
			/* not pool allocated, because it's temporary */
            fileinfo = (lpc_fileinfo_t*) emalloc(sizeof(lpc_fileinfo_t));

            if ((src_zo->opcode == ZEND_INCLUDE_OR_EVAL) && ZOP_TYPE_IS_CONSTANT_STRING(src_zo->op1)) {
                /* constant includes */
                if(!IS_ABSOLUTE_PATH(Z_STRVAL_P(CONST_PZV(src_zo->op1)),Z_STRLEN_P(CONST_PZV(src_zo->op1)))) { 
                    if (lpc_search_paths(Z_STRVAL_P(CONST_PZV(src_zo->op1)), PG(include_path), fileinfo TSRMLS_CC) == 0) {
                        if((fullpath = realpath(fileinfo->fullpath, canon_path))) {
                            /* everything has to go through a realpath() */
                            zend_op *dzo;
							TAG_SETPTR(dzo, &(dst->opcodes[i]));
#ifdef ZEND_ENGINE_2_4
                            pool_alloc(dzo->op1.literal, sizeof(zend_literal));
                            Z_STRLEN_P(dzo->op1.zv) = strlen(fullpath);
                            pool_memcpy(Z_STRVAL_P(dzo->op1.zv), fullpath, Z_STRLEN_P(dzo->op1.zv) + 1);
                            Z_SET_REFCOUNT_P(dzo->op1.zv, 2);
                            Z_SET_ISREF_P(dzo->op1.zv);
                            dzo->op1.literal->hash_value = zend_hash_func(Z_STRVAL_P(dzo->op1.zv), Z_STRLEN_P(dzo->op1.zv) + 1);
#else
                            Z_STRLEN(dzo->op1.u.constant) = strlen(fullpath);
                            pool_memcpy(Z_STRVAL(dzo->op1.u.constant), 
							            fullpath, Z_STRLEN(dzo->op1.u.constant) + 1);
#endif
                        }
                    }
                }
            }
            efree(fileinfo);
        }
    } /* end for each opline */ 

    if(flags->has_jumps) {
		/* Relocate the Jump targets */
		for (i=0; i < dst->last; ++i) {
		    zend_op *src_zo = &src->opcodes[i];
		    zend_op *dst_zo = &dst->opcodes[i];
		    /*convert opline number to jump address*/
		    switch (dst_zo->opcode) {
#ifdef ZEND_ENGINE_2_3
		        case ZEND_GOTO:
#endif
		        case ZEND_JMP:                      /*  (zend_op *) + (        number of zend_ops           )*/    
		            TAG_SETPTR(JUMP_ADDR(dst_zo->op1), dst->opcodes + (JUMP_ADDR(src_zo->op1) - src->opcodes));
		            break;

		        case ZEND_JMPZ:
		        case ZEND_JMPNZ:
		        case ZEND_JMPZ_EX:
		        case ZEND_JMPNZ_EX:
#ifdef ZEND_ENGINE_2_3
		        case ZEND_JMP_SET:
#endif
		            TAG_SETPTR(JUMP_ADDR(dst_zo->op2), dst->opcodes + (JUMP_ADDR(src_zo->op2) - src->opcodes));
		            break;

		        default:
		            break;
		    }
		}
	}

    /* copy the break-continue array */
    if (src->brk_cont_array) {
        pool_memcpy(dst->brk_cont_array, src->brk_cont_array, 
		            sizeof(src->brk_cont_array[0]) * src->last_brk_cont);
    }

    /* copy the table of static variables */
    if (src->static_variables) {
		pool_alloc_ht(dst->static_variables);
		my_copy_hashtable(dst->static_variables, src->static_variables,
                          (ht_copy_fun_t) my_copy_zval_ptr,
                          HOLDS_PTRS, pool, NULL);
    }

    if (src->try_catch_array) {
        pool_memcpy(dst->try_catch_array, src->try_catch_array, 
		            sizeof(src->try_catch_array[0]) * src->last_try_catch);
    }

#ifdef ZEND_ENGINE_2_1 /* PHP 5.1 */
    if (src->vars) {
        pool_memcpy(dst->vars, src->vars, sizeof(src->vars[0]) * src->last_var);

        for(i = 0; i <  src->last_var; i++) dst->vars[i].name = NULL;

        for(i = 0; i <  src->last_var; i++) {
            pool_memcpy(dst->vars[i].name, src->vars[i].name, src->vars[i].name_len + 1);
        }
    }
#endif

    if (src->doc_comment) {
        pool_memcpy(dst->doc_comment, src->doc_comment, src->doc_comment_len + 1);
    }
}
/* }}} */

/* {{{ my_copy_new_functions 
	Deep copy the last set of functions added during the last compile from the CG(function_table) */
void my_copy_new_functions(lpc_function_t* dst_array, int count, lpc_pool* pool)
{ENTER(my_copy_new_functions)
    int i;
	TSRMLS_FETCH_FROM_POOL();

    /* count back count-1 functions from the end of the function table */
    zend_hash_internal_pointer_end(CG(function_table));
    for (i = 1; i < count; i++) {
        zend_hash_move_backwards(CG(function_table));
    }

    /* Add the next <count> functions to our dst_array */
    for (i = 0; i < count; i++, zend_hash_move_forward(CG(function_table))) {
        char* key;
        uint key_length;
        zend_function* fun;

        zend_hash_get_current_key_ex(CG(function_table), &key, &key_length, NULL, 0, NULL);
        zend_hash_get_current_data(CG(function_table), (void**) &fun);

        pool_memcpy(dst_array[i].name, key, (int) key_length);
        dst_array[i].name_len = (int) key_length-1;

		pool_alloc(dst_array[i].function, sizeof(zend_function));
        lpc_copy_function(dst_array[i].function, fun, pool);
    }																												
}
/* }}} */

/* {{{ my_copy_new_classes
	Deep copy the last set of classes added during the last compile from the CG(function_table) */
void my_copy_new_classes(lpc_class_t* cl_array, int count, lpc_pool* pool)
{ENTER(my_copy_new_classes)
    int i;
	TSRMLS_FETCH_FROM_POOL();

    /* count back count-1 functions from the end of the class table */
    zend_hash_internal_pointer_end(CG(class_table));
    for (i = 1; i < count; i++) {
        zend_hash_move_backwards(CG(class_table));
    }

    /* Add the next <count> classes to our cl_array */
    for (i = 0; i < count; i++, zend_hash_move_forward(CG(class_table))) {
        char* key;
        uint key_length;
        zend_class_entry *class, **class_ptr;
		lpc_class_t *cle = &cl_array[i];

        zend_hash_get_current_key_ex(CG(class_table), &key, &key_length, NULL, 0, NULL);
        zend_hash_get_current_data(CG(class_table), (void**) &class_ptr);
		class= *class_ptr;

        pool_memcpy(cle->name, key, (int) key_length);
        cle->name_len = (int) key_length-1;

		pool_alloc(cle->class_entry, sizeof(zend_class_entry));
        lpc_copy_class_entry(cle->class_entry, class, pool);

        /*
         * If the class has a pointer to its parent class, save the parent
         * name so that we can enable compile-time inheritance when we reload
         * the child class; otherwise, set the parent name to null
         */

        if (class->parent) {
            pool_strdup(cle->parent_name, class->parent->name);
        }
        else {
            cle->parent_name = NULL;
/////// TODO: resolve the (unimplemented) assertion in APC that the op_array should be scanned in the null case  to determine if this class inherits from some base class at execution-time.  As far as I can see, this is addressed in zend_do_inheritance
        }
    }
}
/* }}} */


/* {{{ my_file_halt_offset */
long my_file_halt_offset(const char *filename TSRMLS_DC)
{ENTER(my_file_halt_offset)
    zend_constant *c;
    char *name;
	uint len;
	char haltoff[] = "__COMPILER_HALT_OFFSET__";
    long value = -1;

    zend_mangle_property_name(&name, &len, haltoff, sizeof(haltoff) - 1, filename, strlen(filename), 0);
    
	if (zend_hash_find(EG(zend_constants), name, len+1, (void **) &c) == SUCCESS) {
        value = Z_LVAL(c->value);
    }
    
    pefree(name, 0);

    return value;
}
/* }}} */

/* {{{ lpc_do_halt_compiler_register */
void lpc_do_halt_compiler_register(const char *filename, long halt_offset TSRMLS_DC)
{ENTER(lpc_do_halt_compiler_register)
    char *name;
    char haltoff[] = "__COMPILER_HALT_OFFSET__";
    int len;
   
    if(halt_offset > 0) {
        
        zend_mangle_property_name(&name, &len, haltoff, sizeof(haltoff) - 1, 
                                  filename, strlen(filename), 0);
        
        zend_register_long_constant(name, len+1, halt_offset, CONST_CS, 0 TSRMLS_CC);

        pefree(name, 0);
    }
}
/* }}} */

/* {{{ my_fixup_function */
static void my_fixup_function(Bucket *p, zend_class_entry *src, zend_class_entry *dst, lpc_pool* pool)
{ENTER(my_fixup_function)
    zend_function* zf = p->pData;

    #define SET_IF_SAME_NAME(member) \
        if(src->member && !strcmp(zf->common.function_name, src->member->common.function_name)) { \
            TAG_SETPTR(dst->member, zf); \
		}

    if(zf->common.scope == src)
    {

        /* Fixing up the default functions for objects here since
         * we need to compare with the newly allocated functions
         *
         * caveat: a sub-class method can have the same name as the
         * parent's constructor and create problems.
         */

        if(zf->common.fn_flags & ZEND_ACC_CTOR) {TAG_SETPTR(dst->constructor, zf);}
		else if(zf->common.fn_flags & ZEND_ACC_DTOR) {TAG_SETPTR(dst->destructor, zf);}
		else if(zf->common.fn_flags & ZEND_ACC_CLONE) {TAG_SETPTR(dst->clone, zf);}
        else
        {
            SET_IF_SAME_NAME(__get);
            SET_IF_SAME_NAME(__set);
            SET_IF_SAME_NAME(__unset);
            SET_IF_SAME_NAME(__isset);
            SET_IF_SAME_NAME(__call);
#ifdef ZEND_ENGINE_2_2
            SET_IF_SAME_NAME(__tostring);
#endif
#ifdef ZEND_ENGINE_2_3
            SET_IF_SAME_NAME(__callstatic);
#endif
        }
        TAG_SETPTR(zf->common.scope, dst);
    }
    else
    {
        /* no other function should reach here */
        assert(0);
    }

    #undef SET_IF_SAME_NAME
}
/* }}} */

#ifdef ZEND_ENGINE_2_2
/* {{{ my_fixup_property_info */
static void my_fixup_property_info(Bucket *p, zend_class_entry *src, zend_class_entry *dst, lpc_pool* pool)
{ENTER(my_fixup_property_info)
    assert(((zend_property_info*)p->pData)->ce == src);
    TAG_SETPTR(((zend_property_info*)p->pData)->ce, dst);
}
/* }}} */
#endif

/* {{{ my_fixup_hashtable */
static void my_fixup_hashtable(HashTable *ht, ht_fixup_fun_t fixup, zend_class_entry *src, zend_class_entry *dst, lpc_pool *pool)
{ENTER(my_fixup_hashtable)
    Bucket *p;
    uint i;

    for (i = 0; i < ht->nTableSize; i++) {
        if(!ht->arBuckets) break;
        p = ht->arBuckets[i];
        while (p != NULL) {
            fixup(p, src, dst, pool);
            p = p->pNext;
        }
    }
}
/* }}} */

/* {{{ my_check_* filter functions used as callback from my_copy_hashtable */

/* {{{ my_check_copy_function
       returns ACCEPT if the function is in the scope of the class being copied */
static check_t my_check_copy_function(Bucket* p, va_list args)
{ENTER(my_check_copy_function)
    zend_class_entry* src = va_arg(args, zend_class_entry*);
    zend_function* zf = (zend_function*)p->pData;

    return  (zf->common.scope == src) ? CHECK_ACCEPT_ELT : CHECK_SKIP_ELT;
}
/* }}} */

#ifndef ZEND_ENGINE_2_4
/* {{{ my_check_copy_default_property 
       returns ACCEPT if the property is not inherited from its parent class */
static check_t my_check_copy_default_property(Bucket* p, va_list args)
{ENTER(my_check_copy_default_property)
    zend_class_entry* src = va_arg(args, zend_class_entry*);
    zend_class_entry* parent = src->parent;
    zval ** child_prop = (zval**)p->pData;
    zval ** parent_prop = NULL;

    if (parent && zend_hash_quick_find(&parent->default_properties, p->arKey, 
                                       p->nKeyLength, p->h, (void **) &parent_prop)==SUCCESS &&
		(parent_prop && child_prop) && (*parent_prop) == (*child_prop)) {
        return CHECK_SKIP_ELT;
    }

    /* possibly not in the parent */
    return CHECK_ACCEPT_ELT;
}
/* }}} */
#endif

/* {{{ my_check_copy_property_info */
static check_t my_check_copy_property_info(Bucket* p, va_list args)
{ENTER(my_check_copy_property_info)
    zend_class_entry* src = va_arg(args, zend_class_entry*);
    zend_class_entry* parent = src->parent;
    zend_property_info* child_info = (zend_property_info*)p->pData;
    zend_property_info* parent_info = NULL;

#ifdef ZEND_ENGINE_2_2
    /* so much easier */
    return (child_info->ce == src);
#endif

    if (parent &&
        zend_hash_quick_find(&parent->properties_info, p->arKey, p->nKeyLength,
            p->h, (void **) &parent_info)==SUCCESS) {
        if(parent_info->flags & ZEND_ACC_PRIVATE)
        {
            return CHECK_ACCEPT_ELT;
        }
        if((parent_info->flags & ZEND_ACC_PPP_MASK) !=
            (child_info->flags & ZEND_ACC_PPP_MASK))
        {
            /* TODO: figure out whether ACC_CHANGED is more appropriate here */
            return CHECK_ACCEPT_ELT;
        }
        return  CHECK_SKIP_ELT;
    }

    /* property doesn't exist in parent, copy into cached child */
    return CHECK_ACCEPT_ELT;
}
/* }}} */

#ifndef ZEND_ENGINE_2_4
/* {{{ my_check_copy_static_member */
static check_t my_check_copy_static_member(Bucket* p, va_list args)
{ENTER(my_check_copy_static_member)
    zend_class_entry* src = va_arg(args, zend_class_entry*);
    HashTable * ht = va_arg(args, HashTable*);
    zend_class_entry* parent = src->parent;
    HashTable * parent_ht = NULL;
    char * member_name;
    char * class_name = NULL;

    zend_property_info *parent_info = NULL;
    zend_property_info *child_info = NULL;
    zval ** parent_prop = NULL;
    zval ** child_prop = (zval**)(p->pData);

    if(!parent) {
        return CHECK_ACCEPT_ELT;
    }

    /* these do not need free'ing */
#ifdef ZEND_ENGINE_2_2
    zend_unmangle_property_name(p->arKey, p->nKeyLength-1, &class_name, &member_name);
#else
    zend_unmangle_property_name(p->arKey, &class_name, &member_name);
#endif

    /* please refer do_inherit_property_access_check in zend_compile.c
     * to understand why we lookup in properties_info.
     */
    if ((zend_hash_find(&parent->properties_info, member_name,
                        strlen(member_name)+1, (void**)&parent_info) == SUCCESS) &&
        (zend_hash_find(&src->properties_info, member_name,
                        strlen(member_name)+1, (void**)&child_info) == SUCCESS)) {

        if (ht == &(src->default_static_members)) {
            parent_ht = &parent->default_static_members;
        } else {
            parent_ht = parent->static_members;
        }

        if (zend_hash_quick_find(parent_ht, p->arKey,
                                 p->nKeyLength, p->h, (void**)&parent_prop) == SUCCESS) {
            /* they point to the same zval */
            if(*parent_prop == *child_prop) {
                return CHECK_SKIP_ELT;
            }
        }
    }

    return CHECK_ACCEPT_ELT;
}
/* }}} */
#endif

/* {{{ my_check_copy_constant */
static check_t my_check_copy_constant(Bucket* p, va_list args)
{ENTER(my_check_copy_constant)
    zend_class_entry* src = va_arg(args, zend_class_entry*);
    zend_class_entry* parent = src->parent;
    zval ** child_const = (zval**)p->pData;
    zval ** parent_const = NULL;

    if (parent &&
        zend_hash_quick_find(&parent->constants_table, p->arKey, 
                             p->nKeyLength, p->h, (void **) &parent_const)==SUCCESS) {
        if((parent_const && child_const) && (*parent_const) == (*child_const))
        {
            return  CHECK_SKIP_ELT;
        }
    }

    /* possibly not in the parent */
    return CHECK_ACCEPT_ELT;
}
/* }}} */
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */

