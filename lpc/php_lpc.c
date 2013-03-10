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

#ifdef HAVE_SYS_FILE_H
#include <sys/file.h>
#endif
#include "lpc.h"
#include "php_lpc.h"
#include "lpc.h"
#include "lpc_cache.h"
#include "lpc_request.h"
#include "lpc_copy_source.h"
#include "php_globals.h"
#include "php_ini.h"
#include "SAPI.h"
#include "ext/standard/info.h"
#include "ext/standard/file.h"
#include "ext/standard/md5.h"
#include "Zend/zend_vm_opcodes.h"

/* {{{ PHP_FUNCTION declarations */
PHP_FUNCTION(lpc_cache_info);
PHP_FUNCTION(lpc_clear_cache);
PHP_FUNCTION(lpc_compile_file);
/* }}} */

/* {{{ ZEND_DECLARE_MODULE_GLOBALS(lpc) */
ZEND_DECLARE_MODULE_GLOBALS(lpc)
/* }}} */

/* {{{ lpc_reserved_offset 
       a true global: fixed LPC-specific offset used to allocate opcode flags in the op_array*/
int lpc_reserved_offset;
/* }}} */


/* {{{ PHP_INI 
   Register INI entries.  Note the the SYSTEM / PERDIR split is subject to review */

#define std_ini_bool(k,d,s,a,v)  STD_PHP_INI_BOOLEAN("lpc." k,d,s,a,v,zend_lpc_globals, lpc_globals) 
#define std_ini_entry(k,d,s,a,v) STD_PHP_INI_ENTRY("lpc." k,d,s,a,v,zend_lpc_globals, lpc_globals) 
#define perdir_ini_entry(k,d) PHP_INI_ENTRY("lpc." k,d,PHP_INI_PERDIR,NULL) 
PHP_INI_BEGIN()
std_ini_bool( "cache_by_default",           "1", PHP_INI_ALL,    OnUpdateBool,   cache_by_default)
std_ini_entry("file_update_protection",     "2", PHP_INI_SYSTEM, OnUpdateLong,   file_update_protection)
perdir_ini_entry("enabled",                 "1")
perdir_ini_entry("cache_pattern",            "")
perdir_ini_entry("cache_replacement",        "")
perdir_ini_entry("max_file_size",          "1M")
perdir_ini_entry("stat_percentage",         "0")
perdir_ini_entry("clear_cookie",    (char*)NULL)
perdir_ini_entry("clear_parameter", (char*)NULL)
perdir_ini_entry("filter",                   "")
perdir_ini_entry("resolve_paths",           "1")
perdir_ini_entry("debug_flags",             "0")
perdir_ini_entry("compression",             "1")
perdir_ini_entry("reuse_serial_buffer",     "1")
perdir_ini_entry("storage_quantum",      "128K")
PHP_INI_END()
/* }}} */

