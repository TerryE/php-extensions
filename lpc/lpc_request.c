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
#include "SAPI.h"
#include "lpc.h"
#include "lpc_request.h"
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
    LPCG(current_filename) = NULL;

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

/* {{{ add_filter_delims */
static char* add_filter_delims(const char *filter TSRMLS_DC)
{ENTER(add_filter_delims)
    char *regexp, *p;
    int   i;
#define DELIMS " \"',~@%#"
#ifdef PHP_REXEP_OK
    if (filter) {
       /*
        * Pick a PCRE delimiter that isn't in the file string from one of non-alphanumeric 
        * characters that isn't already used as meta character in REGEX, viz <#%,@~ '"> 
        */
        for( i=strlen(DELIMS+1), p=DELIMS; i>=0; i-- ) {
            if (!strchr(filter, p[i])) break; /* scan DELIMS in reverse to pick one not in filter */
        }
        if (i >= 0) {
            regexp = emalloc(strlen(filter) + 3);
            sprintf(regexp,"%c%s%c",p[i],filter,p[i]);
            return regexp;
        }
        lpc_warning("Invalid filter expression '%s'.  Caching is disabled." TSRMLS_CC, filter);
        LPCG(enabled) = 0;
    }
#endif
    return "";  /* An empty string disables filter processing */
}
/* }}} */

/* {{{ request init */
int lpc_request_init(TSRMLS_D)
{ENTER(lpc_request_init)
    lpc_request_context_t  *rc;
    char                   *request_path = SG(request_info).path_translated;
    struct stat            *sb = sapi_get_stat(TSRMLS_C);
    size_t                  dir_length, basename_length;
   /*
    * fetch the "PER_DIR" scoped ini variables
    */
    LPCG(max_file_size)    = lpc_atol(INI_STR("lpc.max_file_size"),0);
    LPCG(fpstat)           = INI_INT("lpc.stat_percentage");
    LPCG(clear_cookie)     = INI_STR("lpc.clear_cookie");
    LPCG(clear_parameter)  = INI_STR("lpc.clear_parameter");
    LPCG(resolve_paths)    = (INI_BOOL("lpc.resolve_paths")!=0);
    LPCG(debug_flags)      = INI_INT("lpc.debug_flags");

   /*
    * Allocate the request context block and fill in the fields based on the request path name
    */
    rc = ecalloc(1, sizeof(lpc_request_context_t));
//// TODO: set clear_flag_set based on cookie and request parameter in the meantime default to false
    rc->clear_flag_set     = 0;
    rc->filter             = add_filter_delims(INI_STR("lpc.filter") TSRMLS_CC);
    rc->cachedb_pattern    = add_filter_delims(INI_STR("lpc.cache_pattern") TSRMLS_CC);
    rc->cachedb_replacement= INI_STR("lpc.cache_replacement");
    LPCG(request_context)  = rc;
    rc->PHP_version        = PHP_VERSION;

    if (request_path) {
        rc->request_fullpath   = request_path;
        rc->request_dir        = estrdup(rc->request_fullpath);
        dir_length             = zend_dirname(rc->request_dir, strlen(rc->request_fullpath));
        rc->request_dir[dir_length] = '\0';
        php_basename(rc->request_fullpath, strlen(rc->request_fullpath), NULL, 0, 
                     &rc->request_basename, &basename_length TSRMLS_CC);
        rc->request_mtime      = sb->st_mtime;
        rc->request_filesize   = sb->st_size;
    }
    /*
     * If the global LPC enabled flag is clear or the request path isn't a filename then
     * fail back to normal uncached operation
     */
    if (!LPCG(enabled) || !request_path) {
        return 0;
    }
   /* 
    * Determine the cache name either from the cache pattern and replacement if set and
    * defaulting to one based on the dirname and basename of the request path 
    */
    if (lpc_generate_cache_name(rc TSRMLS_CC) &&
        lpc_cache_create(TSRMLS_C)==SUCCESS) {

            zend_hash_init(&LPCG(pools), 10, NULL, NULL, 0);
            return 1;

        } else {

            lpc_dtor_request_context(TSRMLS_C);
    }
    return 0;
}
/* }}} */

/* {{{ request shutdown */
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
#ifdef LPC_DEBUG
        if (LPCG(debug_flags)&LPC_DBG_LOAD) { /* Load/Unload Info */
            lpc_debug("Freeing dangling pool at 0x%lx" TSRMLS_CC, pool);
        }
#endif
        pool_destroy();
    }    
    lpc_dtor_request_context(TSRMLS_C);
    zend_hash_destroy(pools_ht);
    return 0;
}
/* }}} */

/* {{{ lpc_dtor_request_context */
void lpc_dtor_request_context(TSRMLS_D)
{ENTER(lpc_dtor_context)
#define EFREE(v) if (v) efree(v);
    lpc_request_context_t  *rc = LPCG(request_context);
    if (rc) {
        EFREE(rc->request_dir);
        EFREE(rc->request_basename);
        EFREE(rc->cachedb_fullpath);
        if (rc->filter&&rc->filter[0]) efree(rc->filter);
        if (rc->cachedb_pattern && rc->cachedb_pattern[0]) efree(rc->cachedb_pattern);
        efree(rc);
        LPCG(request_context) = NULL;
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
