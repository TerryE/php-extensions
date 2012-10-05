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
  |          Rasmus Lerdorf <rasmus@php.net>                             |
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Community Connect Inc. in 2002
   and revised in 2005 by Yahoo! Inc. to add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.

 */

/* $Id: php_lpc.c 307215 2011-01-07 09:54:00Z gopalv $ */

#include "lpc_zend.h"
#include "lpc_cache.h"
#include "lpc_main.h"
#include "lpc_bin.h"
#include "php_globals.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/standard/file.h"
#ifdef HAVE_SYS_FILE_H
#include <sys/file.h>
#endif
#include "SAPI.h"
#include "php_lpc.h"
#include "ext/standard/md5.h"

/* {{{ PHP_FUNCTION declarations */
PHP_FUNCTION(lpc_cache_info);
PHP_FUNCTION(lpc_clear_cache);
PHP_FUNCTION(lpc_compile_file);
PHP_FUNCTION(lpc_bin_dump);
PHP_FUNCTION(lpc_bin_load);
/* }}} */

/* {{{ ZEND_DECLARE_MODULE_GLOBALS(lpc) */
ZEND_DECLARE_MODULE_GLOBALS(lpc)

static void php_lpc_init_globals(zend_lpc_globals* lpc_globals TSRMLS_DC)
{
    lpc_globals->filters = NULL;
    lpc_globals->compiled_filters = NULL;
    lpc_globals->initialized = 0;
    lpc_globals->cache_stack = lpc_stack_create(0 TSRMLS_CC);
    lpc_globals->cache_by_default = 1;
    lpc_globals->fpstat = 1;
    lpc_globals->canonicalize = 1;
    lpc_globals->stat_ctime = 0;
    lpc_globals->report_autofilter = 0;
    lpc_globals->include_once = 0;
    lpc_globals->lpc_optimize_function = NULL;
    memset(&lpc_globals->copied_zvals, 0, sizeof(HashTable));
    lpc_globals->force_file_update = 0;
    lpc_globals->coredump_unmap = 0;
    lpc_globals->use_request_time = 1;
    lpc_globals->lazy_class_table = NULL;
    lpc_globals->lazy_function_table = NULL;
    lpc_globals->serializer_name = NULL;
    lpc_globals->serializer = NULL;
    lpc_globals->lpc_cache = NULL;
    lpc_globals->clear_cookie = NULL;
    lpc_globals->clear_parameter = NULL;
}

static void php_lpc_shutdown_globals(zend_lpc_globals* lpc_globals TSRMLS_DC)
{
    /* deallocate the ignore patterns */
    if (lpc_globals->filters != NULL) {
        int i;
        for (i=0; lpc_globals->filters[i] != NULL; i++) {
            lpc_efree(lpc_globals->filters[i] TSRMLS_CC);
        }
        lpc_efree(lpc_globals->filters TSRMLS_CC);
    }

    /* the stack should be empty */
    assert(lpc_stack_size(lpc_globals->cache_stack) == 0);

    /* lpc cleanup */
    lpc_stack_destroy(lpc_globals->cache_stack TSRMLS_CC);

    /* the rest of the globals are cleaned up in lpc_module_shutdown() */
}

static long lpc_atol(const char *str, int str_len)
{
#if PHP_MAJOR_VERSION >= 6 || PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION >= 3
    return zend_atol(str, str_len);
#else
    /* Re-implement zend_atol() for 5.2.x */
    long retval;

    if (!str_len) {
        str_len = strlen(str);
    }

    retval = strtol(str, NULL, 0);

    if (str_len > 0) {
        switch (str[str_len - 1]) {
            case 'g':
            case 'G':
                retval *= 1024;
                /* break intentionally missing */
            case 'm':
            case 'M':
                retval *= 1024;
                /* break intentionally missing */
            case 'k':
            case 'K':
                retval *= 1024;
                break;
        }
    }

    return retval;
#endif
}

/* }}} */

/* {{{ PHP_INI */

static PHP_INI_MH(OnUpdate_filters) /* {{{ */
{
    LPCG(filters) = lpc_tokenize(new_value, ',' TSRMLS_CC);
    return SUCCESS;
}

