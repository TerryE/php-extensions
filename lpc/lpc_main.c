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

/* $Id: lpc_main.c 307259 2011-01-08 12:05:24Z gopalv $ */

#include "lpc_php.h"
#include "lpc_main.h"
#include "lpc.h"
#include "lpc_lock.h"
#include "lpc_cache.h"
#include "lpc_compile.h"
#include "lpc_globals.h"
#include "lpc_sma.h"
#include "lpc_stack.h"
#include "lpc_zend.h"
#include "lpc_pool.h"
#include "lpc_string.h"
#include "SAPI.h"
#include "php_scandir.h"
#include "ext/standard/php_var.h"
#include "ext/standard/md5.h"

#define LPC_MAX_SERIALIZERS 16

/* {{{ module variables */

/* pointer to the original Zend engine compile_file function */
typedef zend_op_array* (zend_compile_t)(zend_file_handle*, int TSRMLS_DC);
static zend_compile_t *old_compile_file;
static lpc_serializer_t lpc_serializers[LPC_MAX_SERIALIZERS] = {{0,}};

/* }}} */

/* {{{ get/set old_compile_file (to interact with other extensions that need the compile hook) */
static zend_compile_t* set_compile_hook(zend_compile_t *ptr)
{
    zend_compile_t *retval = old_compile_file;

    if (ptr != NULL) old_compile_file = ptr;
    return retval;
}
/* }}} */

