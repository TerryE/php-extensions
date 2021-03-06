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
  | Authors: Terry Ellison <Terry@ellisons.org.uk                        |
  +----------------------------------------------------------------------+

   This software includes content derived from the APC extension which was
   initially contributed to PHP by Community Connect Inc. in 2002 and revised 
   in 2005 by Yahoo! Inc. See README for further details.

   All other licensing and usage conditions are those of the PHP Group.
*/

#include "lpc.h"
#include <stdio.h>
#include "zend.h"
#include "zend_compile.h"
#include "zend_hash.h"

/* {{{ zend_op_array */
/* keep track of vld_dump_oparray() signature */
void dump(zend_op_array *op_array TSRMLS_DC)
{
#ifdef LPC_DEBUG
    void (*dump_op_array) (zend_op_array * TSRMLS_DC) = 
        lpc_resolve_symbol("vld_dump_oparray" TSRMLS_CC);
    if(dump_op_array) {
        dump_op_array(op_array TSRMLS_CC);
    } else {
        lpc_warning("vld is not installed or something even worse." TSRMLS_CC);
    }
#endif
}
/* }}} */
/* {{{ lpc_debug_enter 
    HEALTH WARNING: this code is NOT thread safe as it's only intended for coverage collection
    during development. To keep the code simple, this uses a simple hash + linear scan algo since we
    can't initialize and use a standard PHP HashTable before GINIT. At ~25% table occupancy, there
    are ~1.14 compares per lookup (avg), so it's faster and uses less memory than a HashTable for
    this use.  

    Also the get_stack_depth function is unashamedly x86 -- it's not here to stay; only to help me
    do dynamic analysis of the APC code base to be refactored. 
*/

#ifdef LPC_DEBUG

#define FUNC_MAX  0x200
#define FUNC_MASK 0x1ff

static void *get_bp(void) { __asm__ __volatile__("mov %rbp, %rax"); }

static int get_stack_depth(void) {
    void *bp;
    bp = (void *)get_bp();
    int stack_depth = -1;
    while(bp) {
        bp = *(void **)bp;
        stack_depth++; 
    }
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
"*** lpc.c", "lpc_valid_file_match", "lpc_generate_cache_name", "lpc_atol", "lpc_resolve_path",
"lpc_resolve_symbol",
"*** lpc_cache.c", "lpc_cache_create", "lpc_cache_destroy", "lpc_cache_clear", "lpc_cache_insert",
"lpc_cache_retrieve", "lpc_cache_make_key", "lpc_cache_free_key", "lpc_cache_info",
"lpc_include_or_eval_handler", 
"*** lpc_copy_class.c", "lpc_copy_new_classes", "lpc_install_classes", "lpc_copy_class_entry",
"copy_property_info", "is_local_method", "is_local_default_property", "is_local_property_info",
"is_local_static_member", "is_local_constant", "fixup_denormalised_methods",
"fixup_property_info_ce_field",
"*** lpc_copy_function.c", "lpc_copy_function", "lpc_copy_new_functions", "lpc_install_functions",
"copy_zval_out", "copy_opcodes_out", "lpc_copy_op_array", "lpc_copy_zval_ptr", 
"*** lpc_copy_source.c", "lpc_compile_file", "build_cache_entry", "cached_compile",
"file_halt_offset", "do_halt_compiler_register", 
"*** lpc_debug.c", "dump",
"*** lpc_hashtable.c", "lpc_copy_hashtable", "lpc_fixup_hashtable",
"*** lpc_pool.c", "lpc_pool_init", "lpc_pool_shutdown","_lpc_pool_alloc", "_lpc_pool_alloc_ht",
"_lpc_pool_alloc_zval", "_lpc_pool_strdup", "_lpc_pool_nstrdup", "_lpc_pool_strcmp",
"_lpc_pool_strncmp", "_lpc_pool_memcpy", "lpc_pool_storage", "lpc_pool_create",
"lpc_pool_serialize","lpc_pool_destroy", "make_pool_rbvec", "missed_tag_check", "relocate_pool",
"generate_interned_strings", "pool_compress", "pool_uncompress",
"*** lpc_request.c ***", "lpc_set_compile_hook", "lpc_module_shutdown", 
"add_filter_delims", "lpc_request_init", "lpc_request_shutdown", "lpc_dtor_context", 
//"*** lpc_string.c", "dummy_interned_strings_snapshot_for_php",
//"dummy_interned_strings_restore_for_php", "lpc_new_interned_string", "lpc_copy_internal_strings",
//"lpc_interned_strings_init", "lpc_interned_strings_shutdown",  
"*** php_lpc.c", "PHP_MINFO_FUNCTION", "PHP_GINIT_FUNCTION", "PHP_GSHUTDOWN_FUNCTION",
"PHP_RINIT_FUNCTION", "PHP_RSHUTDOWN_FUNCTION", "PHP-lpc_cache_info", "PHP-lpc_clear_cache",
"PHP-lpc_compile_file"};
    uint stack_depth,i,ndx,found;
    const char fill[] = "                                                                                                    ";
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
    if (!(LPCG(debug_flags)&(LPC_DBG_ENTER|LPC_DBG_COUNTS))) {  /* */
        return 0;
    }
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
    if (s[0]=='D' && strcmp(s+1, "UMP")==0 && (LPCG(debug_flags)&LPC_DBG_COUNTS)) { 
        /* it is an entry counter */
        for (i = 0; i<(sizeof(module)/sizeof(char *)); i++) {
            if (module[i][0] == '*') continue;
            find(module[i],ndx,found);
            assert(found); /* 'cos the key should exist */
            lpc_debug("%6i  %s" TSRMLS_CC,  func_table[ndx].func_cnt, module[i]);
        }
    } else  {                    /* it is a summary table request*/
        find(s,ndx,found);
        assert(found); /* 'cos the key should exist */
        func_table[ndx].func_cnt++;
        IF_DEBUG(ENTER) {
            stack_depth = get_stack_depth();
            lpc_debug("Entering %s %s" TSRMLS_CC,
                      (stack_depth >= sizeof(fill) ? "" : fill + (sizeof(fill)-stack_depth)),
                      s);
        }
    }
    return 0;
}

void lpc_mem_check(void) { full_mem_check(0);}
#endif /* LPC_DEBUG */
/* }}} */
