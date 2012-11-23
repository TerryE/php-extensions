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
#include "lpc_zend.h"

/* Only implement regexp filters for PHP versions >= 5.2.2 otherwise it acts as a Noop */
#ifdef PHP_REXEP_OK
#  include "ext/pcre/php_pcre.h"
#  include "ext/standard/php_smart_str.h"
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

#ifdef __DEBUG_LPC__
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
   *  REGEXs are only supported for PHP versions >= 5.2.2 and ignored otherwise
   *  The PHP regexp engine goes to great lengths to cache REGEX compiles so don't redo this!
   *  This is now thread aware and so can directly access the LPC global vector 
   *  Only a single expression is used because normal REGEX syntax (alternative selections 
      and negative assertion) can be used do all of the previous APC REGEX functions.
*/

int lpc_valid_file_match(const char *filename TSRMLS_DC)
{ENTER(lpc_valid_file_match)
#ifdef PHP_REXEP_OK

	char *filt = LPCG(filter);
	pcre *re;

	/* handle the simple "always match and always fail conditions. */ 
	if (filt[0] == '\0' || filt[2] == '\0') { 
		return 1;	/* always return TRUE if the filter is an empty string */
	} else if (filt[3] == '\0' && filt[1]=='*') {
		return 0;   /* always return FALSE if the filter is "<delim>*<delim>" */
	}

	CHECK(re = pcre_get_compiled_regex(filt, NULL, NULL TSRMLS_CC));

	return (pcre_exec(re, NULL, (filename), strlen(filename), 0, 0, NULL, 0) >= 0) ? 1 : 0;
		
#else /* PHP_REXEP_OK */

	return 1;  /* if PHP version < 5.2.2 then ignore filtering and always return TRUE */

#endif /* PHP_REXEP_OK */

error:
	lpc_warning("Invalid lpc.filter, '%s'" TSRMLS_CC, filt);
	efree(LPCG(filter));
	LPCG(filter)="";  /* set the filter to an empty string: no point in repeating error */ 
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