/* {{{ install_function */
static int install_function(lpc_function_t fn, lpc_context_t* ctxt, int lazy TSRMLS_DC)
{
    int status;

#if LPC_HAVE_LOOKUP_HOOKS
    if(lazy && fn.name[0] != '\0' && strncmp(fn.name, "__autoload", fn.name_len) != 0) {
        status = zend_hash_add(LPCG(lazy_function_table),
                              fn.name,
                              fn.name_len+1,
                              &fn,
                              sizeof(lpc_function_t),
                              NULL);
#else
    if(0) {
#endif
    } else {
        zend_function *func = lpc_copy_function_for_execution(fn.function, ctxt TSRMLS_CC);
        status = zend_hash_add(EG(function_table),
                              fn.name,
                              fn.name_len+1,
                              func,
                              sizeof(fn.function[0]),
                              NULL);
        efree(func);
    }

    if (status == FAILURE) {
        /* lpc_error("Cannot redeclare %s()" TSRMLS_CC, fn.name); */
    }

    return status;
}
/* }}} */

/* {{{ lpc_lookup_function_hook */
int lpc_lookup_function_hook(char *name, int len, ulong hash, zend_function **fe) {
    lpc_function_t *fn;
    int status = FAILURE;
    lpc_context_t ctxt = {0,};
    TSRMLS_FETCH();

    ctxt.pool = lpc_pool_create(LPC_UNPOOL, lpc_php_malloc, lpc_php_free, lpc_sma_protect, lpc_sma_unprotect TSRMLS_CC);
    ctxt.copy = LPC_COPY_OUT_OPCODE;

    if(zend_hash_quick_find(LPCG(lazy_function_table), name, len, hash, (void**)&fn) == SUCCESS) {
        *fe = lpc_copy_function_for_execution(fn->function, &ctxt TSRMLS_CC);
        status = zend_hash_add(EG(function_table),
                                  fn->name,
                                  fn->name_len+1,
                                  *fe,
                                  sizeof(zend_function),
                                  NULL);
    }

    return status;
}
/* }}} */

/* {{{ install_class */
static int install_class(lpc_class_t cl, lpc_context_t* ctxt, int lazy TSRMLS_DC)
{
    zend_class_entry* class_entry = cl.class_entry;
    zend_class_entry* parent = NULL;
    int status;
    zend_class_entry** allocated_ce = NULL;

    /* Special case for mangled names. Mangled names are unique to a file.
     * There is no way two classes with the same mangled name will occur,
     * unless a file is included twice. And if in case, a file is included
     * twice, all mangled name conflicts can be ignored and the class redeclaration
     * error may be deferred till runtime of the corresponding DECLARE_CLASS
     * calls.
     */

    if(cl.name_len != 0 && cl.name[0] == '\0') {
        if(zend_hash_exists(CG(class_table), cl.name, cl.name_len+1)) {
            return SUCCESS;
        }
    }

    if(lazy && cl.name_len != 0 && cl.name[0] != '\0') {
        status = zend_hash_add(LPCG(lazy_class_table),
                               cl.name,
                               cl.name_len+1,
                               &cl,
                               sizeof(lpc_class_t),
                               NULL);
        if(status == FAILURE) {
            zend_error(E_ERROR, "Cannot redeclare class %s", cl.name);
        }
        return status;
    }

    /*
     * XXX: We need to free this somewhere...
     */
    allocated_ce = lpc_php_malloc(sizeof(zend_class_entry*) TSRMLS_CC);

    if(!allocated_ce) {
        return FAILURE;
    }

    *allocated_ce =
    class_entry =
        lpc_copy_class_entry_for_execution(cl.class_entry, ctxt TSRMLS_CC);


    /* restore parent class pointer for compile-time inheritance */
    if (cl.parent_name != NULL) {
        zend_class_entry** parent_ptr = NULL;
        /*
         * __autoload brings in the old issues with mixed inheritance.
         * When a statically inherited class triggers autoload, it runs
         * afoul of a potential require_once "parent.php" in the previous 
         * line, which when executed provides the parent class, but right
         * now goes and hits __autoload which could fail. 
         * 
         * missing parent == re-compile. 
         *
         * whether __autoload is enabled or not, because __autoload errors
         * cause php to die.
         *
         * Aside: Do NOT pass *strlen(cl.parent_name)+1* because
         * zend_lookup_class_ex does it internally anyway!
         */
        status = zend_lookup_class_ex(cl.parent_name,
                                    strlen(cl.parent_name), 
#ifdef ZEND_ENGINE_2_4
                                    NULL,
#endif
                                    0,
                                    &parent_ptr TSRMLS_CC);
        if (status == FAILURE) {
            if(LPCG(report_autofilter)) {
                lpc_warning("Dynamic inheritance detected for class %s" TSRMLS_CC, cl.name);
            }
            class_entry->parent = NULL;
            return status;
        }
        else {
            parent = *parent_ptr;
            class_entry->parent = parent;
            zend_do_inheritance(class_entry, parent TSRMLS_CC);
        }


    }

    status = zend_hash_add(EG(class_table),
                           cl.name,
                           cl.name_len+1,
                           allocated_ce,
                           sizeof(zend_class_entry*),
                           NULL);

    if (status == FAILURE) {
        lpc_error("Cannot redeclare class %s" TSRMLS_CC, cl.name);
    }
    return status;
}
/* }}} */

/* {{{ lpc_lookup_class_hook */
int lpc_lookup_class_hook(char *name, int len, ulong hash, zend_class_entry ***ce) {

    lpc_class_t *cl;
    lpc_context_t ctxt = {0,};
    TSRMLS_FETCH();

    if(zend_is_compiling(TSRMLS_C)) { return FAILURE; }

    if(zend_hash_quick_find(LPCG(lazy_class_table), name, len, hash, (void**)&cl) == FAILURE) {
        return FAILURE;
    }

    ctxt.pool = lpc_pool_create(LPC_UNPOOL, lpc_php_malloc, lpc_php_free, lpc_sma_protect, lpc_sma_unprotect TSRMLS_CC);
    ctxt.copy = LPC_COPY_OUT_OPCODE;

    if(install_class(*cl, &ctxt, 0 TSRMLS_CC) == FAILURE) {
        lpc_warning("lpc_lookup_class_hook: could not install %s" TSRMLS_CC, name);
        return FAILURE;
    }

    if(zend_hash_quick_find(EG(class_table), name, len, hash, (void**)ce) == FAILURE) {
        lpc_warning("lpc_lookup_class_hook: known error trying to fetch class %s" TSRMLS_CC, name);
        return FAILURE;
    }

    return SUCCESS;

}
/* }}} */

/* {{{ uninstall_class */
static int uninstall_class(lpc_class_t cl TSRMLS_DC)
{
    int status;

    status = zend_hash_del(EG(class_table),
                           cl.name,
                           cl.name_len+1);
    if (status == FAILURE) {
        lpc_error("Cannot delete class %s" TSRMLS_CC, cl.name);
    }
    return status;
}
/* }}} */

/* {{{ copy_function_name (taken from zend_builtin_functions.c to ensure future compatibility with LPC) */
static int copy_function_name(lpc_function_t *pf TSRMLS_DC, int num_args, va_list args, zend_hash_key *hash_key)
{
    zval *internal_ar = va_arg(args, zval *),
         *user_ar     = va_arg(args, zval *);
    zend_function *func = pf->function;

    if (hash_key->nKeyLength == 0 || hash_key->arKey[0] == 0) {
        return 0;
    }

    if (func->type == ZEND_INTERNAL_FUNCTION) {
        add_next_index_stringl(internal_ar, hash_key->arKey, hash_key->nKeyLength-1, 1);
    } else if (func->type == ZEND_USER_FUNCTION) {
        add_next_index_stringl(user_ar, hash_key->arKey, hash_key->nKeyLength-1, 1);
    }

    return 0;
}

/* {{{ copy_class_or_interface_name (taken from zend_builtin_functions.c to ensure future compatibility with LPC) */
static int copy_class_or_interface_name(lpc_class_t *cl TSRMLS_DC, int num_args, va_list args, zend_hash_key *hash_key)
{
    zval *array = va_arg(args, zval *);
    zend_uint mask = va_arg(args, zend_uint);
    zend_uint comply = va_arg(args, zend_uint);
    zend_uint comply_mask = (comply)? mask:0;
    zend_class_entry *ce  = cl->class_entry;

    if ((hash_key->nKeyLength==0 || hash_key->arKey[0]!=0)
        && (comply_mask == (ce->ce_flags & mask))) {
        add_next_index_stringl(array, ce->name, ce->name_length, 1);
    }
    return ZEND_HASH_APPLY_KEEP;
}
/* }}} */

/* }}} */

/* {{{ lpc_defined_function_hook */
int lpc_defined_function_hook(zval *internal, zval *user) {
    TSRMLS_FETCH();
    zend_hash_apply_with_arguments(LPCG(lazy_function_table) 
#ifdef ZEND_ENGINE_2_3
    TSRMLS_CC
#endif
    ,(apply_func_args_t) copy_function_name, 2, internal, user);
  return 1;
}
/* }}} */

/* {{{ lpc_declared_class_hook */
int lpc_declared_class_hook(zval *classes, zend_uint mask, zend_uint comply) {
    TSRMLS_FETCH();
    zend_hash_apply_with_arguments(LPCG(lazy_class_table) 
#ifdef ZEND_ENGINE_2_3
    TSRMLS_CC
#endif
    , (apply_func_args_t) copy_class_or_interface_name, 3, classes, mask, comply);
  return 1;
}
/* }}} */

/* {{{ cached_compile */
static zend_op_array* cached_compile(zend_file_handle* h,
                                        int type,
                                        lpc_context_t* ctxt TSRMLS_DC)
{
    lpc_cache_entry_t* cache_entry;
    int i, ii;

    cache_entry = (lpc_cache_entry_t*) lpc_stack_top(LPCG(cache_stack));
    assert(cache_entry != NULL);

    if (cache_entry->data.file.classes) {
        int lazy_classes = LPCG(lazy_classes);
        for (i = 0; cache_entry->data.file.classes[i].class_entry != NULL; i++) {
            if(install_class(cache_entry->data.file.classes[i], ctxt, lazy_classes TSRMLS_CC) == FAILURE) {
                goto default_compile;
            }
        }
    }

    if (cache_entry->data.file.functions) {
        int lazy_functions = LPCG(lazy_functions);
        for (i = 0; cache_entry->data.file.functions[i].function != NULL; i++) {
            install_function(cache_entry->data.file.functions[i], ctxt, lazy_functions TSRMLS_CC);
        }
    }

    lpc_do_halt_compiler_register(cache_entry->data.file.filename, cache_entry->data.file.halt_offset TSRMLS_CC);


    return lpc_copy_op_array_for_execution(NULL, cache_entry->data.file.op_array, ctxt TSRMLS_CC);

default_compile:

    if(cache_entry->data.file.classes) {
        for(ii = 0; ii < i ; ii++) {
            uninstall_class(cache_entry->data.file.classes[ii] TSRMLS_CC);
        }
    }

    lpc_stack_pop(LPCG(cache_stack)); /* pop out cache_entry */

    lpc_cache_release(lpc_cache, cache_entry TSRMLS_CC);

    /* cannot free up cache data yet, it maybe in use */

    return NULL;
}
/* }}} */

/* {{{ lpc_compile_cache_entry  */
zend_bool lpc_compile_cache_entry(lpc_cache_key_t key, zend_file_handle* h, int type, time_t t, zend_op_array** op_array, lpc_cache_entry_t** cache_entry TSRMLS_DC) {
    int num_functions, num_classes;
    lpc_function_t* alloc_functions;
    zend_op_array* alloc_op_array;
    lpc_class_t* alloc_classes;
    char *path;
    lpc_context_t ctxt;

    /* remember how many functions and classes existed before compilation */
    num_functions = zend_hash_num_elements(CG(function_table));
    num_classes   = zend_hash_num_elements(CG(class_table));

    /* compile the file using the default compile function,  *
     * we set *op_array here so we return opcodes during     *
     * a failure.  We should not return prior to this line.  */
    *op_array = old_compile_file(h, type TSRMLS_CC);
    if (*op_array == NULL) {
        return FAILURE;
    }

    ctxt.pool = lpc_pool_create(LPC_MEDIUM_POOL, lpc_sma_malloc, lpc_sma_free, 
                                                 lpc_sma_protect, lpc_sma_unprotect TSRMLS_CC);
    if (!ctxt.pool) {
        lpc_warning("Unable to allocate memory for pool." TSRMLS_CC);
        return FAILURE;
    }
    ctxt.copy = LPC_COPY_IN_OPCODE;

    if(LPCG(file_md5)) {
        int n;
        unsigned char buf[1024];
        PHP_MD5_CTX context;
        php_stream *stream;
        char *filename;

        if(h->opened_path) {
            filename = h->opened_path;
        } else {
            filename = h->filename;
        }
        stream = php_stream_open_wrapper(filename, "rb", REPORT_ERRORS | ENFORCE_SAFE_MODE, NULL);
        if(stream) {
            PHP_MD5Init(&context);
            while((n = php_stream_read(stream, (char*)buf, sizeof(buf))) > 0) {
                PHP_MD5Update(&context, buf, n);
            }
            PHP_MD5Final(key.md5, &context);
            php_stream_close(stream);
            if(n<0) {
                lpc_warning("Error while reading '%s' for md5 generation." TSRMLS_CC, filename);
            }
        } else {
            lpc_warning("Unable to open '%s' for md5 generation." TSRMLS_CC, filename);
        }
    }

    if(!(alloc_op_array = lpc_copy_op_array(NULL, *op_array, &ctxt TSRMLS_CC))) {
        goto freepool;
    }

    if(!(alloc_functions = lpc_copy_new_functions(num_functions, &ctxt TSRMLS_CC))) {
        goto freepool;
    }
    if(!(alloc_classes = lpc_copy_new_classes(*op_array, num_classes, &ctxt TSRMLS_CC))) {
        goto freepool;
    }

    path = h->opened_path;
    if(!path && key.type == LPC_CACHE_KEY_FPFILE) path = (char*)key.data.fpfile.fullpath;
    if(!path) path=h->filename;

    lpc_debug("2. h->opened_path=[%s]  h->filename=[%s]\n" TSRMLS_CC, h->opened_path?h->opened_path:"null",h->filename);

    if(!(*cache_entry = lpc_cache_make_file_entry(path, alloc_op_array, alloc_functions, alloc_classes, &ctxt TSRMLS_CC))) {
        goto freepool;
    }

    return SUCCESS;

freepool:
    lpc_pool_destroy(ctxt.pool TSRMLS_CC);
    ctxt.pool = NULL;

    return FAILURE;

}
/* }}} */

/* {{{ my_compile_file
   Overrides zend_compile_file */
static zend_op_array* my_compile_file(zend_file_handle* h,
                                               int type TSRMLS_DC)
{
    lpc_cache_key_t key;
    lpc_cache_entry_t* cache_entry;
    zend_op_array* op_array = NULL;
    time_t t;
    lpc_context_t ctxt = {0,};
    int bailout=0;
	const char* filename = NULL;

    if (!LPCG(enabled) || lpc_cache_busy(lpc_cache)) {
        return old_compile_file(h, type TSRMLS_CC);
    }

    if(h->opened_path) {
        filename = h->opened_path;
    } else {
        filename = h->filename;
    }

    /* check our regular expression filters */
    if (LPCG(filters) && LPCG(compiled_filters) && filename) {
        int ret = lpc_regex_match_array(LPCG(compiled_filters), filename);

        if(ret == LPC_NEGATIVE_MATCH || (ret != LPC_POSITIVE_MATCH && !LPCG(cache_by_default))) {
            return old_compile_file(h, type TSRMLS_CC);
        }
    } else if(!LPCG(cache_by_default)) {
        return old_compile_file(h, type TSRMLS_CC);
    }
    LPCG(current_cache) = lpc_cache;


    t = lpc_time();

    lpc_debug("1. h->opened_path=[%s]  h->filename=[%s]\n" TSRMLS_CC, h->opened_path?h->opened_path:"null",h->filename);

    /* try to create a cache key; if we fail, give up on caching */
    if (!lpc_cache_make_file_key(&key, h->filename, PG(include_path), t TSRMLS_CC)) {
        return old_compile_file(h, type TSRMLS_CC);
    }

    if(!LPCG(force_file_update)) {
        /* search for the file in the cache */
        cache_entry = lpc_cache_find(lpc_cache, key, t TSRMLS_CC);
        ctxt.force_update = 0;
    } else {
        cache_entry = NULL;
        ctxt.force_update = 1;
    }

    if (cache_entry != NULL) {
        int dummy = 1;
        
        ctxt.pool = lpc_pool_create(LPC_UNPOOL, lpc_php_malloc, lpc_php_free,
                                                lpc_sma_protect, lpc_sma_unprotect TSRMLS_CC);
        if (!ctxt.pool) {
            lpc_warning("Unable to allocate memory for pool." TSRMLS_CC);
            return old_compile_file(h, type TSRMLS_CC);
        }
        ctxt.copy = LPC_COPY_OUT_OPCODE;
        
        zend_hash_add(&EG(included_files), cache_entry->data.file.filename, 
                            strlen(cache_entry->data.file.filename)+1,
                            (void *)&dummy, sizeof(int), NULL);

        lpc_stack_push(LPCG(cache_stack), cache_entry TSRMLS_CC);
        op_array = cached_compile(h, type, &ctxt TSRMLS_CC);

        if(op_array) {
#ifdef LPC_FILEHITS
            /* If the file comes from the cache, add it to the global request file list */
            add_next_index_string(LPCG(filehits), h->filename, 1);
#endif
            /* this is an unpool, which has no cleanup - this only free's the pool header */
            lpc_pool_destroy(ctxt.pool TSRMLS_CC);
            
            /* We might leak fds without this hack */
            if (h->type != ZEND_HANDLE_FILENAME) {
                zend_llist_add_element(&CG(open_files), h); 
            }
            return op_array;
        }
        if(LPCG(report_autofilter)) {
            lpc_warning("Autofiltering %s" TSRMLS_CC, 
                            (h->opened_path ? h->opened_path : h->filename));
            lpc_warning("Recompiling %s" TSRMLS_CC, cache_entry->data.file.filename);
        }
        /* TODO: check what happens with EG(included_files) */
    }

    /* Make sure the mtime reflects the files last known mtime, and we respect max_file_size in the case of fpstat==0 */
    if(key.type == LPC_CACHE_KEY_FPFILE) {
        lpc_fileinfo_t fileinfo;
        struct stat *tmp_buf = NULL;
        if(!strcmp(SG(request_info).path_translated, h->filename)) {
            tmp_buf = sapi_get_stat(TSRMLS_C);  /* Apache has already done this stat() for us */
        }
        if(tmp_buf) {
            fileinfo.st_buf.sb = *tmp_buf;
        } else {
            if (lpc_search_paths(h->filename, PG(include_path), &fileinfo TSRMLS_CC) != 0) {
                lpc_debug("Stat failed %s - bailing (%s) (%d)\n" TSRMLS_CC,h->filename,SG(request_info).path_translated);
                return old_compile_file(h, type TSRMLS_CC);
            }
        }
        if (LPCG(max_file_size) < fileinfo.st_buf.sb.st_size) { 
            lpc_debug("File is too big %s (%ld) - bailing\n" TSRMLS_CC, h->filename, fileinfo.st_buf.sb.st_size);
            return old_compile_file(h, type TSRMLS_CC);
        }
        key.mtime = fileinfo.st_buf.sb.st_mtime;
    }

    HANDLE_BLOCK_INTERRUPTIONS();

#if NONBLOCKING_LOCK_AVAILABLE
    if(LPCG(write_lock)) {
        if(!lpc_cache_write_lock(lpc_cache TSRMLS_CC)) {
            HANDLE_UNBLOCK_INTERRUPTIONS();
            return old_compile_file(h, type TSRMLS_CC);
        }
    }
#endif

    zend_try {
        if (lpc_compile_cache_entry(key, h, type, t, &op_array, &cache_entry TSRMLS_CC) == SUCCESS) {
            ctxt.pool = cache_entry->pool;
            ctxt.copy = LPC_COPY_IN_OPCODE;
            if (lpc_cache_insert(lpc_cache, key, cache_entry, &ctxt, t TSRMLS_CC) != 1) {
                lpc_pool_destroy(ctxt.pool TSRMLS_CC);
                ctxt.pool = NULL;
            }
        }
    } zend_catch {
        bailout=1; /* in the event of a bailout, ensure we don't create a dead-lock */
    } zend_end_try();

    LPCG(current_cache) = NULL;

#if NONBLOCKING_LOCK_AVAILABLE
    if(LPCG(write_lock)) {
        lpc_cache_write_unlock(lpc_cache TSRMLS_CC);
    }
#endif
    HANDLE_UNBLOCK_INTERRUPTIONS();

    if (bailout) zend_bailout();

    return op_array;
}
/* }}} */

/* {{{ data preload */

extern int _lpc_store(char *strkey, int strkey_len, const zval *val, const unsigned int ttl, const int exclusive TSRMLS_DC);

static zval* data_unserialize(const char *filename TSRMLS_DC)
{
    zval* retval;
    long len = 0;
    struct stat sb;
    char *contents, *tmp;
    FILE *fp;
    php_unserialize_data_t var_hash;

    if(VCWD_STAT(filename, &sb) == -1) {
        return NULL;
    }

    fp = fopen(filename, "rb");

    len = sizeof(char)*sb.st_size;

    tmp = contents = malloc(len);

    if(!contents) {
       return NULL;
    }

    if(fread(contents, 1, len, fp) < 1) {	
      free(contents);
      return NULL;
    }

    MAKE_STD_ZVAL(retval);

    PHP_VAR_UNSERIALIZE_INIT(var_hash);
    
    /* I wish I could use json */
    if(!php_var_unserialize(&retval, (const unsigned char**)&tmp, (const unsigned char*)(contents+len), &var_hash TSRMLS_CC)) {
        zval_ptr_dtor(&retval);
        return NULL;
    }

    PHP_VAR_UNSERIALIZE_DESTROY(var_hash);

    free(contents);
    fclose(fp);

    return retval;
}

static int lpc_load_data(const char *data_file TSRMLS_DC)
{
    char *p;
    char key[MAXPATHLEN] = {0,};
    unsigned int key_len;
    zval *data;

    p = strrchr(data_file, DEFAULT_SLASH);

    if(p && p[1]) {
        strlcpy(key, p+1, sizeof(key));
        p = strrchr(key, '.');

        if(p) {
            p[0] = '\0';
            key_len = strlen(key);

            data = data_unserialize(data_file TSRMLS_CC);
            if(data) {
                _lpc_store(key, key_len, data, 0, 1 TSRMLS_CC);
            }
            return 1;
        }
    }

    return 0;
}

static int lpc_walk_dir(const char *path TSRMLS_DC)
{
    char file[MAXPATHLEN]={0,};
    int ndir, i;
    char *p = NULL;
    struct dirent **namelist = NULL;

    if ((ndir = php_scandir(path, &namelist, 0, php_alphasort)) > 0)
    {
        for (i = 0; i < ndir; i++)
        {
            /* check for extension */
            if (!(p = strrchr(namelist[i]->d_name, '.'))
                    || (p && strcmp(p, ".data")))
            {
                free(namelist[i]);
                continue;
            }
            snprintf(file, MAXPATHLEN, "%s%c%s",
                    path, DEFAULT_SLASH, namelist[i]->d_name);
            if(!lpc_load_data(file TSRMLS_CC))
            {
                /* print error */
            }
            free(namelist[i]);
        }
        free(namelist);
    }

    return 1;
}

void lpc_data_preload(TSRMLS_D)
{
    if(!LPCG(preload_path)) return;

    lpc_walk_dir(LPCG(preload_path) TSRMLS_CC);
}
/* }}} */

/* {{{ lpc_serializer hooks */
static int _lpc_register_serializer(const char* name, lpc_serialize_t serialize, 
                                    lpc_unserialize_t unserialize,
                                    void *config TSRMLS_DC)
{
    int i;
    lpc_serializer_t *serializer;

    for(i = 0; i < LPC_MAX_SERIALIZERS; i++) {
        serializer = &lpc_serializers[i];
        if(!serializer->name) {
            /* empty entry */
            serializer->name = name; /* assumed to be const */
            serializer->serialize = serialize;
            serializer->unserialize = unserialize;
            serializer->config = config;
            lpc_serializers[i+1].name = NULL;
            return 1;
        }
    }

    return 0;
}

static lpc_serializer_t* lpc_find_serializer(const char* name TSRMLS_DC)
{
    int i;
    lpc_serializer_t *serializer;

    for(i = 0; i < LPC_MAX_SERIALIZERS; i++) {
        serializer = &lpc_serializers[i];
        if(serializer->name && (strcmp(serializer->name, name) == 0)) {
            return serializer;
        }
    }
    return NULL;
}

lpc_serializer_t* lpc_get_serializers(TSRMLS_D)
{
    return &(lpc_serializers[0]);
}
/* }}} */

/* {{{ module init and shutdown */

int lpc_module_init(int module_number TSRMLS_DC)
{
    /* lpc initialization */
#if LPC_MMAP
    lpc_sma_init(LPCG(shm_segments), LPCG(shm_size), LPCG(mmap_file_mask) TSRMLS_CC);
#else
    lpc_sma_init(LPCG(shm_segments), LPCG(shm_size), NULL TSRMLS_CC);
#endif
    lpc_cache = lpc_cache_create(LPCG(num_files_hint), LPCG(gc_ttl), LPCG(ttl) TSRMLS_CC);
    lpc_user_cache = lpc_cache_create(LPCG(user_entries_hint), LPCG(gc_ttl), LPCG(user_ttl) TSRMLS_CC);

    /* override compilation */
    old_compile_file = zend_compile_file;
    zend_compile_file = my_compile_file;
    REGISTER_LONG_CONSTANT("\000apc_magic", (long)&set_compile_hook, CONST_PERSISTENT | CONST_CS);
    REGISTER_LONG_CONSTANT("\000apc_compile_file", (long)&my_compile_file, CONST_PERSISTENT | CONST_CS);
    REGISTER_LONG_CONSTANT(LPC_SERIALIZER_CONSTANT, (long)&_lpc_register_serializer, CONST_PERSISTENT | CONST_CS);

    /* test out the constant function pointer */
    lpc_register_serializer("php", LPC_SERIALIZER_NAME(php), LPC_UNSERIALIZER_NAME(php), NULL TSRMLS_CC);

    assert(lpc_serializers[0].name != NULL);

    lpc_pool_init();

    lpc_data_preload(TSRMLS_C);

#if LPC_HAVE_LOOKUP_HOOKS
    if(LPCG(lazy_functions)) {
        zend_set_lookup_function_hook(lpc_lookup_function_hook TSRMLS_CC);
        zend_set_defined_function_hook(lpc_defined_function_hook TSRMLS_CC);
    }
    if(LPCG(lazy_classes)) {
        zend_set_lookup_class_hook(lpc_lookup_class_hook TSRMLS_CC);
        zend_set_declared_class_hook(lpc_declared_class_hook TSRMLS_CC);
    }
#else
    if(LPCG(lazy_functions) || LPCG(lazy_classes)) {
        lpc_warning("Lazy function/class loading not available with this version of PHP, please disable LPC lazy loading." TSRMLS_CC);
        LPCG(lazy_functions) = LPCG(lazy_classes) = 0;
    }
#endif

#ifdef ZEND_ENGINE_2_4
    lpc_interned_strings_init(TSRMLS_C);
#endif

    LPCG(initialized) = 1;
    return 0;
}

int lpc_module_shutdown(TSRMLS_D)
{
    if (!LPCG(initialized))
        return 0;

    /* restore compilation */
    zend_compile_file = old_compile_file;

    /*
     * In case we got interrupted by a SIGTERM or something else during execution
     * we may have cache entries left on the stack that we need to check to make
     * sure that any functions or classes these may have added to the global function
     * and class tables are removed before we blow away the memory that hold them.
     * 
     * This is merely to remove memory leak warnings - as the process is terminated
     * immediately after shutdown. The following while loop can be removed without
     * affecting anything else.
     */
    while (lpc_stack_size(LPCG(cache_stack)) > 0) {
        int i;
        lpc_cache_entry_t* cache_entry = (lpc_cache_entry_t*) lpc_stack_pop(LPCG(cache_stack));
        if (cache_entry->data.file.functions) {
            for (i = 0; cache_entry->data.file.functions[i].function != NULL; i++) {
                zend_hash_del(EG(function_table),
                    cache_entry->data.file.functions[i].name,
                    cache_entry->data.file.functions[i].name_len+1);
            }
        }
        if (cache_entry->data.file.classes) {
            for (i = 0; cache_entry->data.file.classes[i].class_entry != NULL; i++) {
                zend_hash_del(EG(class_table),
                    cache_entry->data.file.classes[i].name,
                    cache_entry->data.file.classes[i].name_len+1);
            }
        }
        lpc_cache_release(lpc_cache, cache_entry TSRMLS_CC);
    }

    lpc_cache_destroy(lpc_cache TSRMLS_CC);
    lpc_cache_destroy(lpc_user_cache TSRMLS_CC);
    lpc_sma_cleanup(TSRMLS_C);

#ifdef ZEND_ENGINE_2_4
    lpc_interned_strings_shutdown(TSRMLS_C);
#endif

    LPCG(initialized) = 0;
    return 0;
}

/* }}} */

/* {{{ process init and shutdown */
int lpc_process_init(int module_number TSRMLS_DC)
{
    return 0;
}

int lpc_process_shutdown(TSRMLS_D)
{
    return 0;
}
/* }}} */


/* {{{ lpc_deactivate */
static void lpc_deactivate(TSRMLS_D)
{
    /* The execution stack was unwound, which prevented us from decrementing
     * the reference counts on active cache entries in `my_execute`.
     */
    while (lpc_stack_size(LPCG(cache_stack)) > 0) {
        int i;
        zend_class_entry* zce = NULL;
        void ** centry = (void*)(&zce);
        zend_class_entry** pzce = NULL;

        lpc_cache_entry_t* cache_entry =
            (lpc_cache_entry_t*) lpc_stack_pop(LPCG(cache_stack));

        if (cache_entry->data.file.classes) {
            for (i = 0; cache_entry->data.file.classes[i].class_entry != NULL; i++) {
                centry = (void**)&pzce; /* a triple indirection to get zend_class_entry*** */
                if(zend_hash_find(EG(class_table), 
                    cache_entry->data.file.classes[i].name,
                    cache_entry->data.file.classes[i].name_len+1,
                    (void**)centry) == FAILURE)
                {
                    /* double inclusion of conditional classes ends up failing 
                     * this lookup the second time around.
                     */
                    continue;
                }

                zce = *pzce;

                zend_hash_del(EG(class_table),
                    cache_entry->data.file.classes[i].name,
                    cache_entry->data.file.classes[i].name_len+1);

                lpc_free_class_entry_after_execution(zce TSRMLS_CC);
            }
        }
        lpc_cache_release(lpc_cache, cache_entry TSRMLS_CC);
    }
}
/* }}} */

/* {{{ request init and shutdown */

int lpc_request_init(TSRMLS_D)
{
    lpc_stack_clear(LPCG(cache_stack));
    if (!LPCG(compiled_filters) && LPCG(filters)) {
        /* compile regex filters here to avoid race condition between MINIT of PCRE and LPC.
         * This should be moved to lpc_cache_create() if this race condition between modules is resolved */
        LPCG(compiled_filters) = lpc_regex_compile_array(LPCG(filters) TSRMLS_CC);
    }

    if (!LPCG(serializer) && LPCG(serializer_name)) {
        /* Avoid race conditions between MINIT of lpc and serializer exts like igbinary */
        LPCG(serializer) = lpc_find_serializer(LPCG(serializer_name) TSRMLS_CC);
    }

#if LPC_HAVE_LOOKUP_HOOKS
    if(LPCG(lazy_functions)) {
        LPCG(lazy_function_table) = emalloc(sizeof(HashTable));
        zend_hash_init(LPCG(lazy_function_table), 0, NULL, NULL, 0);
    }
    if(LPCG(lazy_classes)) {
        LPCG(lazy_class_table) = emalloc(sizeof(HashTable));
        zend_hash_init(LPCG(lazy_class_table), 0, NULL, NULL, 0);
    }
#endif

#ifdef LPC_FILEHITS
    ALLOC_INIT_ZVAL(LPCG(filehits));
    array_init(LPCG(filehits));
#endif

    return 0;
}

int lpc_request_shutdown(TSRMLS_D)
{

#if LPC_HAVE_LOOKUP_HOOKS
    if(LPCG(lazy_class_table)) {
        zend_hash_destroy(LPCG(lazy_class_table));
        efree(LPCG(lazy_class_table));
    }
    if(LPCG(lazy_function_table)) {
        zend_hash_destroy(LPCG(lazy_function_table));
        efree(LPCG(lazy_function_table));
    }
#endif

    lpc_deactivate(TSRMLS_C);

#ifdef LPC_FILEHITS
    zval_ptr_dtor(&LPCG(filehits));
#endif

    return 0;
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
