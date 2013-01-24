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
#include "lpc_copy_op_array.h"
#include "lpc_copy_function.h"

/* {{{ lpc_copy_function */
void lpc_copy_function(zend_function* dst, zend_function* src, lpc_pool* pool)
{ENTER(lpc_copy_function)
    assert(src != NULL);
    BITWISE_COPY(src,dst);
   /*
    * The union zend_function is defined in zend_compile.h and the first group of fields is common 
    * to all. This includes a type selector with one of the following swithed values and the three
    * pointers: function_name, (ce) scope, prototype and arg_info. User functions are deep-copy and
    * everything else is by reference.  Also back out any fields set by compile-time 
    * zend_do_inheritance() as this will be done at load or runtime anyway.
    */
    if (src->type == ZEND_USER_FUNCTION) {
        lpc_copy_op_array(&dst->op_array, &src->op_array, pool);
    } else {
        dst->op_array = src->op_array;
    }

    dst->common.fn_flags = src->common.fn_flags & (~ZEND_ACC_IMPLEMENTED_ABSTRACT);
    dst->common.prototype = NULL;
}
/* }}} */

/* {{{ lpc_copy_new_functions 
    Deep copy the last set of functions added during the last compile from the CG(function_table) */
void lpc_copy_new_functions(lpc_function_t** pdst_array, uint count, lpc_pool* pool)
{ENTER(lpc_copy_new_functions)
    uint i;
    lpc_function_t* dst_array;
    TSRMLS_FETCH_FROM_POOL()

    pool_alloc(*pdst_array, sizeof(lpc_function_t) * count);
    dst_array =*pdst_array;

   /*
    * The functions table can typically have ~1K entries and the source only adds a few of these, so
    * it's better to count back count-1 functions from the end of the function table.
    */
    zend_hash_internal_pointer_end(CG(function_table));
    for (i = 1; i < count; i++) {
        zend_hash_move_backwards(CG(function_table));
    }
   /*
    * Now add the next <count> functions to the dst_array 
    */
    for (i = 0; i < count; i++, zend_hash_move_forward(CG(function_table))) {
        char* key;
        uint key_length, dummy_len;
        zend_function* fun;

        zend_hash_get_current_key_ex(CG(function_table), &key, &key_length, NULL, 0, NULL);
        zend_hash_get_current_data(CG(function_table), (void**) &fun);

        pool_nstrdup(dst_array[i].name, dummy_len, key, (uint) key_length-1, 0);

        pool_alloc(dst_array[i].function, sizeof(zend_function));
        lpc_copy_function(dst_array[i].function, fun, pool);
    }                                                                                                               
}
/* }}} */

/* {{{ lpc_install_functions  */ 

void lpc_install_functions(lpc_function_t* functions, uint num_functions, lpc_pool* pool)
{ENTER(lpc_install_functions)
    lpc_function_t *fn;
    uint i;
    TSRMLS_FETCH_FROM_POOL();
    for (i = 0, fn = functions; i < num_functions; i++, fn++) {
       /*
       * Installed functions are maintained by value in the EG(function_table). However, all of the
        * dynamically allocated fields must be copied from the serial pool (which is about to be
        * destroyed) into exec pool (emalloc) storage. lpc_copy_function() does this deep copy. As
        * the function structure itelf is coped by value, it can be allocated on the stack. 
        */
        char *fn_name;
        zend_uint fn_name_len;
        zend_function func;

        pool_nstrdup( fn_name, fn_name_len, fn->name, 0, 0);  /* de-intern */

        lpc_copy_function(&func, fn->function,  pool);

        if (zend_hash_add(EG(function_table), fn_name, fn_name_len+1,
                            &func, sizeof(*fn->function), NULL) == FAILURE) {
            lpc_error("Cannot redeclare %s()" TSRMLS_CC, fn->name);
        }
    }
}
/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */

