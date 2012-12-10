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
  |          George Schlossnagle <george@omniti.com>                     |
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
//#include "lpc_zend.h"

/* Only implement regexp filters for PHP versions >= 5.2.2 otherwise it acts as a Noop */
#ifdef PHP_REXEP_OK
#  include "ext/pcre/php_pcre.h"
#endif

/* }}} */

/* {{{ console display functions */
#ifdef ZTS
# define LPC_PRINT_FUNCTION_PARAMETER TSRMLS_C
#else
# define LPC_PRINT_FUNCTION_PARAMETER format
#endif

#define LPC_PRINT_FUNCTION(name, verbosity)							\
	void lpc_##name(const char *format TSRMLS_DC, ...)				\
	{																\
		va_list args;												\
		va_start(args, LPC_PRINT_FUNCTION_PARAMETER);				\
		php_verror(NULL, "", verbosity, format, args TSRMLS_CC);	\
		va_end(args);												\
	}

LPC_PRINT_FUNCTION(error, E_ERROR)
LPC_PRINT_FUNCTION(warning, E_WARNING)
LPC_PRINT_FUNCTION(notice, E_NOTICE)

#ifdef DEBUG_LPC
LPC_PRINT_FUNCTION(debug, E_NOTICE)
#else
void lpc_debug(const char *format TSRMLS_DC, ...) {}
#endif
/* }}} */

/* {{{ lpc_search_paths */
/* similar to php_stream_stat_path */
#define LPC_URL_STAT(wrapper, filename, pstatbuf) \
    ((wrapper)->wops->url_stat((wrapper), (filename), PHP_STREAM_URL_STAT_QUIET, (pstatbuf), NULL TSRMLS_CC))

/* copy out to path_buf if path_for_open isn't the same as filename */
#define COPY_IF_CHANGED(p) \
    (char*) (((p) == filename) ? filename : \
            (strlcpy((char*)fileinfo->path_buf, (p), sizeof(fileinfo->path_buf))) \
                    ? (fileinfo->path_buf) : NULL)

/* len checks can be skipped here because filename is NUL terminated */
#define IS_RELATIVE_PATH(filename, len) \
        ((filename) && (filename[0] == '.' && \
            (IS_SLASH(filename[1]) || \
                (filename[1] == '.' && \
                    S_SLASH(filename[2])))))
    