/* {{{ PHP_MINFO_FUNCTION(lpc) */
static PHP_MINFO_FUNCTION(lpc)
{ENTER(PHP_MINFO_FUNCTION)
    lpc_request_context_t *rc=LPCG(request_context);
    char buf[100];
#define info_convert(f,v) snprintf(buf, sizeof(buf)-1, f, LPCG(v)) 
#define info_convert_rc(f,v) snprintf(buf, sizeof(buf)-1, f, rc->v) 
    int i;

    php_info_print_table_start();
    php_info_print_table_header(2, "LPC Support", LPCG(enabled) ? "enabled" : "disabled");
    php_info_print_table_row(2, "Version", PHP_LPC_VERSION);
#ifdef LPC_DEBUG
    php_info_print_table_row(2, "LPC Debugging", "Enabled");
#else
    php_info_print_table_row(2, "LPC Debugging", "Disabled");
#endif
    php_info_print_table_row(2, "LPC Revision", "$Revision: 307215 $");
    php_info_print_table_row(2, "LPC Build Date", __DATE__ " " __TIME__);
    info_convert("%lu", cache_by_default);
    php_info_print_table_row(2, "Cache by default", buf);
    if (rc) {   
        php_info_print_table_row(2, "Filter", rc->filter);
        php_info_print_table_row(2, "Cache pattern", rc->cachedb_pattern);
        php_info_print_table_row(2, "Cache replacement", rc->cachedb_replacement);
    }
    info_convert("%lu", file_update_protection);
    php_info_print_table_row(2, "File update protection",buf);
    info_convert("%lu", enable_cli);
    php_info_print_table_row(2, "Enable CLI",buf);
    info_convert("%lu", max_file_size);
    php_info_print_table_row(2, "Max file size",buf);
    info_convert("%lu", fpstat);
    php_info_print_table_row(2, "Stat percentage",buf);
    info_convert("%lu", clear_cookie);
    php_info_print_table_row(2, "Clear cookie",buf);
    info_convert("%lu", clear_parameter);
    php_info_print_table_row(2, "Clear parameter",buf);
    info_convert("%lu", resolve_paths);
    php_info_print_table_row(2, "Resolve paths",buf);
    info_convert("%u", debug_flags);
    php_info_print_table_row(2, "Debug flags",buf);
    info_convert("%u", compression_algo);
    php_info_print_table_row(2, "Compression",buf);
    info_convert("%u", reuse_serial_buffer);
    php_info_print_table_row(2, "Reuse serial buffer",buf);
    info_convert("%u", storage_quantum);
    php_info_print_table_row(2, "Storage quantum",buf);
    php_info_print_table_end();
    DISPLAY_INI_ENTRIES();
}
/* }}} */

/* {{{ PHP_GINIT_FUNCTION(lpc) 
       Global CTOR for LPC  */
static PHP_GINIT_FUNCTION(lpc)
{ENTER(PHP_GINIT_FUNCTION)
   /* Since the default scope of globals is now the request, the CTOR simply
    * zero-fills the global structure and any non-zero elements are assigned
    * in MINIT or RINIT as appropriate.
    */  
    memset(lpc_globals, 0, sizeof(zend_lpc_globals));
}
/* }}} */

/* {{{ PHP_GSHUTDOWN_FUNCTION(lpc)
       Global DTOR for LPC  */
static PHP_GSHUTDOWN_FUNCTION(lpc)
{ENTER(PHP_GSHUTDOWN_FUNCTION)
   /* As for the CTOR, the DTOR functions are normally invoked as part of M/RSHUTDOWN.
    * However, as a safety net the DTOR does a safe cleanup of any of the following 
    * LPCG allocated elements */ 
#if 0
    lpc_cache_t* lpc_cache;       /* the global compiler cache */
    char *clear_cookie;           /* Name of Cookie which will force a cache clear */
    char *clear_parameter;        /* Name of Request parameter which will force a cache clear */
#endif

    /* the rest of the globals are cleaned up in lpc_module_shutdown() */
}
/* }}} */

#define OPCODE_TABLE_SIZE 25*LPC_MAX_OPCODE+26
opcode_handler_t *lpc_old_opcode_handler_ptr = NULL;
static opcode_handler_t opcode_handlers[OPCODE_TABLE_SIZE];

