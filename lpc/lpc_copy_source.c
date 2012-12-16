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

   This software was derived from the APC extension which was initially 
   contributed to PHP by Community Connect Inc. in 2002 and revised in 2005 
   by Yahoo! Inc. See README for further details.
 
   All other licensing and usage conditions are those of the PHP Group.
*/

#include "lpc.h"
#include "lpc_copy_op_array.h"
#include "lpc_copy_source.h"
#include "lpc_hashtable.h"

static zend_op_array* cached_compile(lpc_entry_block_t* cache_entry, lpc_pool* pool);
static long           file_halt_offset(const char *filename TSRMLS_DC);
static zend_bool      compile_cache_entry(lpc_cache_key_t *key, zend_file_handle* h, 
                                          int type, zend_op_array** op_array, 
                                          lpc_pool **pool_ptr TSRMLS_DC);
static void           do_halt_compiler_register(const char *filename, long halt_offset TSRMLS_DC);

extern zend_compile_t *lpc_old_compile_file;

/* {{{ lpc_compile_file
    LPC substitutes lpc_compile_file callback for the standard zend_compile_file.  It essentially
    takes one of three execution paths:

    *  If a valid copy of the file is already cached in the opcode cache then cached_compile() is
       called to install this in the runtime environment
  
    *  If it is a valid file for caching, then zend_compile_file() is called to compile the file
       into the runtime environment, and is then deep copied into a serial pool for insertion 
       into the opcode cache.

    *  If caching is disabled, either generally or for this file then execution is passed 
       directly to zend_compile_file().
*/
zend_op_array* lpc_compile_file(zend_file_handle* h, int type TSRMLS_DC)
{ENTER(lpc_compile_file)
    lpc_cache_key_t   *key = NULL;
    lpc_entry_block_t *cache_entry;
    lpc_pool          *pool;
    zend_op_array     *op_array = NULL;
    time_t             t = LPCG(sapi_request_time);
    lpc_pool          *exec_pool = NULL;
    int                bailout = 0;
    const char        *filename = NULL;

    filename = (h->opened_path) ? h->opened_path : h->filename;
   /*
    * Chain onto the old (zend) compile file function if the file isn't cacheable 
    */
    if (!LPCG(enabled) ||
        !lpc_valid_file_match((char *)filename TSRMLS_CC) ||
        !(key = lpc_cache_make_key(h, INI_STR("include_path") TSRMLS_CC))) { 
        return lpc_old_compile_file(h, type TSRMLS_CC);
    }
#ifdef LPC_DEBUG
    if (LPCG(debug_flags)&LPC_DBG_FILES)  /* Load/Unload Info */
        lpc_debug("1. h->opened_path=[%s]  h->filename=[%s]" TSRMLS_CC, 
                  h->opened_path?h->opened_path:"null",h->filename);
#endif

    LPCG(current_filename) = h->filename;
   /*
    * The PHP RTS opens some files (e.g. the script request), so add to the Zend open files list to
    * ensure that the fds and any associated strings are garbage collected at request shutdown.
    */
    if (h->type == ZEND_HANDLE_FP) {
        zend_llist_add_element(&CG(open_files), h); 
    }

    /* If a valid cache entry exists then load the previously compiled module */
    if ( key->type == LPC_CACHE_LOOKUP &&
         (pool = lpc_cache_retrieve(key TSRMLS_CC)) != NULL) {
        int dummy = 1;
        
        /* The cache entry record was the first allocated in the pool */
        cache_entry = (lpc_entry_block_t *)pool_get_entry_rec();

        exec_pool = pool_create(LPC_EXECPOOL);
        
        zend_hash_add(&EG(included_files), cache_entry->filename, 
                            strlen(cache_entry->filename)+1,
                            (void *)&dummy, sizeof(int), NULL);

        if ((op_array = cached_compile(cache_entry, exec_pool)) == NULL) {
/////////// TODO: Decide on correct action if cached compile fails, but drop-through isn't the correct response
        }
        lpc_cache_free_key(key TSRMLS_CC);
        pool_destroy();

        LPCG(current_filename) = NULL;
           
        return op_array;
 
   } else if(key->type == LPC_CACHE_MISS) {

        /* Compile the file and add it to the cache */
            if (compile_cache_entry(key, h, type, &op_array, &pool TSRMLS_CC) == SUCCESS) {
                lpc_cache_insert(key, pool);
            }
        lpc_cache_free_key(key TSRMLS_CC);
        LPCG(current_filename) = NULL;
        return op_array;

    } else {
        lpc_cache_free_key(key TSRMLS_CC);
        LPCG(current_filename) = NULL;
        return lpc_old_compile_file(h, type TSRMLS_CC);
    }
}
/* }}} */

/* {{{ compile_cache_entry  */
static zend_bool compile_cache_entry(lpc_cache_key_t *key, zend_file_handle* h,
                                     int type, zend_op_array** op_array,
                                     lpc_pool **pool_ptr TSRMLS_DC) 
{ENTER(compile_cache_entry)
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
        lpc_copy_new_functions(entry->functions, num_functions, pool);
    } else {
        entry->functions = NULL;
    }

    entry->num_classes = num_classes;
    if (num_classes) {
        pool_alloc(entry->classes, sizeof(lpc_class_t) * num_classes);
///////// STATUS =
        lpc_copy_new_classes(entry->classes, num_classes, pool);
    } else {
        entry->classes = NULL;
    }

    entry->halt_offset = file_halt_offset(h->filename TSRMLS_PC);

#ifdef LPC_DEBUG
    if (LPCG(debug_flags)&LPC_DBG_FILES)  /* Load/Unload Info */
        lpc_debug("h->opened_path=[%s]  h->filename=[%s]" TSRMLS_CC, 
                  h->opened_path?h->opened_path:"null",h->filename);
#endif
    *pool_ptr = pool; 
    return SUCCESS;

error:
    /* The old compile file module will have raised any errors, so just ... */
    return FAILURE;
}
/* }}} */

/* {{{ cached_compile */
static zend_op_array* cached_compile(lpc_entry_block_t* cache_entry, lpc_pool* pool)
{ENTER(cached_compile)
    int i, ii;
    int i_fail = -1;
    zend_op_array* op_array;
    TSRMLS_FETCH_FROM_POOL();

    assert(cache_entry != NULL);

    if (lpc_install_classes(cache_entry->classes, cache_entry->num_classes, pool) == FAILURE) {
        lpc_cache_release(cache_entry TSRMLS_CC);
        return NULL;
    }

    lpc_install_functions(cache_entry->functions, cache_entry->num_functions, pool);

    do_halt_compiler_register(cache_entry->filename, cache_entry->halt_offset TSRMLS_CC);

    pool_alloc(op_array, sizeof(zend_op_array));
    lpc_copy_op_array(op_array, cache_entry->op_array,  pool);

    return op_array;
}
/* }}} */

/* {{{ file_halt_offset */
static long file_halt_offset(const char *filename TSRMLS_DC)
{ENTER(file_halt_offset)
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
static void do_halt_compiler_register(const char *filename, long halt_offset TSRMLS_DC)
{ENTER(do_halt_compiler_register)
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

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */

