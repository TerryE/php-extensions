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

#ifndef LPC_COPY_SOURCE_H
#define LPC_COPY_SOURCE_H

/*
 * This module encapsulates most of the complexity involved in deep-copying
 * the Zend compiler data structures. The routines are allocator-agnostic, so
 * the same function can be used for copying to and from shared memory.
 */
#include "zend_compile.h"
#include "lpc.h"
#include "lpc_pool.h"
#include "lpc_cache.h"
#include "lpc_copy_function.h"
#include "lpc_copy_class.h"

/* {{{ struct definition: lpc_entry_block_t */
typedef struct _lpc_entry_block_t {  
    long halt_offset;           /* value of __COMPILER_HALT_OFFSET__ for the file */
    zend_op_array* op_array;    /* op_array allocated in shared memory */
    lpc_function_t* functions;  /* array of lpc_function_t's */
    lpc_class_t* classes;       /* array of lpc_class_t's */
    uint num_functions;         /* count of lpc_function_t's */
    uint num_classes;           /* count of lpc_class_t's */
    char *filename;             /* path to source file */
} lpc_entry_block_t;

/* }}} */

typedef zend_op_array* (zend_compile_t)(zend_file_handle*, int TSRMLS_DC);

/*
 * These are the top-level copy functions.
 */
extern zend_op_array* lpc_compile_file(zend_file_handle* h, int type TSRMLS_DC);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
