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
   and revised in 2005 by Yahoo! Inc. to 9add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.
*/

#include "lpc.h"
#include "lpc_copy_class.h"
#include "lpc_copy_function.h"
#include "lpc_copy_op_array.h"
#include "lpc_hashtable.h"
#include "php_lpc.h"
#include "lpc_string.h"
#include "Zend/zend_compile.h"

/*
 * These functions are filter functions used in lpc_copy_class_entry as lpc_copy_hashtable callbacks to 
 * determine whether each element within the HashTable should be copied or if it is already defined /
 *  overridden in the 'current' class entry and therefore not to be inherited.  
 */
static lpc_check_t check_copy_default_property(Bucket* p, const void *, const void *);
static lpc_check_t check_copy_property_info(Bucket* src, const void *, const void *);
static lpc_check_t check_copy_static_member(Bucket* src, const void *, const void *);
static lpc_check_t check_copy_constant(Bucket* src, const void *, const void *);
static lpc_check_t check_copy_method(Bucket* p, const void *, const void *);
static void        fixup_method(Bucket *p, zend_class_entry *src, 
                                zend_class_entry *dst, lpc_pool* pool);
#ifdef ZEND_ENGINE_2_2
static void        fixup_property_info(Bucket *p, zend_class_entry *src, 
                                       zend_class_entry *dst, lpc_pool* pool);
#endif
void copy_property_info(zend_property_info* dst, zend_property_info* src, lpc_pool* pool);

#define TAG_SETPTR(dst,src) dst = src; pool_tag_ptr(dst);

#ifdef ZEND_ENGINE_2_4
# define CE_FILENAME(ce)			(ce)->info.user.filename
# define CE_DOC_COMMENT(ce)        (ce)->info.user.doc_comment
# define CE_DOC_COMMENT_LEN(ce)	(ce)->info.user.doc_comment_len
# define CE_BUILTIN_FUNCTIONS(ce)  (ce)->info.internal.builtin_functions
#else
# define CE_FILENAME(ce)			(ce)->filename
# define CE_DOC_COMMENT(ce)        (ce)->doc_comment
# define CE_DOC_COMMENT_LEN(ce)	(ce)->doc_comment_len
# define CE_BUILTIN_FUNCTIONS(ce)  (ce)->builtin_functions
#endif

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
   /*-
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
	* These will either be set inside lpc_fixup_hashtable or they will be copied
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
    COPY_HT(function_table, lpc_copy_function, zend_function, check_copy_method, NULL);
 
    lpc_fixup_hashtable(&dst->function_table, (lpc_ht_fixup_fun_t)fixup_method, src, dst, pool);
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
                lpc_copy_zval_ptr(&dst->default_properties_table[i], 
				                  (const zval**) &src->default_properties_table[i], pool);
				pool_tag_ptr(dst->default_properties_table[i]);
            } else {
                dst->default_properties_table[i] = NULL;
            }
        }
    } else {
        dst->default_properties_table = NULL;
    }
#else
    COPY_HT(default_properties, lpc_copy_zval_ptr, zval *, check_copy_default_property, NULL);
#endif

   /*
	* Copy the properties info table and fixup scope attribute (introduced in Zend 2.2) 
    */
    COPY_HT(properties_info, copy_property_info, zend_property_info, check_copy_property_info, NULL);
#ifdef ZEND_ENGINE_2_2
    lpc_fixup_hashtable(&dst->properties_info, 
	                   (lpc_ht_fixup_fun_t)fixup_property_info, 
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
               lpc_copy_zval_ptr(&dst->default_static_members_table[i], 
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
    COPY_HT(default_static_members, lpc_copy_zval_ptr, zval *, 
            check_copy_static_member, &src->default_static_members);

    if (src->static_members != &src->default_static_members) {
        pool_alloc_ht(dst->static_members); 
	    COPY_HT_P(static_members, lpc_copy_zval_ptr, zval *,
                  check_copy_static_member, src->static_members);
    } else {
        dst->static_members = &dst->default_static_members;
        pool_tag_ptr(dst->static_members);
    }
#endif
   /*
	* Copy the constants table 
    */
    COPY_HT(constants_table, lpc_copy_zval_ptr, zval *, check_copy_constant, NULL);

	/* In the case of a user class dup the doc_comment if any */
    if (src->type == ZEND_USER_CLASS && CE_DOC_COMMENT(src)) {
        pool_memcpy(CE_DOC_COMMENT(dst),
		            CE_DOC_COMMENT(src), (CE_DOC_COMMENT_LEN(src) + 1));
    } else {
	    CE_DOC_COMMENT(dst) = NULL;
	}

	/* For an internal class dup the null terminated list of builtin functions */ 
    if (src->type == ZEND_INTERNAL_CLASS && CE_BUILTIN_FUNCTIONS(src)) {
        int n;

        for (n = 0; CE_BUILTIN_FUNCTIONS(src)[n].fname != NULL; n++) {}

        pool_memcpy(CE_BUILTIN_FUNCTIONS(dst), 
					CE_BUILTIN_FUNCTIONS(src), ((n + 1) * sizeof(zend_function_entry)));

        for (i = 0; i < n; i++) {
			zend_function_entry *fsrc = (zend_function_entry *) &CE_BUILTIN_FUNCTIONS(src)[i];
			zend_function_entry *fdst = (zend_function_entry *) &CE_BUILTIN_FUNCTIONS(dst)[i];

			if (fsrc->fname) {
			   pool_strdup(fdst->fname, fsrc->fname);
			}

			if (fsrc->arg_info) {
				lpc_copy_arg_info_array((zend_arg_info **) &fdst->arg_info, fsrc->arg_info, fsrc->num_args, pool);
			}
        }
        *(char**)&(CE_BUILTIN_FUNCTIONS(dst)[n].fname) = NULL;
    }

    if (src->type == ZEND_USER_CLASS && CE_FILENAME(src)) {
        pool_strdup(CE_FILENAME(dst), CE_FILENAME(src));
    } else {
	    CE_FILENAME(dst) = NULL;
	}
}
/* }}} */

/* {{{ lpc_copy_new_classes
	Deep copy the last set of classes added during the last compile from the CG(function_table) */
void lpc_copy_new_classes(lpc_class_t* cl_array, zend_uint count, lpc_pool* pool)
{ENTER(lpc_copy_new_classes)
    zend_uint i;
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
        }
    }
}
/* }}} */

zend_bool lpc_install_classes(lpc_class_t* classes, zend_uint num_classes, lpc_pool *pool)
{ENTER(lpc_install_classes)
   /*
	* There could be problems class inheritance problems during the module load, especially if
	* autoloading is being employed as the loading of the child class triggers autoload of the
	* parent. A missing parent will therefore forces explicit re-compile whether __autoload is
	* enabled or not, because __autoload errors cause php to die.
    *
	* Failing back to a recompile requires the uninstalling any classes already loaded and aborting
	* any function or opcode loads.  
    */
    zend_uint i;
    int i_fail = -1;
    TSRMLS_FETCH_FROM_POOL();

	for (i = 0; i < num_classes; i++) {
		lpc_class_t*       cl = classes + i;
		zend_class_entry*  dst_cl;
        zend_class_entry** parent = NULL;

		if(cl->name_len != 0 && cl->name[0] == '\0') {
		    if(zend_hash_exists(CG(class_table), cl->name, cl->name_len+1)) {
		        continue;
		    }
		}

        if (cl->parent_name != NULL) {
			if (zend_lookup_class_ex(cl->parent_name, strlen(cl->parent_name), 
#ifdef ZEND_ENGINE_2_4  /* Adds an extra paramater which is NULL for this lookup */ 
                                     NULL,
#endif
                                     0, &parent TSRMLS_PC) == FAILURE) {
				i_fail = i;
				break;
			}
		}	
		pool_alloc(dst_cl, sizeof(zend_class_entry));
		lpc_copy_class_entry(dst_cl, cl->class_entry,  pool);

		if (parent) {
	        dst_cl->parent = *parent;
	        zend_do_inheritance(dst_cl, *parent TSRMLS_PC);
	    }
		if (zend_hash_add(EG(class_table), cl->name, cl->name_len+1,
		                  &dst_cl, sizeof(zend_class_entry*), NULL) == FAILURE) {
		    lpc_error("Cannot redeclare class %s" TSRMLS_CC, cl->name);
			i_fail = i;
			break;
		}
	}

	if (i_fail >= 0) {
       /*
        * There has been a class inheritance failure, so remove installed classes in reverse order
        * and return. Note that this still leaves the class structures in memory so these WILL leak
        */
		for (i = i_fail; i >= 0; i--) {
			lpc_class_t* cl = classes + i;
		    if (zend_hash_del(EG(class_table), cl->name, cl->name_len+1) == FAILURE) {
       			 lpc_error("Cannot delete class %s" TSRMLS_CC, cl->name);
			}
		}
		return FAILURE;
	}
    return SUCCESS;
}
/* }}} */

/* {{{ copy_property_info */
void copy_property_info(zend_property_info* dst, zend_property_info* src, lpc_pool* pool)
{ENTER(copy_property_info)
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
}
/* }}} */
#ifndef ZEND_ENGINE_2_4
/* {{{ check_copy_default_property 
       returns ACCEPT if the property is not inherited from its parent class */
