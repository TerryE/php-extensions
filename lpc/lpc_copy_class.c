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
  |          Daniel Cowgill <dcowgill@communityconnect.com>              |
  |          Rasmus Lerdorf <rasmus@php.net>                             |
  |          Arun C. Murthy <arunc@yahoo-inc.com>                        |
  |          Gopal Vijayaraghavan <gopalv@yahoo-inc.com>                 |
  +----------------------------------------------------------------------+

   This software includes content derived from the APC extension which was
   initially contributed to PHP by Community Connect Inc. in 2002 and revised 
   in 2005 by Yahoo! Inc. See README for further details.

   All other licensing and usage conditions are those of the PHP Group.
*/

#include "lpc.h"
#include "lpc_copy_class.h"
#include "lpc_copy_function.h"
#include "lpc_copy_op_array.h"
#include "lpc_hashtable.h"
#include "php_lpc.h"
//#include "lpc_string.h"
#include "Zend/zend_compile.h"
/*
 * These functions are filter functions used in lpc_copy_class_entry as lpc_copy_hashtable
 * callbacks to determine whether each element within the HashTable should be copied or if it is
 * already defined / overridden in the 'current' class entry and therefore not to be inherited.  
 */
static int is_local_method(Bucket* p, const void *, const void *);
static int is_local_default_property(Bucket* p, const void *, const void *);
static int is_local_property_info(Bucket* src, const void *, const void *);
static int is_local_static_member(Bucket* src, const void *, const void *);
static int is_local_constant(Bucket* src, const void *, const void *);
static void fixup_denormalised_methods(Bucket *p, zend_class_entry *src, 
                                       zend_class_entry *dst, lpc_pool* pool);
#ifdef ZEND_ENGINE_2_2
static void fixup_property_info_ce_field(Bucket *p, zend_class_entry *src, 
                                          zend_class_entry *dst, lpc_pool* pool);
#endif
static void copy_property_info(zend_property_info* dst, zend_property_info* src, lpc_pool* pool);

/* {{{ lpc_copy_new_classes
    Deep copy the last set of classes added during the last compile from the CG(function_table) */
