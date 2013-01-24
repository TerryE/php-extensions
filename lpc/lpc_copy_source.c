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
#include "lpc_copy_source.h"
#include "lpc_copy_function.h"
#include "lpc_copy_class.h"
#include "lpc_hashtable.h"

static zend_op_array* cached_compile(lpc_entry_block_t* cache_entry, lpc_pool* pool);
static long           file_halt_offset(const char *filename TSRMLS_DC);
static lpc_pool*      build_cache_entry(lpc_cache_key_t*key, zend_op_array *op_array,
                                        int num_functions, int num_classes TSRMLS_DC); 
static void           do_halt_compiler_register(const char *filename, long halt_offset TSRMLS_DC);

extern zend_compile_t *lpc_old_compile_file;

/* {{{ safe_old_compile_file 
       Compile errors can result in the Zend compile bailing out, so the call is wrapped in a 
       zend_try / zend_catch to handle any LPC cleanup after Compile errors. */
static zend_op_array* safe_old_compile_file(zend_file_handle* h, int type TSRMLS_DC)
{
    zend_op_array *op_array;
    int bailout       = 0;

    zend_try {
        op_array = lpc_old_compile_file(h, type TSRMLS_CC);
    } zend_catch {
        bailout=1;
    } zend_end_try();

    if (bailout || !op_array) {
        LPCG(enabled) = 0;  /* Any compile errors turn off LPC caching */
        if (bailout) {
           /*
            * Pool shutdown is called to free any malloced memory before bailing
            */
            lpc_pool_shutdown(TSRMLS_C);
            zend_bailout();
        } else {
            return NULL;
        }
    }
    return op_array;
}
/* }}} */

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

    Note that by convention "pool" is always used as the o/p pool so that the pool allocator macros
    work correctly hence:
        (copy-out)   (direct from memory)      -> (serial pool) pool;
        (copy-in)    (serial RO pool) sro_pool -> (exec pool) pool; 
*/
zend_op_array* lpc_compile_file(zend_file_handle* h, int type TSRMLS_DC)
{ENTER(lpc_compile_file)
    lpc_cache_key_t   *key = NULL;
    lpc_entry_block_t *cache_entry;
    lpc_pool          *pool = NULL,  *sro_pool = NULL;
    time_t             t = LPCG(sapi_request_time);
    const char        *filename = NULL;

    filename = (h->opened_path) ? h->opened_path : h->filename;

    if (LPCG(enabled) && lpc_valid_file_match((char *)filename TSRMLS_CC)) {
        key = lpc_cache_make_key(h, INI_STR("include_path") TSRMLS_CC);
    }

    if (key && (LPCG(sapi_request_time) - key->mtime) > LPCG(file_update_protection) &&
        key->type == LPC_CACHE_MISS) {  /* ============ Source needs compiled ============ */
       /*
        * No valid entry for the key exists in the file cache, but it is an includable filename, so 
        * the cache entry needs to be compiled and serialised.  (As a safety check, the file must be 
        * at least lpc.file_update_protection seconds old.) This first involves calling the normal 
        * compilation process which returns an op_array as well as adding any new entries for any
        * functions and classes that were compiled onto the CG(function_table) and CG(class_table) 
        * HashTables.  So these last two HTs are first high-water marked to determine any additions.
        */
        zend_op_array *op_array;
        zend_uint      pool_length, compressed_length, request;
        zend_uchar    *compressed_buffer;

        int num_functions = zend_hash_num_elements(CG(function_table));
        int num_classes   = zend_hash_num_elements(CG(class_table));
        op_array          = safe_old_compile_file(h, type TSRMLS_CC);
        num_functions     = zend_hash_num_elements(CG(function_table)) - num_functions;
        num_classes       = zend_hash_num_elements(CG(class_table))    - num_classes;
       /*
        * Once a compile is (cleanly) completed, it is then serialised for o/p to the file cache.  
        * LPC adopts an optimistic strategy here, preallocating the serial storage before doing the 
        * build. Overflows can still occur, but this is a delatively rare occurance with the default
        * lpc.compression setting.  However if overflow occurs then the copy out process bails out,
        * and the copy-out is then retried with a larger allocation. Retry is infrequent and is
        * relatively cheap, but this approach considerably simplifies the pool allocations logic.
        * See TECHNOTES.txt for further discussion of this approach.  
        */
        LPCG(current_filename) = op_array->filename;

        request = 0;  /* start off with current / default buffer */
        do {
            lpc_try {

                pool = NULL;
                compressed_length = 0;
                /* Any of the following 3 calls can bailout with an LPC_POOL_OVERFLOW */
                lpc_pool_storage(request, 0, NULL TSRMLS_CC);
                pool = build_cache_entry(key, op_array, num_functions, num_classes TSRMLS_CC);
                compressed_buffer = lpc_pool_serialize(pool, &compressed_length, &pool_length);

            } lpc_catch {

                request = LPCG(storage_quantum); /* retry with an enlarged pool buffer */
                lpc_pool_destroy(&LPCG(serial_pool));

            } lpc_end_try();

        } while (!compressed_length);

       /*
        * If the bailout_status != LPC_POOL_OVERFLOW, but compressed_length == 0 then we've
        * bailed out for some other reason, so clean up LPC and reissue the bailout.
        */
        if (compressed_length == 0) {
            lpc_pool_destroy(&LPCG(serial_pool));
            lpc_pool_shutdown(TSRMLS_C);
            zend_bailout();
        }

        lpc_cache_insert(key, compressed_buffer, compressed_length, pool_length TSRMLS_CC);
        lpc_cache_free_key(key TSRMLS_CC);
        lpc_pool_destroy(pool);

#ifdef LPC_DEBUG
        if (LPCG(debug_flags)&LPC_DBG_FILES)  /* Load/Unload Info */
            lpc_debug("Opcode cache entry for %s created (%u bytes)" TSRMLS_CC, 
                      h->filename, compressed_length);
#endif	
        LPCG(current_filename) = NULL;

        return op_array;

    } else if (key && key->type == LPC_CACHE_LOOKUP &&  /* ======= Load a cached compile ======== */
              (sro_pool = lpc_cache_retrieve(key, (void **) &cache_entry TSRMLS_CC)) != NULL ) {
       /*
        * A valid entry for the key exists in the file cache and has been returned.  Copy it into 
        * the exec pool and return to the zend environment. 
        */
        zend_op_array *op_array = NULL;
        int dummy = 1;
    
        pool     = lpc_pool_create(LPC_EXECPOOL, NULL TSRMLS_CC);
        pool_strdup(LPCGP(current_filename), cache_entry->filename, 0);
        op_array = cached_compile(cache_entry, pool);

        zend_hash_add(&EG(included_files), LPCGP(current_filename), 
                            strlen(LPCGP(current_filename))+1,
                            (void *)&dummy, sizeof(int), NULL);

	    if (h->opened_path) {
            efree(h->opened_path);
        }

        lpc_cache_free_key(key TSRMLS_CC);
        lpc_pool_destroy(sro_pool);

        return op_array;
    }
   /*
    * If we've dropped through to here, the file isn't cacheable or has failed stat validation,
    * or something has gone wrong.  So chain onto the old (zend) compile file function.  
    */
#ifdef LPC_DEBUG
    if (LPCG(debug_flags)&LPC_DBG_FILES)  /* Load/Unload Info */
        lpc_debug("Failing back to default compile for %s" TSRMLS_CC, h->filename);
#endif
    if (key) lpc_cache_free_key(key TSRMLS_CC);
    LPCG(current_filename) = NULL;
    return safe_old_compile_file(h, type TSRMLS_CC);
}
/* }}} */

