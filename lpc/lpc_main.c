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

#include "lpc.h"
#include "lpc_php.h"
#include "lpc_string.h"
#include "lpc_zend.h"
#include "lpc_compile.h"

#include "zend_API.h"
#include "zend_alloc.h"
#include "zend_hash.h"
#include "zend_variables.h"
#include "SAPI.h"
#include "php_scandir.h"
#include "ext/standard/php_var.h"

#define CHECK(p) if(!(p)) goto error

/* {{{ module variables */

zend_compile_t *lpc_old_compile_file;

/* }}} */

/* {{{ get/set lpc_old_compile_file (to interact with other extensions that need the compile hook) */
zend_compile_t* lpc_set_compile_hook(zend_compile_t *ptr)
{ENTER(lpc_set_compile_hook)
    zend_compile_t *retval = lpc_old_compile_file;

    if (ptr != NULL) lpc_old_compile_file = ptr;
    return retval;
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

   /*
	* There could be problems class inheritance problems during the module load, especially if
	* autoloading is being employed as the loading of the child class triggers autoload of the
	* parent. A missing parent will therefore forces explicit re-compile whether __autoload is
	* enabled or not, because __autoload errors cause php to die.
    *
	* Failing back to a recompile requires the uninstalling any classes already loaded and aborting
	* any function or opcode loads.  
    */

	for (i = 0; i < cache_entry->num_classes; i++) {
		lpc_class_t*       cl = &cache_entry->classes[i];
		zend_class_entry*  dst_cl;
        zend_class_entry** parent = NULL;

		if(cl->name_len != 0 && cl->name[0] == '\0') {
		    if(zend_hash_exists(CG(class_table), cl->name, cl->name_len+1)) {
		        continue;
		    }
		}

        if (cl->parent_name != NULL) {
			if (zend_lookup_class_ex(cl->parent_name, strlen(cl->parent_name), 
#ifdef ZEND_ENGINE_2_4
                                     NULL,
#endif
                                     0, &parent TSRMLS_PC) == FAILURE) {
				i_fail = i;
				break;
			}
		}	
		pool_alloc(dst_cl, sizeof(zend_class_entry));
		lpc_copy_class_entry(dst_cl, cl->class_entry,  pool);

///TODO: test this inheritance
		if (parent) {
	        dst_cl->parent = *parent;
	        zend_do_inheritance(dst_cl, *parent TSRMLS_PC);
	    }
		if (zend_hash_add(EG(class_table), cl->name, cl->name_len+1,
		                  &dst_cl, sizeof(zend_class_entry*), NULL) == FAILURE) {
		    lpc_error("Cannot redeclare class %s" TSRMLS_CC, cl->name);
			i_fail = i;
			break;
		}
	}

	if (i_fail >= 0) {
		/* There has been a class inheritance failure, so install classes in reverse order and return */
		for (i = i_fail; i >= 0; i--) {
			lpc_class_t* cl = &cache_entry->classes[i];
		    if (zend_hash_del(EG(class_table), cl->name, cl->name_len+1) == FAILURE) {
       			 lpc_error("Cannot delete class %s" TSRMLS_CC, cl->name);
			}
		}
///// TODO:  This still leaves the class structures in memory so these WILL leak.
//		lpc_cache_release(cache_entry TSRMLS_CC);
		return NULL;
	}

    for (i = 0; i < cache_entry->num_functions; i++) {
       /*
        * Installed functions are maintained by value in the EG(function_table). However, all of the
        * dynamically allocated fields must be copied from the serial pool (which is about to be
        * destroyed) into exec pool (emalloc) storage. lpc_copy_function() does this deep copy. As
        * the function structure itelf is coped by value, it can be allocated on the stack. 
        */
        lpc_function_t *fn = &cache_entry->functions[i];
		zend_function func;
    	lpc_copy_function(&func, fn->function,  pool);
		if (zend_hash_add(EG(function_table), fn->name, fn->name_len+1,
		                    &func, sizeof(*fn->function), NULL) == FAILURE) {
		    lpc_error("Cannot redeclare %s()" TSRMLS_CC, fn->name);
		}
    }

    lpc_do_halt_compiler_register(cache_entry->filename, cache_entry->halt_offset TSRMLS_CC);

	pool_alloc(op_array, sizeof(zend_op_array));
    lpc_copy_op_array(op_array, cache_entry->op_array,  pool);
	return op_array;
}
/* }}} */