void lpc_copy_new_classes(lpc_class_t** pcl_array, zend_uint count, lpc_pool* pool)
{ENTER(lpc_copy_new_classes)
    lpc_class_t *cle;
    char* key;
    zend_class_entry **class_ptr;
    uint i, key_length, dummy_len;
    TSRMLS_FETCH_FROM_POOL()

    pool_alloc(*pcl_array, sizeof(lpc_class_t) * count);
    memset (*pcl_array, 0, sizeof(lpc_class_t) * count);
    cle = *pcl_array;
   /*
    * Normally count is small compared to the size of the class table, so move back count-1 
    * functions and read the last count classname => class from the table into the cl_array.  Note 
    * that the key may be a mangled class name and different to class->name which is the unmangled
    * version. Alson ote that the PHP convention for zvals and HT key lengths is inconsistent as
    * the former excludes the terminating \0 char and the latter includes it.
    */
    zend_hash_internal_pointer_end(CG(class_table));
    for (i = 1; i < count; i++) {
        zend_hash_move_backwards(CG(class_table));
    }
    for (i = 0; i < count; i++, cle++, zend_hash_move_forward(CG(class_table))) {
        zend_hash_get_current_key_ex(CG(class_table), &key, &key_length, NULL, 0, NULL);
        zend_hash_get_current_data(CG(class_table), (void**) &class_ptr);
        pool_nstrdup(cle->name, dummy_len, key, key_length-1, 0);
        lpc_copy_class_entry(&cle->class_entry, *class_ptr, pool);
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
    * Failing back to a recompile requires the uninstalling any classes already loaded and
    * aborting any function or opcode loads.  
    */
    lpc_class_t       *cl;
    char              *cl_name, *cl_err_name, *parent_name;
    zend_class_entry  *dst_cl, **parent;
    zend_uint          cl_name_len, i;
    TSRMLS_FETCH_FROM_POOL()

    for (i = 0, cl = classes; i < num_classes; i++, cl++) {
        zend_bool do_inheritance = 0;
       /*
        * Skip duplicated mangled names, as this will be pricked up at runtime when the class is 
        * attempted to be bound with the multiple DECLARE_CLASS zend_ops.  If the class has a 
        * parent set to the interned parent name. If this parent exists, then this was a compile-
        * time bound class that needs to be rebound.  If it doesn't exist then bailout, otherwise
        * copy the class.   
        */
        pool_nstrdup(cl_name, cl_name_len, cl->name, 0, 0); /* de-intern */
        if (cl_name[0] || !zend_hash_exists(CG(class_table), cl_name, cl_name_len+1)) {
            if (cl->class_entry.parent) {
                pool_strdup(parent_name, (char *) cl->class_entry.parent, 0); /* de-intern */
                do_inheritance = (zend_lookup_class_ex(parent_name, strlen(parent_name),
#ifdef ZEND_ENGINE_2_4
                                                       NULL,
#endif
                                                       0, &parent TSRMLS_PC) == SUCCESS);
                CHECK(do_inheritance);
            }
            pool_alloc(dst_cl, sizeof(zend_class_entry));
            lpc_copy_class_entry(dst_cl, &cl->class_entry,  pool);

            DEBUG2(LOAD,"Class %s installed, current memory usage %u Kb)", 
                        dst_cl->name, zend_memory_usage(0 TSRMLS_PC));

            if (do_inheritance) {
                zend_do_inheritance(dst_cl, *parent TSRMLS_PC);
            }

            CHECK(zend_hash_add(EG(class_table), cl_name, cl_name_len+1,
                              &dst_cl, sizeof(zend_class_entry*), NULL) == SUCCESS);
        }
    }
    return SUCCESS;

error:
   /*
    * There has been a class inheritance failure, so remove installed classes in reverse order
    * and return. Note that this still leaves the class structures in memory so these WILL leak
    */
    while (i--) {
        zend_class_entry **class_ptr;
        pool_nstrdup(cl_err_name, cl_name_len, classes[i].name, 0, 0); /* de-intern poss mangled */
        if (zend_hash_find(EG(class_table), cl_err_name, cl_name_len, (void **) &class_ptr) 
            == SUCCESS) {
            destroy_zend_class(class_ptr);
            zend_hash_del(EG(class_table), cl_err_name, cl_name_len+1);
            efree(cl_name);
        }
    }
    lpc_warning("Cannot redeclare class %s" TSRMLS_CC, dst_cl->name);
    return FAILURE;
}
/* }}} */


#define TAG_SETPTR(dst,src) dst = src; pool_tag_ptr(dst)

#ifdef ZEND_ENGINE_2_4
# define CE_FILENAME(ce)          (ce)->info.user.filename
# define CE_DOC_COMMENT(ce)       (ce)->info.user.doc_comment
# define CE_DOC_COMMENT_LEN(ce)   (ce)->info.user.doc_comment_len
# define CE_BUILTIN_FUNCTIONS(ce) (ce)->info.internal.builtin_functions
#else
# define CE_FILENAME(ce)          (ce)->filename
# define CE_DOC_COMMENT(ce)       (ce)->doc_comment
# define CE_DOC_COMMENT_LEN(ce)   (ce)->doc_comment_len
# define CE_BUILTIN_FUNCTIONS(ce) (ce)->builtin_functions
#endif

#define CHECK(p) if(!(p)) goto error

/* {{{ lpc_copy_class_entry */
void lpc_copy_class_entry(zend_class_entry* dst, zend_class_entry* src, lpc_pool* pool)
/*
 * Deep copying a zend_class_entry (see Zend/zend.h) involves various components:
 *  1)  scalars which need to be "bit copied"
 *  2)  scalars which shouldn't be inherited on copy
 *  3)  Embeded HashTables and pointers to structures which need to be deep copied
 *  4)  function pointers which need to be relocated.
 *
 * (1) is achieved by doing a base "bit copy" of the entire class entry; (2) by explicitly setting
 * to 0/NULL; (3) are the fields ([E] is an emalloced element; [R] a derived address reference):
 *
 *    char                *name               [E]
 *    zend_class_entry    *parent             [R]
 *    zend_function_entry *builtin_functions  [R]
 *    zend_class_entry   **interfaces         [E]
 *    char                *filename           [R]
 *    char                *doc_comment        [E]
 *    zend_module_entry   *module             [R]
 *
 * and the HashTables:
 *    
 *    function_table            zend_function_dtor
 *    default_properties        zval_ptr_dtor_wrapper
 *    properties_info           zend_destroy_property_info
 *    default_static_members    zval_ptr_dtor_wrapper
 *    constants_table           zval_ptr_dtor_wrapper
 *    *static_members           [R] (points to default_static_member in the case of user classes)
 * 
 * (4) are the function pointers for : constructor, destructor, clone, __get, __set, __unset,
 * __isset, __call, __callstatic, __tostring, serialize_func, unserialize_func, create_object,
 * get_iterator, interface_gets_implemented, get_static_method, serialize, unserialize
 * 
 * The compiler binds class inheritance at compile-time, but for reasons discussed in TECHNOTES, LPC
 * must back these changes out on copy-out. Hence on copy-out of a child class already bound to its
 * parent, the COPY_HT macros use a filter routine to  determine if the current element is local
 * (that is not inherited) and therefore must be copied.  These are checks are tagged as in
 * comments as *** compile-time inheritence backout *** and are clearly only done on copy-out. 
 * 
 * Some fields which have denormalised for performance reasons (e.g the above function pointer), so
 * where appropriate a fixup_hashtable callback is used to do this.  As this is only done on
 * copy-in, these routines all assume copy-in mode to simplify coding.
 */
{ENTER(lpc_copy_class_entry)
    uint i = 0;
    lpc_ht_copy_element copy_out_is_local_method           = NULL ;
    lpc_ht_copy_element copy_out_is_local_default_property = NULL ;
    lpc_ht_copy_element copy_out_is_local_property_info    = NULL ;
    lpc_ht_copy_element copy_out_is_local_static_member    = NULL ;
    lpc_ht_copy_element copy_out_is_local_constant         = NULL ;
    TSRMLS_FETCH_FROM_POOL()

    assert( src->type == ZEND_USER_CLASS && src->name );

/* all done through class inheritance so can be zero for saving compiled class to cache 
            src->iterator_funcs.funcs       == NULL &&
            src->create_object              == NULL &&
            src->get_iterator               == NULL &&
            src->interface_gets_implemented == NULL &&
            src->get_static_method          == NULL &&
            src->module                     == NULL);
*/
    /* So many fields are zeroed that its easier to default to zero and only copy non-zero ones */
    memset(dst, 0, sizeof(zend_class_entry));

    pool_nstrdup(dst->name, dst->name_length, src->name, src->name_length, 1);
    dst->type       = ZEND_USER_CLASS;
    dst->line_start = src->line_start;
    dst->line_end   = src->line_end;

    if (is_copy_out()) {

       /*
        * If the class has a pointer to its parent class then the compiler has performed compile-
        * time inheritance which must be backed out, but the parent class name is first interned
        * into the parent field to be avaiable for rebinding on subsequent retrieval.
        */
        if (src->parent) {
            pool_strdup(dst->parent, src->parent->name, 0);

            copy_out_is_local_method           = is_local_method;
            copy_out_is_local_default_property = is_local_default_property;
            copy_out_is_local_property_info    = is_local_property_info;
            copy_out_is_local_static_member    = is_local_static_member;
            copy_out_is_local_constant         = is_local_constant;
        }

       /*
        * Local class interfaces are NULLed then populated at runtime using ADD_INTERFACE zend_op.  
        * Inherited ones added if the class is compile-time bound to a parent, should be ignored; 
        */    
        for(i = 0 ; (i < src->num_interfaces) && !src->interfaces[i]; i++) {}
        dst->num_interfaces = i;
        dst->interfaces     = src->interfaces ? LPC_ALLOCATE_TAG : NULL;  
///  TODO: this used to be set to 0 for nonbound child classes?

    } else {

        if (src->parent) {
           pool_strdup(dst->parent, (char *)src->parent, 0);
        }

        zend_initialize_class_data(dst, 1 TSRMLS_CC);

        dst->num_interfaces = src->num_interfaces;

        if (src->interfaces) { /* Only alloc array if original was allocated */
            pool_alloc(dst->interfaces, sizeof(zend_class_entry*) * src->num_interfaces);
            memset(dst->interfaces, 0,  sizeof(zend_class_entry*) * src->num_interfaces);
            }

        CE_FILENAME(dst) = LPCGP(current_filename);
    }

    dst->ce_flags   = src->ce_flags;

    /* In the case of a user class dup the doc_comment if any */
    if (CE_DOC_COMMENT(src)) {
        pool_memcpy(CE_DOC_COMMENT(dst),
                    CE_DOC_COMMENT(src), (CE_DOC_COMMENT_LEN(src) + 1));
        CE_DOC_COMMENT_LEN(dst) = CE_DOC_COMMENT_LEN(src);
    }
 
    COPY_HT(function_table, lpc_copy_function, zend_function, copy_out_is_local_method, NULL);

#ifdef ZEND_ENGINE_2_4
    dst->default_properties_count = src->default_properties_count;
    if (src->default_properties_count) {
        pool_alloc(dst->default_properties_table, 
                   sizeof(zval*) * src->default_properties_count);
        for (i = 0; i < src->default_properties_count; i++) {
            if (src->default_properties_table[i]) {
                lpc_copy_zval_ptr(&dst->default_properties_table[i], 
                                  (const zval**) &src->default_properties_table[i], pool);
//////          pool_tag_ptr(dst->default_properties_table[i]);
            } else {
                dst->default_properties_table[i] = NULL;
            }
        }
    } else {
        dst->default_properties_table = NULL;
    }
#else
    COPY_HT(default_properties, lpc_copy_zval_ptr, zval *, 
            copy_out_is_local_default_property, NULL);
#endif

    COPY_HT(properties_info, copy_property_info, zend_property_info, 
            copy_out_is_local_property_info, NULL);

    if (is_copy_in()) {
        lpc_fixup_hashtable(&dst->function_table, fixup_denormalised_methods, src, dst, pool);
#ifdef ZEND_ENGINE_2_2
        lpc_fixup_hashtable(&dst->properties_info, fixup_property_info_ce_field, src, dst, pool);
#endif
    }

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
            copy_out_is_local_static_member, NULL);

    if (src->static_members && src->static_members != &src->default_static_members) {
        pool_alloc_ht(dst->static_members); 
        COPY_HT_P(static_members, lpc_copy_zval_ptr, zval *,
                  copy_out_is_local_static_member, (void *)1);
    } else if (is_copy_in()){
        dst->static_members = &dst->default_static_members;
    }
#endif
    COPY_HT(constants_table, lpc_copy_zval_ptr, zval *, 
            copy_out_is_local_constant, NULL);

}
/* }}} */

