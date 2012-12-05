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
  | Authors: Terry Ellison <Terry@ellisons.org.uk>                       |
  +----------------------------------------------------------------------+

   This software was derived from the APC extension which was initially 
   contributed to PHP by Community Connect Inc. in 2002 and revised in 2005 
   by Yahoo! Inc. See README for further details.
 
   All other licensing and usage conditions are those of the PHP Group.
*/

#include "zend.h"
#include "lpc.h"
//#include "lpc_zend.h"
#include "lpc_copy_source.h"

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


/* {{{ module init and shutdown */

int lpc_module_init(int module_number TSRMLS_DC)
{
    /* lpc initialization */

    /* override compilation */
    lpc_old_compile_file = zend_compile_file;
    zend_compile_file = lpc_compile_file;
    REGISTER_LONG_CONSTANT("\000lpc_magic", (long)&lpc_set_compile_hook, CONST_PERSISTENT | CONST_CS);
    REGISTER_LONG_CONSTANT("\000lpc_compile_file", (long)&lpc_compile_file, CONST_PERSISTENT | CONST_CS);

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
