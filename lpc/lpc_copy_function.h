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

#ifndef LPC_COPY_FUNCTION_H
#define LPC_COPY_FUNCTION_H

/*
 * This module encapsulates most of the complexity involved in deep-copying the Zend compiler data
 * structures relating to functions. The routines are allocator-agnostic, so the same function can
 * be used for copying to and from shared memory.
 */
#include "zend_compile.h"
#include "lpc.h"
#include "lpc_pool.h"

/* {{{ struct definition: lpc_function_t */
typedef struct lpc_function_t {
    char* name;                 /* the function name */
    zend_function* function;    /* the zend function data structure */
} lpc_function_t;

extern void lpc_copy_function(zend_function* dst, zend_function* src, lpc_pool* pool);
extern void lpc_copy_new_functions(lpc_function_t* dst_array, uint count, lpc_pool* pool);
extern void lpc_install_functions(lpc_function_t* functions, uint num_functions, lpc_pool* pool);
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
