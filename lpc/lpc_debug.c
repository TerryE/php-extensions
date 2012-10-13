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
  | Authors: Daniel Cowgill <dcowgill@communityconnect.com>              |
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

/* $Id: lpc_debug.c 307048 2011-01-03 23:53:17Z kalle $ */
#include "lpc.h"
#include <stdio.h>
#include "zend.h"
#include "zend_compile.h"
#include "zend_hash.h"

/* {{{ zend_op_array */
#if defined(__DEBUG_LPC__)
/* keep track of vld_dump_oparray() signature */
typedef void (*vld_dump_f) (zend_op_array * TSRMLS_DC);
#endif

void dump(zend_op_array *op_array TSRMLS_DC)
{ENTER(dump)
#if defined(__DEBUG_LPC__)
  vld_dump_f dump_op_array;
  DL_HANDLE handle = NULL;

#ifdef PHP_WIN32
  handle = GetModuleHandle(NULL);
  
  if (!handle) {
	lpc_warning("unable to fetch current module handle." TSRMLS_CC);
  }
#endif
  
  dump_op_array = (vld_dump_f) DL_FETCH_SYMBOL(handle, "vld_dump_oparray");
  
#ifdef PHP_WIN32
  DL_UNLOAD(handle);
#endif

  if(dump_op_array) {
    dump_op_array(op_array TSRMLS_CC);
  
    return;
  }
  
  lpc_warning("vld is not installed or something even worse." TSRMLS_CC);
#endif
}
/* }}} */

/* {{{ lpc_debug_enter 
		HEALTH WARNING this code is not thread safe as it's only intended for coverage collection during 
		testing.  This uses a simple linear scan hash algo since we can initialize and use a standard PHP
		HashTable before GINIT. At ~50% table occupancy, the average compares per lookup is still ~1.1 
		so its actually a lot faster and uses less memory than a HashTable */

#ifdef APC_DEBUG

#define FUNC_MAX  0x200
#define FUNC_MASK 0x1ff