#define std_ini_bool(k,d,s,a,v)  STD_PHP_INI_BOOLEAN("lpc." k,d,s,a,v,zend_lpc_globals, lpc_globals) 
#define std_ini_entry(k,d,s,a,v) STD_PHP_INI_ENTRY("lpc." k,d,s,a,v,zend_lpc_globals, lpc_globals) 
PHP_INI_BEGIN()
std_ini_bool( "enabled",                 "1", PHP_INI_SYSTEM, OnUpdateBool,   enabled)
std_ini_bool( "cache_by_default",        "1", PHP_INI_ALL,    OnUpdateBool,   cache_by_default)
std_ini_bool( "enable_cli",              "0", PHP_INI_SYSTEM, OnUpdateBool,   enable_cli)
std_ini_entry("file_update_protection",  "2", PHP_INI_SYSTEM, OnUpdateLong,   file_update_protection)
std_ini_entry("max_file_size",          "1M", PHP_INI_SYSTEM, OnUpdateLong,   max_file_size)
std_ini_entry("stat_percentage",         "0", PHP_INI_SYSTEM, OnUpdateLong,   fpstat)
std_ini_entry("clear_cookie",    (char*)NULL, PHP_INI_SYSTEM, OnUpdateString, clear_cookie)
std_ini_entry("clear_parameter", (char*)NULL, PHP_INI_SYSTEM, OnUpdateString, clear_parameter)
PHP_INI_ENTRY("filters",                NULL, PHP_INI_SYSTEM, OnUpdate_filters)
PHP_INI_END()

/* }}} */

/* {{{ PHP_MINFO_FUNCTION(lpc) */
static PHP_MINFO_FUNCTION(lpc)
{
   	char buf[100];
#define info_convert(f,v) snprintf(buf, sizeof(buf), f, LPCG(v)) 
    int i;

    php_info_print_table_start();
    php_info_print_table_header(2, "LPC Support", LPCG(enabled) ? "enabled" : "disabled");
    php_info_print_table_row(2, "Version", PHP_LPC_VERSION);
#ifdef __DEBUG_LPC__
    php_info_print_table_row(2, "LPC Debugging", "Enabled");
#else
    php_info_print_table_row(2, "LPC Debugging", "Disabled");
#endif
    php_info_print_table_row(2, "LPC Revision", "$Revision: 307215 $");
    php_info_print_table_row(2, "LPC Build Date", __DATE__ " " __TIME__);
	info_convert("%lu", cache_by_default);
	php_info_print_table_row(2, "Cache by default", buf);    
//	php_info_print_table_row(2, "Filters",APGC(filters));
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
    php_info_print_table_end();
    DISPLAY_INI_ENTRIES();
}
/* }}} */

/* {{{ PHP_MINIT_FUNCTION(lpc) */
static PHP_MINIT_FUNCTION(lpc)
{
    ZEND_INIT_MODULE_GLOBALS(lpc, php_lpc_init_globals, php_lpc_shutdown_globals);

    REGISTER_INI_ENTRIES();

    if (LPCG(enabled)) {
        if(LPCG(initialized)) {
            lpc_process_init(module_number TSRMLS_CC);
        } else {
            lpc_module_init(module_number TSRMLS_CC);
            lpc_zend_init(TSRMLS_C);
            lpc_process_init(module_number TSRMLS_CC);
        }

        zend_register_long_constant("LPC_BIN_VERIFY_MD5", sizeof("LPC_BIN_VERIFY_MD5"), LPC_BIN_VERIFY_MD5, (CONST_CS | CONST_PERSISTENT), module_number TSRMLS_CC);
        zend_register_long_constant("LPC_BIN_VERIFY_CRC32", sizeof("LPC_BIN_VERIFY_CRC32"), LPC_BIN_VERIFY_CRC32, (CONST_CS | CONST_PERSISTENT), module_number TSRMLS_CC);
    }

    return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION(lpc) */
static PHP_MSHUTDOWN_FUNCTION(lpc)
{
    if(LPCG(enabled)) {
        lpc_process_shutdown(TSRMLS_C);
        lpc_zend_shutdown(TSRMLS_C);
        lpc_module_shutdown(TSRMLS_C);
#ifndef ZTS
        php_lpc_shutdown_globals(&lpc_globals);
#endif
    }
#ifdef ZTS
    ts_free_id(lpc_globals_id);
#endif
    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}
/* }}} */

/* {{{ PHP_RINIT_FUNCTION(lpc) */
static PHP_RINIT_FUNCTION(lpc)
{
    if(LPCG(enabled)) {
        lpc_request_init(TSRMLS_C);

    }
    return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION(lpc) */
static PHP_RSHUTDOWN_FUNCTION(lpc)
{
    if(LPCG(enabled)) {
        lpc_request_shutdown(TSRMLS_C);
    }
    return SUCCESS;
}
/* }}} */

/* {{{ proto array lpc_cache_info([string type [, bool limited]]) */
PHP_FUNCTION(lpc_cache_info)
{
    zval* info;
    char *cache_type;
    int ct_len;
    zend_bool limited = 0;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|sb", &cache_type, &ct_len, &limited) == FAILURE) {
        return;
    }

    info = lpc_cache_info(LPCG(lpc_cache), limited TSRMLS_CC);

    if(!info) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "No LPC info available.  Perhaps LPC is not enabled? Check lpc.enabled in your ini file");
        RETURN_FALSE;
    }

    RETURN_ZVAL(info, 0, 1);

}
/* }}} */

/* {{{ proto void lpc_clear_cache() */
PHP_FUNCTION(lpc_clear_cache)
{

	LPCG(force_cache_delete) = 1;
    RETURN_TRUE;
}
/* }}} */    

void *lpc_erealloc_wrapper(void *ptr, size_t size) {
    return _erealloc(ptr, size, 0 ZEND_FILE_LINE_CC ZEND_FILE_LINE_EMPTY_CC);
}

/* {{{ proto mixed lpc_compile_file(mixed filenames [, bool atomic])
 */
PHP_FUNCTION(lpc_compile_file) {
    zval *file;
    zend_file_handle file_handle;
    zend_op_array *op_array;
    char** filters = NULL;
    zend_bool cache_by_default = 1;
    HashTable cg_function_table, cg_class_table;
    HashTable *cg_orig_function_table, *cg_orig_class_table, *eg_orig_function_table, *eg_orig_class_table;
    lpc_cache_entry_t** cache_entries;
    lpc_cache_key_t* keys;
    zend_op_array **op_arrays;
    time_t t;
    zval **hentry;
    HashPosition hpos;
    int i=0, c=0;
    int *rval=NULL;
    int count=0;
    zend_bool atomic=1;
    lpc_context_t ctxt = {0,};
    zend_execute_data *orig_current_execute_data;
    int atomic_fail;

    if(!LPCG(enabled)) RETURN_FALSE;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z|b", &file, &atomic) == FAILURE) {
        return;
    }

    if (Z_TYPE_P(file) != IS_ARRAY && Z_TYPE_P(file) != IS_STRING) {
        lpc_warning("lpc_compile_file argument must be a string or an array of strings" TSRMLS_CC);
        RETURN_FALSE;
    }

    LPCG(current_cache) = LPCG(lpc_cache);

    /* reset filters and cache_by_default */
    filters = LPCG(filters);
    LPCG(filters) = NULL;

    cache_by_default = LPCG(cache_by_default);
    LPCG(cache_by_default) = 1;

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

    } else { /* IS_ARRAY */

        array_init(return_value);

        t = lpc_time();

        op_arrays = ecalloc(Z_ARRVAL_P(file)->nNumOfElements, sizeof(zend_op_array*));
        cache_entries = ecalloc(Z_ARRVAL_P(file)->nNumOfElements, sizeof(lpc_cache_entry_t*));
        keys = ecalloc(Z_ARRVAL_P(file)->nNumOfElements, sizeof(lpc_cache_key_t));
        zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(file), &hpos);
        while(zend_hash_get_current_data_ex(Z_ARRVAL_P(file), (void**)&hentry, &hpos) == SUCCESS) {
            if (Z_TYPE_PP(hentry) != IS_STRING) {
                lpc_warning("lpc_compile_file array values must be strings, aborting." TSRMLS_CC);
                break;
            }
            file_handle.type = ZEND_HANDLE_FILENAME;
            file_handle.filename = Z_STRVAL_PP(hentry);
            file_handle.free_filename = 0;
            file_handle.opened_path = NULL;

            if (!lpc_cache_make_file_key(&(keys[i]), file_handle.filename, PG(include_path), t TSRMLS_CC)) {
                add_assoc_long(return_value, Z_STRVAL_PP(hentry), -1);  /* -1: compilation error */
                lpc_warning("Error compiling %s in lpc_compile_file." TSRMLS_CC, file_handle.filename);
                break;
            }

            if (keys[i].type == LPC_CACHE_KEY_FPFILE) {
                keys[i].data.fpfile.fullpath = estrndup(keys[i].data.fpfile.fullpath, keys[i].data.fpfile.fullpath_len);
            }

            orig_current_execute_data = EG(current_execute_data);
            zend_try {
                if (lpc_compile_cache_entry(keys[i], &file_handle, ZEND_INCLUDE, t, &op_arrays[i], &cache_entries[i] TSRMLS_CC) != SUCCESS) {
                    op_arrays[i] = NULL;
                    cache_entries[i] = NULL;
                    add_assoc_long(return_value, Z_STRVAL_PP(hentry), -2);  /* -2: input or cache insertion error */
                    lpc_warning("Error compiling %s in lpc_compile_file." TSRMLS_CC, file_handle.filename);
                }
            } zend_catch {
                EG(current_execute_data) = orig_current_execute_data;
                EG(in_execution) = 1;
                CG(unclean_shutdown) = 0;
                op_arrays[i] = NULL;
                cache_entries[i] = NULL;
                add_assoc_long(return_value, Z_STRVAL_PP(hentry), -1);  /* -1: compilation error */
                lpc_warning("Error compiling %s in lpc_compile_file." TSRMLS_CC, file_handle.filename);
            } zend_end_try();

            zend_destroy_file_handle(&file_handle TSRMLS_CC);
            if(op_arrays[i] != NULL) {
                count++;
            }

            /* clean out the function/class tables */
            zend_hash_clean(&cg_function_table);
            zend_hash_clean(&cg_class_table);

            zend_hash_move_forward_ex(Z_ARRVAL_P(file), &hpos);
            i++;
        }

        /* atomically update the cache if no errors or not atomic */
        ctxt.copy = LPC_COPY_IN_OPCODE;
        ctxt.force_update = 1;
        if (count == i || !atomic) {
            rval = lpc_cache_insert_mult(LPCG(lpc_cache), keys, cache_entries, &ctxt, t, i TSRMLS_CC);
            atomic_fail = 0;
        } else {
            atomic_fail = 1;
        }

        zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(file), &hpos);
        for(c=0; c < i; c++) {
            zend_hash_get_current_data_ex(Z_ARRVAL_P(file), (void**)&hentry, &hpos);
            if (rval && rval[c] != 1) {
                add_assoc_long(return_value, Z_STRVAL_PP(hentry), -2);  /* -2: input or cache insertion error */
                if (cache_entries[c]) {
                    lpc_pool_destroy(cache_entries[c]->pool TSRMLS_CC);
                }
            }
            if (op_arrays[c]) {
                destroy_op_array(op_arrays[c] TSRMLS_CC);
                efree(op_arrays[c]);
            }
            if (atomic_fail && cache_entries[c]) {
                lpc_pool_destroy(cache_entries[c]->pool TSRMLS_CC);
            }
            if (keys[c].type == LPC_CACHE_KEY_FPFILE) {
                efree((void*)keys[c].data.fpfile.fullpath);
            }
            zend_hash_move_forward_ex(Z_ARRVAL_P(file), &hpos);
        }
        efree(op_arrays);
        efree(keys);
        efree(cache_entries);
        if (rval) {
            efree(rval);
        }

    }

    /* Return class/function tables to previous states, destroy temp tables */
    LPCG(force_file_update) = 0;
    CG(function_table) = cg_orig_function_table;
    zend_hash_destroy(&cg_function_table);
    CG(class_table) = cg_orig_class_table;
    zend_hash_destroy(&cg_class_table);
    EG(function_table) = eg_orig_function_table;
    EG(class_table) = eg_orig_class_table;

    /* Restore global settings */
    LPCG(filters) = filters;
    LPCG(cache_by_default) = cache_by_default;

    LPCG(current_cache) = NULL;
}
/* }}} */

////  Will ultimately be removed

/* {{{ proto mixed lpc_bin_dump([array files)
    Returns a binary dump of the given files from the LPC cache.
    A NULL for files signals a dump of every entry, while array() will dump nothing.
 */
PHP_FUNCTION(lpc_bin_dump) {

    zval *z_files = NULL;
    HashTable *h_files;
    lpc_bd_t *bd;

    if(!LPCG(enabled)) {
        lpc_warning("LPC is not enabled, lpc_bin_dump not available." TSRMLS_CC);
        RETURN_FALSE;
    }

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|a!a!", &z_files) == FAILURE) {
        return;
    }

    h_files = z_files ? Z_ARRVAL_P(z_files) : NULL;
    bd = lpc_bin_dump(h_files TSRMLS_CC);
    if(bd) {
        RETVAL_STRINGL((char*)bd, bd->size-1, 0);
    } else {
        lpc_error("Unknown error encountered during lpc_bin_dump." TSRMLS_CC);
        RETVAL_NULL();
    }

    return;
}

/* {{{ proto mixed lpc_bin_load(string data, [int flags])
    Load the given binary dump into the LPC file cache.
 */
PHP_FUNCTION(lpc_bin_load) {

    int data_len;
    char *data;
    long flags = 0;

    if(!LPCG(enabled)) {
        lpc_warning("LPC is not enabled, lpc_bin_load not available." TSRMLS_CC);
        RETURN_FALSE;
    }

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|l", &data, &data_len, &flags) == FAILURE) {
        return;
    }

    if(!data_len || data_len != ((lpc_bd_t*)data)->size -1) {
        lpc_error("lpc_bin_load string argument does not appear to be a valid LPC binary dump due to size (%d vs expected %d)." TSRMLS_CC, data_len, ((lpc_bd_t*)data)->size -1);
        RETURN_FALSE;
    }

    lpc_bin_load((lpc_bd_t*)data, (int)flags TSRMLS_CC);

    RETURN_TRUE;
}

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

PHP_LPC_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_lpc_bin_dump, 0, 0, 0)
    ZEND_ARG_INFO(0, files)
ZEND_END_ARG_INFO()

PHP_LPC_ARGINFO
ZEND_BEGIN_ARG_INFO_EX(arginfo_lpc_bin_load, 0, 0, 1)
    ZEND_ARG_INFO(0, data)
    ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()
/* }}} */

/* {{{ lpc_functions[] */
zend_function_entry lpc_functions[] = {
    PHP_FE(lpc_cache_info,          arginfo_lpc_cache_info)
    PHP_FE(lpc_clear_cache,         arginfo_lpc_clear_cache)
    PHP_FE(lpc_compile_file,        arginfo_lpc_compile_file)
    PHP_FE(lpc_bin_dump,            arginfo_lpc_bin_dump)
    PHP_FE(lpc_bin_load,            arginfo_lpc_bin_load)
    {NULL, NULL, NULL}
};
/* }}} */

/* {{{ module definition structure */

zend_module_entry lpc_module_entry = {
    STANDARD_MODULE_HEADER,
    "lpc",
    lpc_functions,
    PHP_MINIT(lpc),
    PHP_MSHUTDOWN(lpc),
    PHP_RINIT(lpc),
    PHP_RSHUTDOWN(lpc),
    PHP_MINFO(lpc),
    PHP_LPC_VERSION,
    STANDARD_MODULE_PROPERTIES
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
