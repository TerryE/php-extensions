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

#ifndef LPC_REQUEST_H
#define LPC_REQUEST_H

typedef struct _lpc_request_context_t lpc_request_context_t;

struct _lpc_request_context_t {
    char         *request_fullpath;
    char         *request_dir;
    char         *request_basename;
    char         *filter;
    char         *cachedb_pattern;
    char         *cachedb_replacement;
    char         *cachedb_fullpath;
    char         *PHP_version;
    time_t        request_mtime;
    size_t        request_filesize;
    int           clear_flag_set;
};

/*
 * This module provides the primary interface between PHP and LPC.
 */

extern int lpc_module_init(int module_number TSRMLS_DC);
extern int lpc_module_shutdown(TSRMLS_D);
extern int lpc_request_init(TSRMLS_D);
extern int lpc_request_shutdown(TSRMLS_D);

/* pointer to the original Zend engine compile_file function */
typedef zend_op_array* (zend_compile_t)(zend_file_handle*, int TSRMLS_DC);
extern zend_compile_t* lpc_set_compile_hook(zend_compile_t *ptr);
void lpc_dtor_request_context(TSRMLS_D);

#endif
/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