/* {{{ lpc_valid_file_match (string filename)
   LPC has cut REGEX support back to a minimum subset:
   *  REGEXs are only used if PHP version >= 5.2.2 and ignored otherwise
   *  The PHP regexp engine goes to great lengths to cache REGEX compiles so don't redo this!
   *  This is now thread aware and so can directly access the LPC global vector 
   *  Only a single expression is used because normal REGEX syntax (alternative selections 
      and negative assertion) can be used do all of the previous APC REGEX functions.
*/
int lpc_valid_file_match(char *filename TSRMLS_DC)
{ENTER(lpc_valid_file_match)
	char *filt = LPCG(request_context)->filter;
	pcre_cache_entry *pce;
	zval retval;

	/* handle the simple "always match conditions. */ 
	if (filt == NULL || filt[0] =='\0' || filt[2] == '\0') { 
		return 1;	/* always return TRUE if the filter is an empty string */
	}

    /* note that pce points to a PCRE cache entry which is cleared when nec. by PCRE */
	if( (pce = pcre_get_compiled_regex_cache(filt, strlen(filt) TSRMLS_CC)) == NULL) {
		lpc_warning("Invalid lpc.filter expression '%s'.  Caching is disabled." TSRMLS_CC, filt);
		LPCG(enabled) = 0;
        return 0;
    }

	INIT_ZVAL(retval);
# ifdef ZEND_ENGINE_2_4
    php_pcre_match_impl(pce, IS_STRING, filename, strlen(filename),
# else
    php_pcre_match_impl(pce, filename, strlen(filename),
# endif
                        &retval, 0, 0, 0, 0, 0 TSRMLS_CC);

    return (Z_TYPE(retval) == IS_LONG && Z_LVAL(retval) > 0);
}
 
int lpc_generate_cache_name(lpc_request_context_t *rc TSRMLS_DC)
{ENTER(lpc_generate_cache_name)
	char *filt     = rc->cachedb_pattern;
	char *repl     = rc->cachedb_replacement;
    char *filename = rc->request_fullpath;
	pcre_cache_entry *pce;
	zval *retval,*subpats;
   /*
    * Only do replacement processing if the simple "always match" conditions don't apply. 
    */ 
	if (filt && filt[0] !='\0' && filt[2] != '\0' && repl[2] !='\0') { 

	    if( (pce = pcre_get_compiled_regex_cache(filt, strlen(filt) TSRMLS_CC)) == NULL) {
           /*
            * An invalid cache regexp pattern fails to disabling caching 
            */
		    lpc_warning("Invalid lpc.cache_pattern expression '%s'.  Caching is disabled." TSRMLS_CC, filt);
		    LPCG(enabled) = 0;
            return 0;
        }
       /*
        * Once compiled, the regexp is used on the filename to generate the subpatterns
        */
	    MAKE_STD_ZVAL(retval);
	    ALLOC_INIT_ZVAL(subpats);
    #ifdef ZEND_ENGINE_2_4
        php_pcre_match_impl(pce, IS_STRING, filename, strlen(filename),
    #else
        php_pcre_match_impl(pce, filename, strlen(filename),
    #endif                           /*    not gbl, no flags   start offset */
                            retval, subpats, 0,     0,0,        0          TSRMLS_CC);
       /*
        * if the match has generated a sub-pattern array do a simple scan of the replacement string 
        * replacing $0..$9 by subpatterns 0..9 respectively
        */ 
        if (Z_LVAL_P(retval) || (Z_TYPE_P(subpats) == IS_ARRAY)) {
		    HashTable *ht = Z_ARRVAL_P(subpats);
            char cdb[MAXPATHLEN];
            char *p = rc->cachedb_replacement, *q = cdb, *qend = &cdb[MAXPATHLEN];
            int n = strlen(p);
            int i;
            zval **pzv;
            int mode = 0;    /* 1 = last was \ escape; 2 last was $; 0 otherwise */
            memset(cdb, 0, MAXPATHLEN);

		    for (i = 0; i<n && q<qend; i++, p++) {
                if (*p == '\\') {
                    if (mode==0) {
                        mode = 1;
                    } else {
                        *q++ = '\\';
                        mode = 0;
                    }
                } else if (*p == '$' && mode != 1 && p[1]>='0' && p[1]<= '9' ) {
                    mode = 2;
                } else if (mode == 2) {
                    if (zend_hash_index_find(ht, (*p - '0'), (void **) &pzv) == SUCCESS && 
                        Z_TYPE_PP(pzv) == IS_STRING) {
                        int n = MIN( qend-q, Z_STRLEN_PP(pzv) );
                        strncpy(q, Z_STRVAL_PP(pzv), n);
                        q += n;
                    }
                    mode = 0;
                } else {
                    *q++ = *p;
                    mode = 0;
                }
            }
            rc->cachedb_fullpath = estrdup(cdb);
		    zval_ptr_dtor(&subpats);
		    zval_ptr_dtor(&retval);
            return 1;
        } else {
		    zval_ptr_dtor(&subpats);
		    FREE_ZVAL(subpats);
		    FREE_ZVAL(retval);
        }
    }
   /*
    * Default to using default naming for the CacheDB 
    */
    rc->cachedb_fullpath = emalloc(strlen(rc->request_dir) + strlen(rc->request_basename) + sizeof("./.cache\0"));
    sprintf(rc->cachedb_fullpath, "%s%c.%s.cache", 
            rc->request_dir, DEFAULT_SLASH, rc->request_basename);
    return 1;
}
/* }}} */

/* {{{ proto long lpc_atol( string str, int str_len)
	   Chain to zend_atol, except for PHP 5.2.x which doesn't handle [KMG], so in this case reimplement */
long lpc_atol(const char *str, int str_len)
{ENTER(lpc_atol)
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
            case 'g': case 'G':
                retval *= 1024;
                /* break intentionally missing */
            case 'm': case 'M':
                retval *= 1024;
                /* break intentionally missing */
            case 'k': case 'K':
                retval *= 1024;
                break;
        }
    }
    return retval;
#endif
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