int lpc_debug_enter(char *s)
{
		struct _func_table {
			const char* func_name;
			ulong func_cnt;
		};
		static struct _func_table func_table[FUNC_MAX] = {0};
		static int n_func_probe = 0;
		static const char* module[] = {
	"==lpc.c==", "lpc_tokenize", "lpc_restat", "lpc_search_paths", "lpc_regex_destroy_array", "lpc_regex_match_array",
		"lpc_regex_compile_array", "lpc_crc32", "crc32gen", "lpc_flip_hash", 
	"==lpc_cache.c==", "hash", "make_prime", "make_slot", "free_slot", "remove_slot", "lpc_cache_create", 
		"lpc_cache_destroy", "lpc_cache_clear", "lpc_cache_insert", "lpc_cache_find_slot", "lpc_cache_find", 
		"lpc_cache_release", "lpc_cache_make_file_key", "lpc_cache_make_file_entry", "lpc_cache_link_info", "lpc_cache_info", 
	"==lpc_compile.c==", "LPC_SERIALIZER_NAME", "LPC_UNSERIALIZER_NAME", "check_op_array_integrity",
		"my_bitwise_copy_function", "my_copy_zval_ptr", "my_serialize_object", "my_unserialize_object", "my_copy_zval",  
		"my_check_znode", "my_copy_zend_op","my_copy_znode", "my_copy_zend_op", "my_copy_function", "my_copy_function_entry",  
		"my_copy_property_info", "my_copy_property_info_for_execution", "my_copy_arg_info_array", "my_copy_arg_info", 
		"lpc_copy_class_entry", "my_copy_class_entry", "my_copy_hashtable_ex", "my_copy_static_variables", "lpc_copy_zval", 
		"lpc_fixup_op_array_jumps", "lpc_copy_op_array", "lpc_copy_new_functions", "lpc_copy_new_classes",
		"my_prepare_op_array_for_execution", "lpc_copy_op_array_for_execution", "lpc_copy_function_for_execution", 
		"lpc_copy_function_for_execution_ex", "lpc_copy_class_entry_for_execution", "lpc_free_class_entry_after_execution",
		"lpc_file_halt_offset", "lpc_do_halt_compiler_register", "my_fixup_function", "my_fixup_property_info", 
		"my_fixup_hashtable", "my_check_copy_function", "my_check_copy_default_property", "my_check_copy_property_info", 
		"my_check_copy_static_member", "my_check_copy_constant", "lpc_register_optimizer",
	"==lpc_debug.c==" "dump", 
	"==lpc_main.c==", "set_compile_hook", "install_function", "lpc_lookup_function_hook", "install_class", 
		"lpc_lookup_class_hook", "uninstall_class", "copy_function_name", "copy_class_or_interface_name",
		"lpc_defined_function_hook", "lpc_declared_class_hook", "cached_compile", "lpc_compile_cache_entry", 
		"my_compile_file", "_lpc_register_serializer", "lpc_find_serializer", "lpc_get_serializers", "lpc_module_init",
		"lpc_module_shutdown", "lpc_deactivate", "lpc_request_init", "lpc_request_shutdown",
	"==lpc_pool.c==", "_lpc_pool_create", "_lpc_pool_destroy", "_lpc_pool_set_size", 
		"_lpc_pool_alloc", "_lpc_pool_strdup", "_lpc_pool_memcpy", 
	"==lpc_stack.c==", "lpc_stack_create", "lpc_stack_destroy", "lpc_stack_clear", "lpc_stack_push", "lpc_stack_pop",
		"lpc_stack_top", "lpc_stack_get", "lpc_stack_size", 
	"==lpc_string.c==", "lpc_dummy_interned_strings_snapshot_for_php", "lpc_dummy_interned_strings_restore_for_php",
		"lpc_copy_internal_strings", "lpc_interned_strings_init", "lpc_interned_strings_shutdown", 
	"==lpc_zend.c==", "lpc_php_malloc", "lpc_php_free", "lpc_op_ZEND_INCLUDE_OR_EVAL", "lpc_zend_init", 
		"lpc_zend_shutdown", 
	"==php_lpc.c==", "lpc_atol", "PHP_MINFO_FUNCTION", "PHP_GINIT_FUNCTION", "PHP_GSHUTDOWN_FUNCTION", 
		"PHP_MINIT_FUNCTION", "PHP_MSHUTDOWN_FUNCTION", "PHP_RINIT_FUNCTION", "PHP_RSHUTDOWN_FUNCTION" };
	uint i,ndx,found;
	TSRMLS_FETCH();

#define find(str,ndx,found) \
	do {found = 2;\
		ulong hash = zend_inline_hash_func(str, strlen(str)); \
		for (ndx = 0; ndx < FUNC_MAX; ndx++, n_func_probe++) { \
			uint hash_ndx = ( hash + ndx ) & FUNC_MASK; \
			if(func_table[hash_ndx].func_name == NULL) \
				{ found = 0; ndx=hash_ndx; break; } \
			if (strcmp(str, func_table[hash_ndx].func_name) == 0 ) \
				{ found = 1; ndx=hash_ndx; break; } \
		} \
		assert(ndx != 2);\
	} while (0)
	
	if (n_func_probe == 0) {
		for (i = 0; i<(sizeof(module)/sizeof(char *)); i++) {
			find(module[i],ndx,found);
			if (!found) {
				func_table[ndx].func_name = module[i];
			}
		}
		lpc_debug("Funtion Table initialised with %i elements in %i probes" TSRMLS_CC,  
		          sizeof(module)/sizeof(char *), n_func_probe);
	}
	if (strcmp(s, "DUMP")!=0) { /* it is a real entry counter */
		lpc_debug("Entering %s" TSRMLS_CC, s);
		find(s,ndx,found);
		assert(found); /* 'cos the key should exist */
		func_table[ndx].func_cnt++;
	} else { /* it is a summary table */
		for (i = 0; i<(sizeof(module)/sizeof(char *)); i++) {
			find(module[i],ndx,found);
			assert(found); /* 'cos the key should exist */
			lpc_debug("%6i  %s" TSRMLS_CC,  func_table[ndx].func_cnt, module[i]);
		}
	}
	return 0;
}
#endif /* APC_DEBUG */
/* }}} */
