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

#include "lpc.h"
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
		HEALTH WARNING this code is NOT thread safe as it's only intended for coverage collection during 
		development.  To keep the code simple, this uses a simple hash + linear scan algo since we can't 
		initialize and use a standard PHP HashTable before GINIT. At ~25% table occupancy, there are ~1.14
		compares per lookup (avg), so it's faster and uses less memory than a HashTable for this use.  

		Also the get_stack_depth function is unashamedly x86 -- it's not here to stay; only to help me do 
		dynamic analysis of the APC code base to be refactored. 
*/

#ifdef APC_DEBUG

#define FUNC_MAX  0x200
#define FUNC_MASK 0x1ff

static void *get_bp(void) { __asm__ __volatile__("mov %rbp, %rax"); }

static int get_stack_depth(void) {
    void *bp = (void *)get_bp();
    int stack_depth = 0;
    while((bp = *(void **)bp) != NULL) { stack_depth++; }
    return stack_depth;
}

int lpc_debug_enter(char *s)
{
		struct _func_table {
			const char* func_name;
			ulong func_cnt;
		};
		static struct _func_table func_table[FUNC_MAX] = {0};
		static int n_func_probe = 0;
		static const char* module[] = {
"=== lpc.c ===", "lpc_valid_file_match", "lpc_cache_create", "lpc_cache_destroy", "lpc_cache_clear",
    "lpc_cache_insert", "lpc_cache_retrieve", "lpc_cache_make_key", "lpc_cache_free_key",
    "lpc_cache_info", "lpc_get_request_context", "lpc_dtor_context",
"=== lpc_compile.c ===","lpc_compile_cache_entry", "my_copy_zval_ptr", "my_copy_zval",
    "my_copy_zend_op", "lpc_copy_function", "my_copy_property_info", "my_copy_arg_info_array",
    "lpc_copy_class_entry", "my_copy_hashtable", "lpc_fixup_op_array_jumps", "lpc_copy_op_array",
    "my_copy_new_functions", "my_copy_new_classes", "my_file_halt_offset",
    "lpc_do_halt_compiler_register", "my_fixup_function", "my_fixup_property_info",
    "my_fixup_hashtable", "my_check_copy_function", "my_check_copy_default_property",
    "my_check_copy_property_info", "my_check_copy_static_member", "my_check_copy_constant", 
"=== lpc_main.c ===", "set_compile_hook", "cached_compile", "my_compile_file",
    "lpc_module_shutdown", "lpc_deactivate", "lpc_request_init", "lpc_request_shutdown", 
"=== lpc_pool.c ===", "_lpc_pool_create", "_lpc_pool_destroy", "_lpc_pool_set_size",
    "_lpc_pool_alloc", "_lpc_pool_alloc_zval", "_lpc_pool_alloc_zval", "_lpc_pool_strdup",
    "_lpc_pool_memcpy", "_lpc_pool_unload", "_lpc_pool_load", "lpc_make_PIC_pool", "lpc_relocate_pool", 
"=== lpc_string.c ===", "lpc_dummy_interned_strings_snapshot_for_php",
    "lpc_dummy_interned_strings_restore_for_php", "lpc_new_interned_string",
    "lpc_copy_internal_strings", "lpc_interned_strings_init", "lpc_interned_strings_shutdown", 
"=== lpc_zend.c ===", "lpc_op_ZEND_INCLUDE_OR_EVAL", 
"=== lpc_zend.c ===", "lpc_zend_shutdown", 
"=== php_lpc.c ===", "lpc_atol", "PHP_MINFO_FUNCTION", "PHP_GINIT_FUNCTION",
    "PHP_GSHUTDOWN_FUNCTION", "PHP_RINIT_FUNCTION", "PHP_RSHUTDOWN_FUNCTION", "PHP-lpc_cache_info",
    "PHP-lpc_clear_cache", "PHP-lpc_compile_file"};
	int stack_depth = get_stack_depth();
	const char fill[] = "                                                                                                    ";
	uint i,ndx,found;
	TSRMLS_FETCH();

#define find(str,n,found) \
	do {found = 2;\
		ulong hash = zend_inline_hash_func(str, strlen(str)); \
		for (n = 0; n < FUNC_MAX; n++) { \
			uint hash_n = ( hash + n ) & FUNC_MASK; \
			n_func_probe++; \
			if(func_table[hash_n].func_name == NULL) \
				{ found = 0; n=hash_n; break; } \
			if (strcmp(str, func_table[hash_n].func_name) == 0 ) \
				{ found = 1; n=hash_n; break; } \
		} \
		assert(found != 2);\
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
	if (strcmp(s, "DUMP")!=0) { /* it is an entry counter */
		lpc_debug("Entering %s %s" TSRMLS_CC,
		(stack_depth >= sizeof(fill)) ? "" : fill + (sizeof(fill)-stack_depth), s);
		find(s,ndx,found);
		assert(found); /* 'cos the key should exist */
		func_table[ndx].func_cnt++;
	} else {                    /* it is a summary table request*/
		for (i = 0; i<(sizeof(module)/sizeof(char *)); i++) {
			find(module[i],ndx,found);
			assert(found); /* 'cos the key should exist */
//			lpc_debug("%6i  %s" TSRMLS_CC,  func_table[ndx].func_cnt, module[i]);
		}
	}
	return 0;
}

void lpc_mem_check(void) { full_mem_check(0);}
#endif /* APC_DEBUG */
/* }}} */