/* {{{ compile_cache_entry  */
static lpc_pool* build_cache_entry(lpc_cache_key_t *key, zend_op_array *op_array,
                                   int num_functions, int num_classes TSRMLS_DC) 
{ENTER(build_cache_entry)
    lpc_entry_block_t* entry;
    lpc_pool* pool;
   /*
    * The compile has succeeded so create the pool and allocated the entry block as this is
    * alway the first block allocated in a serial pool, then fill the entry block. Note that
    * the key->filename is that passed to the include/require request by the execution
    * environment.  We need to store the version that is fully resolved by the compiler.
    */
    pool = lpc_pool_create(LPC_SERIALPOOL, (void **) &entry  TSRMLS_CC);

    pool_alloc(entry, sizeof(lpc_entry_block_t));
    pool_strdup(entry->filename, LPCGP(current_filename), 0);
    entry->num_functions = num_functions;
    entry->num_classes   = num_classes;

    pool_alloc(entry->op_array, sizeof(zend_op_array));
    lpc_copy_op_array(entry->op_array, op_array,  pool);

    if (num_functions) {
        lpc_copy_new_functions(&entry->functions, num_functions, pool);
    }

    if (num_classes) {
        lpc_copy_new_classes(&entry->classes, num_classes, pool);
    }

    entry->halt_offset = file_halt_offset(key->filename TSRMLS_PC);

    return pool;
}
/* }}} */

/* {{{ cached_compile */
static zend_op_array* cached_compile(lpc_entry_block_t* cache_entry, lpc_pool* pool)
{ENTER(cached_compile)
   /*
    * This function substitutes an unserialized op_array hierarchy which has been retrieved from the
    * file cache for the op_array hierarchy that would have been generated in the normal compilation
    * process by zend_lanuage_scanner.c:compile_file().  Unfortunately this Zend function also has
    * side-effects that must be replicated here to ensure proper handover to the Zend RTE and its
    * request rundown garbage collection.  However, as only successful compiles are cached, we only
    * need to mirror side-effects on the succesful compile execution path.  
    */
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