static lpc_check_t check_copy_default_property(Bucket* p, const void *arg1, const void *dummy)
{ENTER(check_copy_default_property)
    zend_class_entry* src = (zend_class_entry*) arg1;
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

#ifdef ZEND_ENGINE_2_2
/* {{{ fixup_property_info */
static void fixup_property_info(Bucket *p, zend_class_entry *src, zend_class_entry *dst,
                                lpc_pool* pool)
{ENTER(fixup_property_info)
    assert(((zend_property_info*)p->pData)->ce == src);
    ((zend_property_info*)p->pData)->ce = dst;
    pool_tag_ptr(((zend_property_info*)p->pData)->ce);
}
/* }}} */
#endif
/* {{{ check_copy_property_info 
       accept property info element for copy if
       (Zend Engine >= 2.2)      */
static lpc_check_t check_copy_property_info(Bucket* p, const void *arg1, 
                                                       const void *dummy)
{ENTER(check_copy_property_info)
    zend_class_entry* src    = (zend_class_entry*) arg1;
    zend_class_entry* parent = src->parent;
    zend_property_info* child_info = (zend_property_info*)p->pData;
    zend_property_info* parent_info = NULL;

#ifdef ZEND_ENGINE_2_2
    /* so much easier */
    return (child_info->ce == src) ? CHECK_ACCEPT_ELT : CHECK_SKIP_ELT;
#else
    if (parent &&
        zend_hash_quick_find(&parent->properties_info, p->arKey, p->nKeyLength,
            p->h, (void **) &parent_info)==SUCCESS) {
        if (parent_info->flags & ZEND_ACC_PRIVATE) {
            return CHECK_ACCEPT_ELT;
        }
        if ((parent_info->flags & ZEND_ACC_PPP_MASK) !=
            (child_info->flags & ZEND_ACC_PPP_MASK)) {
            return CHECK_ACCEPT_ELT;
        }
        return  CHECK_SKIP_ELT;
    }

    /* property doesn't exist in parent, copy into cached child */
    return ;
#endif
}
/* }}} */

#ifndef ZEND_ENGINE_2_4
/* {{{ check_copy_static_member */
static lpc_check_t check_copy_static_member(Bucket* p, const void *arg1, const void *arg2)
{ENTER(check_copy_static_member)
    zend_class_entry* src = (zend_class_entry*) arg1;
    HashTable * ht = (HashTable*) arg2;
    zend_class_entry* parent = src->parent;
    HashTable * parent_ht = NULL;
    char * member_name;
    char * class_name = NULL;

    zend_property_info *parent_info = NULL;
    zend_property_info *child_info = NULL;
    zval ** parent_prop = NULL;
    zval ** child_prop = (zval**)(p->pData);

    if (!parent) {
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
            if (*parent_prop == *child_prop) {
                return CHECK_SKIP_ELT;
            }
        }
    }

    return CHECK_ACCEPT_ELT;
}
/* }}} */
#endif

/* {{{ check_copy_constant */
static lpc_check_t check_copy_constant(Bucket* p, const void *arg1, const void *dummy)
{ENTER(check_copy_constant)
    zend_class_entry* src = (zend_class_entry*) arg1;
    zend_class_entry* parent = src->parent;
    zval ** child_const = (zval**)p->pData;
    zval ** parent_const = NULL;

    if (parent &&
        zend_hash_quick_find(&parent->constants_table, p->arKey, 
                             p->nKeyLength, p->h, (void **) &parent_const)==SUCCESS) {
        if ((parent_const && child_const) && (*parent_const) == (*child_const)) {
            return  CHECK_SKIP_ELT;
        }
    }

    /* possibly not in the parent */
    return CHECK_ACCEPT_ELT;
}
/* }}} */

/* {{{ fixup_method */
    #define SET_IF_SAME_NAME(member) \
        if(src->member && !strcmp(zf->common.function_name, src->member->common.function_name)) { \
            TAG_SETPTR(dst->member, zf); \
		}
void fixup_method(Bucket *p, zend_class_entry *src, zend_class_entry *dst, lpc_pool* pool)
{ENTER(fixup_method)
    zend_function* zf = p->pData;

    if (zf->common.scope == src) {
        /* Fixing up the default functions for objects here since
         * we need to compare with the newly allocated functions
         *
         * caveat: a sub-class method can have the same name as the
         * parent's constructor and create problems.
         */

        if (zf->common.fn_flags & ZEND_ACC_CTOR) {
            TAG_SETPTR(dst->constructor, zf);
        } else if (zf->common.fn_flags & ZEND_ACC_DTOR) {
            TAG_SETPTR(dst->destructor, zf);
		} else if (zf->common.fn_flags & ZEND_ACC_CLONE) {
            TAG_SETPTR(dst->clone, zf);
        } else {
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
    } else {
        /* no other function should reach here */
        assert(0);
    }
}
/* }}} */

/* {{{ check_copy_method
       returns ACCEPT if the function (class method) is in the scope of the class being copied */
static lpc_check_t check_copy_method(Bucket* p, const void *arg1, const void *dummy)
{ENTER(check_copy_method)
    zend_class_entry* src = (zend_class_entry*)arg1;
    zend_function* zf = (zend_function*)p->pData;

    return  (zf->common.scope == src) ? CHECK_ACCEPT_ELT : CHECK_SKIP_ELT;
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