/* {{{ my_compile_file
	LPC substitutes my_compile_file callback for the standard zend_compile_file.  It essentially
    takes one of three execution paths:

    *  If a valid copy of the file is already cached in the opcode cache then cached_compile() is
       called to install this in the runtime environment

    *  If it is a valid file for caching, then zend_compile_file() is called to compile the file
       into the runtime environment, and is then deep copied into a serial pool for insertion 
       into the opcode cache.

    *  If caching is disabled, either generally or for this file then execution is passed 
       directly to zend_compile_file().
*/
static zend_op_array* my_compile_file(zend_file_handle* h, int type TSRMLS_DC)
{ENTER(my_compile_file)
    lpc_cache_key_t   *key = NULL;
    lpc_entry_block_t *cache_entry;
	lpc_pool          *pool;
    zend_op_array     *op_array = NULL;
    time_t             t = LPCG(sapi_request_time);
    lpc_pool          *exec_pool = NULL;
    int                bailout = 0;
	const char        *filename = NULL;

    filename = (h->opened_path) ? h->opened_path : h->filename;

/////////// TODO: At the moment make key can open the file.  If it's going to do this then it makes sense to pass in the zend_file_handle so that the fh type and contents can be updated with the openned FH so that it gets properly GCCed

	/* chain onto the old (zend) compile file function if the file isn't cacheable */
    if (!LPCG(enabled) ||
	    !lpc_valid_file_match(filename TSRMLS_CC) ||
        !(key = lpc_cache_make_key(h TSRMLS_CC))) { 
        return lpc_old_compile_file(h, type TSRMLS_CC);
    }

    lpc_debug("1. h->opened_path=[%s]  h->filename=[%s]" TSRMLS_CC, h->opened_path?h->opened_path:"null",h->filename);

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
		/* For some reason the fn is gcc, so replace pool address by the handle one */ 
		op_array->filename = (char *) filename;            
        return op_array;
    } else if(key->type == LPC_CACHE_MISS) {

		/* Compile the file and add it to the cache */
		    if (lpc_compile_cache_entry(key, h, type, &op_array, &pool TSRMLS_CC) == SUCCESS) {
		        lpc_cache_insert(key, pool);
		    }
		lpc_cache_free_key(key TSRMLS_CC);
		return op_array;
	}
}
/* }}} */

/* {{{ module init and shutdown */

int lpc_module_init(int module_number TSRMLS_DC)
{
    /* lpc initialization */

    /* override compilation */
    lpc_old_compile_file = zend_compile_file;
    zend_compile_file = my_compile_file;
    REGISTER_LONG_CONSTANT("\000lpc_magic", (long)&lpc_set_compile_hook, CONST_PERSISTENT | CONST_CS);
    REGISTER_LONG_CONSTANT("\000lpc_compile_file", (long)&my_compile_file, CONST_PERSISTENT | CONST_CS);

#ifdef ZEND_ENGINE_2_4
    lpc_interned_strings_init(TSRMLS_C);
#endif

    LPCG(initialized) = 1;
    return 0;
}

int lpc_module_shutdown(TSRMLS_D)
{ENTER(lpc_module_shutdown)
    if (LPCG(initialized)) {
		zend_compile_file = lpc_old_compile_file;
		LPCG(initialized) = 0;
	}
    return 0;
}
/* }}} */

/* {{{ lpc_deactivate */
static void lpc_deactivate(TSRMLS_D)
{ENTER(lpc_deactivate)
    /* The execution stack was unwound, but since any in-memory caching is local to the process
     * unlike APC, there is not need to worry about reference counts on active cache entries in
     * `my_execute` -- normal memory cleanup will take care of this.
     */
/////////////// TODO:  unwind code from MSHUTDOWN needs to be folded in RSHUTDOWN now that caches and stacks only have a request lifetime

    lpc_cache_destroy(TSRMLS_C);

#ifdef ZEND_ENGINE_2_4
    lpc_interned_strings_shutdown(TSRMLS_C);
#endif
}
/* }}} */

/* {{{ request init and shutdown */
int lpc_request_init(TSRMLS_D)
{ENTER(lpc_request_init)
	zend_hash_init(&LPCG(pools), 10, NULL, NULL, 0);

    return 0;
}

int lpc_request_shutdown(TSRMLS_D)
{ENTER(lpc_request_shutdown)
	lpc_pool *pool;  int s;
	HashTable *pools_ht = &LPCG(pools);
	char *dummy;

    lpc_deactivate(TSRMLS_C);

	/* Loop over pools to destroy each pool. Note that pool_destroy removes the entry so 
     * the loop repeatly resets the internal point at fetches the first element until empty */
	zend_hash_internal_pointer_reset(pools_ht);
	while(zend_hash_get_current_key(pools_ht, &dummy, (ulong *)&pool, 0) == HASH_KEY_IS_LONG) {
/////////////////////////////////  Temp Patch for testing ///////////////////////
		lpc_debug("Freeing dangling pool at 0x%lx" TSRMLS_CC, pool);
		pool_destroy();
	}
	zend_hash_destroy(pools_ht);

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