/* {{{ Hashtable copy callbacks used in deep copy of the class entry */
/* {{{ copy_property_info */
void copy_property_info(zend_property_info* dst, zend_property_info* src, lpc_pool* pool)
{ENTER(copy_property_info)
    BITWISE_COPY(src,dst);
   /*
    * Note that private properties are mangled by prefixing \0<classname>\0 to the property name.
    */
   if (src->name) {
        pool_nstrdup(dst->name, dst->name_length, src->name, src->name_length, 1);
   }

#if defined(ZEND_ENGINE_2) && PHP_MINOR_VERSION > 0
    if (src->doc_comment) {
        pool_nstrdup(dst->doc_comment, dst->doc_comment_len, 
                     src->doc_comment, src->doc_comment_len, 1);
    }
#endif
} 
/* }}} */
/* }}} Hashtable copy callbacks */

/* {{{ Hashtable copy filters used in deep copy of the class entry
       These all return true if the element is to be copied.  In the case of class entry copying
       these are used to implement compile-time inheritence backout and so return true if the
       element is local to the class.  Any elements that have been inherited from a parent 
       return false and therefore are ignored on the copy */
/* {{{ is_local_method */
static int is_local_method(Bucket* p, const void *arg1, const void *dummy)
{ENTER(is_local_method)
   /*
    * Only locally defined methods should be copied as any inherited by a compile-time 
    * zend_do_inheritance() will be bound at load or runtime anyway.
    */
    zend_class_entry *src = (zend_class_entry*)arg1;
    zend_function    *zf  = (zend_function*)p->pData;
    return  zf->common.scope == src;
}
/* }}} */
/* {{{ is_local_default_property  */
#ifndef ZEND_ENGINE_2_4
static int is_local_default_property(Bucket* p, const void *arg1, const void *dummy)
{ENTER(is_local_default_property)
    zend_class_entry  *src                       = (zend_class_entry*) arg1;
    HashTable         *parent_default_properties = &src->parent->default_properties;
    zval             **child_prop                = (zval**)p->pData;
    zval             **parent_prop               = NULL;
   /*
    * The property's not local if the parent has the same property at the same address
    */
    return  (zend_hash_quick_find(parent_default_properties, p->arKey, p->nKeyLength,
                                   p->h, (void **) &parent_prop)==FAILURE) ||
             !parent_prop || !child_prop ||
             *parent_prop != *child_prop;
}
#endif
/* }}} */
/* {{{ is_local_property_info */
static int is_local_property_info(Bucket* p, const void *arg1, const void *dummy)
{ENTER(is_local_property_info)
    zend_class_entry   *src                    = (zend_class_entry*) arg1;
    HashTable          *parent_properties_info = &src->parent->properties_info;
    zend_property_info *child_info             = (zend_property_info*)p->pData;
    zend_property_info *parent_info            = NULL;

#ifdef ZEND_ENGINE_2_2
    return (child_info->ce == src);
#else
    return (zend_hash_quick_find(parent_properties_info, p->arKey, p->nKeyLength,
                                 p->h, (void **) &parent_info)==FAILURE) ||
           (parent_info->flags & ZEND_ACC_PRIVATE) ||
           ((parent_info->flags & ZEND_ACC_PPP_MASK) != (child_info->flags & ZEND_ACC_PPP_MASK)));
