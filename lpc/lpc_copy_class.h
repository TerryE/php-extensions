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

#ifndef LPC_COPY_CLASS_H
#define LPC_COPY_CLASS_H

/*
 * This module encapsulates most of the complexity involved in deep-copying
 * the Zend compiler data structures. The routines are allocator-agnostic, so
 * the same function can be used for copying to and from shared memory.
 */
#include "zend_compile.h"
#include "lpc.h"
#include "lpc_pool.h"

/* {{{ struct definition: lpc_class_t */
typedef struct lpc_class_t {
    char* name;                     /* the class name */
    int name_len;                   /* length of name */
    char* parent_name;              /* the parent class name */
    zend_class_entry* class_entry;  /* the zend class data structure */
} lpc_class_t;
/* }}} */

/*
 * These are the top-level class copy functions.
 */
extern void      lpc_copy_class_entry(zend_class_entry* dst, zend_class_entry* src, lpc_pool* pool);
extern void      lpc_copy_new_classes(lpc_class_t* cl_array, zend_uint count, lpc_pool* pool);
extern zend_bool lpc_install_classes(lpc_class_t* classes, zend_uint num_classes, lpc_pool* pool);
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