/* {{{ PHP_MINIT_FUNCTION(lpc) */
static PHP_MINIT_FUNCTION(lpc)
{
    zend_extension dummy_ext;
    int            i;
    lpc_reserved_offset     = zend_get_resource_handle(&dummy_ext); 

    LPCG(cache_by_default)  = 1;
    LPCG(canonicalize)      = 1;
    LPCG(sapi_request_time) = (time_t) sapi_get_request_time(TSRMLS_C);

    REGISTER_INI_ENTRIES();

    if (INI_INT("lpc.enabled") && (!LPCG(initialized))) {
        static int hook =1;
        lpc_module_init(module_number TSRMLS_CC);
        /*
         * Intercept all ZEND_INCLUDE_OR_EVAL instructions with LPCs own handler.  This copy / 
         * mod / overwrite pointer blows away 30K, but is atomic and avoids patching an internal
         * static constant Zend datastructure.
         */ 
        memcpy(opcode_handlers, zend_opcode_handlers, sizeof(opcode_handlers));
        for(i = 0; i < 25; i++) {
            if ((i/5) != _LPC_UNUSED_CODE) { /* op1 must be specified so skip unused */
                opcode_handlers[(ZEND_INCLUDE_OR_EVAL*25) + i] = lpc_include_or_eval_handler; 
            }
        }
        lpc_old_opcode_handler_ptr = zend_opcode_handlers;
        zend_opcode_handlers   = opcode_handlers;
    }

    return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION(lpc) */
static PHP_MSHUTDOWN_FUNCTION(lpc)
{
    if(LPCG(enabled)) {
        lpc_module_shutdown(TSRMLS_C);
    }
    UNREGISTER_INI_ENTRIES();
    if (!lpc_old_opcode_handler_ptr) {
        zend_opcode_handlers = lpc_old_opcode_handler_ptr;
    }
    return SUCCESS;
}
/* }}} */

/* {{{ PHP_RINIT_FUNCTION(lpc) */
static PHP_RINIT_FUNCTION(lpc)
{ENTER(PHP_RINIT_FUNCTION)

    LPCG(enabled) = lpc_request_init(TSRMLS_C);

    return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION(lpc) */
static PHP_RSHUTDOWN_FUNCTION(lpc)
{ENTER(PHP_RSHUTDOWN_FUNCTION)

    IF_DEBUG(COUNTS) {
        ENTER(DUMP);     /* Print out function summary counts */
    }
   /*
    * Since errors can flip the enabled state off, the request shutdown is always executed whether
    * or not LPC is enabled.  Since all relevant pointers are initialised to NULL, a simple non-zero
    * test prevents cleanup actions being taken when not needed.
    */
    lpc_request_shutdown(TSRMLS_C);
    return SUCCESS;
}
/* }}} */

/* {{{ proto array lpc_cache_info([string type [, bool limited]]) */
PHP_FUNCTION(lpc_cache_info)
{ENTER(PHP-lpc_cache_info)
    zval* info;
    char *cache_type;
    int ct_len;
    zend_bool limited = 0;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|sb", &cache_type, &ct_len, &limited) == FAILURE) {
        return;
    }

    info = lpc_cache_info(limited TSRMLS_CC);

    if(!info) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "No LPC info available.  Perhaps LPC is not enabled? Check lpc.enabled in your ini file");
        RETURN_FALSE;
    }

    RETURN_ZVAL(info, 0, 1);

}
/* }}} */

/* {{{ proto void lpc_clear_cache() */
PHP_FUNCTION(lpc_clear_cache)
{ENTER(PHP-lpc_clear_cache)
    LPCG(force_cache_delete) = 1;
    RETURN_TRUE;
}
/* }}} */    

/* {{{ proto mixed lpc_compile_file(mixed filenames [, bool atomic])
 */
PHP_FUNCTION(lpc_compile_file) 
{ENTER(PHP-lpc_compile_file) 
    zval *file;
    zend_file_handle file_handle;
    zend_op_array *op_array;
//// get rid of this
    char** filters = NULL;
    zend_bool cache_by_default = 1;
    HashTable cg_function_table, cg_class_table;
    HashTable *cg_orig_function_table, *cg_orig_class_table, *eg_orig_function_table, *eg_orig_class_table;
    zend_op_array **op_arrays;
    time_t t;
    zval **hentry;
    HashPosition hpos;
    int i=0, c=0;
    int *rval=NULL;
    int count=0;
    zend_bool atomic=1;
    zend_execute_data *orig_current_execute_data;
    int atomic_fail;

    if(!LPCG(enabled)) RETURN_FALSE;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z|b", &file, &atomic) == FAILURE) {
        return;
    }

    if (Z_TYPE_P(file) != IS_STRING) {
        lpc_warning("lpc_compile_file argument must be a string" TSRMLS_CC);
        RETURN_FALSE;
    }

    /* Replace function/class tables to avoid namespace conflicts */
    zend_hash_init_ex(&cg_function_table, 100, NULL, ZEND_FUNCTION_DTOR, 1, 0);
    cg_orig_function_table = CG(function_table);
    CG(function_table) = &cg_function_table;
    zend_hash_init_ex(&cg_class_table, 10, NULL, ZEND_CLASS_DTOR, 1, 0);
    cg_orig_class_table = CG(class_table);
    CG(class_table) = &cg_class_table;
    eg_orig_function_table = EG(function_table);
    EG(function_table) = CG(function_table);
    eg_orig_class_table = EG(class_table);
    EG(class_table) = CG(class_table);
    LPCG(force_file_update) = 1;

    /* Compile the file(s), loading it into the cache */
    if (Z_TYPE_P(file) == IS_STRING) {
        file_handle.type = ZEND_HANDLE_FILENAME;
        file_handle.filename = Z_STRVAL_P(file);
        file_handle.free_filename = 0;
        file_handle.opened_path = NULL;

        orig_current_execute_data = EG(current_execute_data);
        zend_try {
            op_array = zend_compile_file(&file_handle, ZEND_INCLUDE TSRMLS_CC);
        } zend_catch {
            EG(current_execute_data) = orig_current_execute_data;
            EG(in_execution) = 1;
            CG(unclean_shutdown) = 0;
            lpc_warning("Error compiling %s in lpc_compile_file." TSRMLS_CC, file_handle.filename);
            op_array = NULL;
        } zend_end_try();
        if(op_array != NULL) {
            /* Free up everything */
            destroy_op_array(op_array TSRMLS_CC);
            efree(op_array);
            RETVAL_TRUE;
        } else {
            RETVAL_FALSE;
        }
        zend_destroy_file_handle(&file_handle TSRMLS_CC);
    }

    /* Return class/function tables to previous states, destroy temp tables */
    LPCG(force_file_update) = 0;
    CG(function_table) = cg_orig_function_table;
    zend_hash_destroy(&cg_function_table);
    CG(class_table) = cg_orig_class_table;
    zend_hash_destroy(&cg_class_table);
    EG(function_table) = eg_orig_function_table;
    EG(class_table) = eg_orig_class_table;

}
/* }}} */


/* {{{ arginfo */
#if (PHP_MAJOR_VERSION >= 6 || (PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION >= 3))
# define PHP_LPC_ARGINFO
#else
# define PHP_LPC_ARGINFO static
#endif

PHP_LPC_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_lpc_clear_cache, 0, 0, 0)
ZEND_END_ARG_INFO()

PHP_LPC_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_lpc_cache_info, 0, 0, 0)
    ZEND_ARG_INFO(0, type)
    ZEND_ARG_INFO(0, limited)
ZEND_END_ARG_INFO()

PHP_LPC_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_lpc_compile_file, 0, 0, 1)
    ZEND_ARG_INFO(0, filenames)
    ZEND_ARG_INFO(0, atomic)
ZEND_END_ARG_INFO()

/* }}} */

/* {{{ lpc_functions[] */
zend_function_entry lpc_functions[] = {
    PHP_FE(lpc_cache_info,          arginfo_lpc_cache_info)
    PHP_FE(lpc_clear_cache,         arginfo_lpc_clear_cache)
    PHP_FE(lpc_compile_file,        arginfo_lpc_compile_file)
    {NULL, NULL, NULL}
};
/* }}} */

/* {{{ module definition structure */
zend_module_entry lpc_module_entry = {
    STANDARD_MODULE_HEADER,
    "lpc",                       /* extension name */
    lpc_functions,               /* function list */
    PHP_MINIT(lpc),              /* process startup */
    PHP_MSHUTDOWN(lpc),          /* process shutdown */
    PHP_RINIT(lpc),              /* request startup */
    PHP_RSHUTDOWN(lpc),          /* request shutdown */
    PHP_MINFO(lpc),              /* extension info */
    NO_VERSION_YET,              /* extension version */
    PHP_MODULE_GLOBALS(lpc),     /* globals descriptor */
    PHP_GINIT(lpc),              /* globals ctor */
    PHP_GSHUTDOWN(lpc),          /* globals dtor */
    NULL,                        /* No post deactivate */
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_LPC
ZEND_GET_MODULE(lpc)
#endif
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