#endif
}
/* }}} */

#ifndef ZEND_ENGINE_2_4
/* {{{ is_local_static_member */
static int is_local_static_member(Bucket* p, const void *arg1, const void *arg2)
{ENTER(is_local_static_member)
    zend_class_entry  *src                    = (zend_class_entry*) arg1;
    int                is_sm                  = (arg2 != NULL);
    HashTable         *parent_properties_info = &src->parent->properties_info;
    HashTable         *parent_sm_ht           = is_sm ? src->parent->static_members :
                                                        &src->parent->default_static_members;
    zval             **child_prop             = (zval**)(p->pData);
    zval             **parent_prop = NULL;
    char              *unmangled, *class_name;
    int                unmangled_len;
    void             *dummy;
    
   /*
    * The member isn't local if the parent has the same member at the same address.  Protected
    * properties are unmangled on inheritance -- hence the unmangling checks.
    */
/////// TODO:  This algo doesn't make sense -- create testcase & step through in debugger
    zend_unmangle_property_name(p->arKey, 
#ifdef ZEND_ENGINE_2_2
                                p->nKeyLength-1,
#endif
                                &class_name, &unmangled);
    unmangled_len = strlen(unmangled)+1;

    return zend_hash_find(parent_properties_info, unmangled, unmangled_len, &dummy) == FAILURE ||
           zend_hash_find(&src->properties_info, unmangled, unmangled_len, &dummy) == FAILURE ||
           zend_hash_quick_find(parent_sm_ht, p->arKey, p->nKeyLength, p->h, (void**)&parent_prop) 
               == FAILURE ||
           *parent_prop != *child_prop;
}
/* }}} */
#endif

/* {{{ is_local_constant */
static int is_local_constant(Bucket* p, const void *arg1, const void *dummy)
{ENTER(is_local_constant)
    zend_class_entry *src                    = (zend_class_entry*) arg1;
    HashTable        *parent_constants_table = &src->parent->constants_table;
    zval            **child_const            = (zval**)p->pData;
    zval           **parent_const            = NULL;

    return zend_hash_quick_find(parent_constants_table, p->arKey, 
                                p->nKeyLength, p->h, (void **) &parent_const) == FAILURE ||
           !parent_const || !child_const ||
           *parent_const != *child_const;
}
/* }}} */


/* {{{ Fixup routines to set fields which have denormalised for performance reasons */
/* {{{ fixup_denormalised_methods */
void fixup_denormalised_methods(Bucket *p, zend_class_entry *src, zend_class_entry *dst, 
                                lpc_pool* pool)
{ENTER(fixup_denormalised_methods)
    zend_function* zf = p->pData;
    /* 
     * The class_entry contains fields for the address of some standard functions.  All functions
     * are checked against this list and the fields set as required.  This is only done on copy-in
     * so all entries are local to the class rather than inherited.  Method names can't be magic
     * and have already been validated by zend_do_begin_function_declaration(), so a straight
     * string comparison can by used for the match.
     */
    if (zf->common.fn_flags & ZEND_ACC_CTOR) {
        dst->constructor = zf;
    } else if (zf->common.fn_flags & ZEND_ACC_DTOR) {
        dst->destructor  = zf;
    } else if (zf->common.fn_flags & ZEND_ACC_CLONE) {
        dst->clone       = zf;
    } else {
        char *method = zf->common.function_name;
        if (method[0] == '_' && method[1] == '_') {
#define SET_IF_SAME(ame,fld)  if (!strcmp(method+3, #ame)) dst->__ ## fld = zf; 
            switch (method[2]) {

            case 'c':
#ifdef ZEND_ENGINE_2_3
                SET_IF_SAME(allStatic, callstatic);
#endif
                SET_IF_SAME(all, call);         break;
            case 'g':
                SET_IF_SAME(et, get);           break;
            case 'i':
                SET_IF_SAME(sset, isset);       break;
            case 's':
                SET_IF_SAME(et, set);           break;
#ifdef ZEND_ENGINE_2_2
            case 't':
                SET_IF_SAME(oString, tostring); break;
#endif
            case 'u':
                SET_IF_SAME(unset, unset);      break;
            default: break;
#undef SET_IF_SAME
            }
        }
    }
    zf->common.scope = dst;
}
/* }}} */

#ifdef ZEND_ENGINE_2_2
/* {{{ fixup_property_info_ce_field */
static void fixup_property_info_ce_field(Bucket *p, zend_class_entry *src, zend_class_entry *dst,
                                lpc_pool* pool)
{ENTER(fixup_property_info_ce_field)
    ((zend_property_info*)p->pData)->ce = dst;
 }
/* }}} */
#endif
/* }}} Fixup routines */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
     */

