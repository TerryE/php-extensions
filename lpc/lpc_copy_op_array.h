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

#ifndef LPC_COPY_OP_ARRAY_H
#define LPC_COPY_OP_ARRAY_H

/*
 * This module encapsulates most of the complexity involved in deep-copying
 * the Zend compiler data structures. The routines are allocator-agnostic, so
 * the same function can be used for copying to and from shared memory.
 */
#include "zend_compile.h"
#include "lpc.h"
#include "lpc_pool.h"

/* {{{ struct definition: lpc_opflags_t */
typedef zend_ushort lpc_opflags_t;

#define LPC_FLAG_HAS_JUMPS      1<<0 /* has jump offsets */

   /* autoglobal bits */
#define LPC_FLAG__POST          1<<1
#define LPC_FLAG__GET           1<<2
#define LPC_FLAG__COOKIE        1<<3
#define LPC_FLAG__SERVER        1<<4
#define LPC_FLAG__ENV           1<<5
#define LPC_FLAG__FILES         1<<6
#define LPC_FLAG__REQUEST       1<<7
#define LPC_FLAG__SESSION       1<<8
#define LPC_FLAG_JIT_GLOBAL     0b111111110 /* ie. a mask for all of the FLAGS__* */
#ifdef ZEND_ENGINE_2_4
#  define LPC_FLAG_GLOBALS      1<<10
#endif
#define LPC_FLAG_UNKNOWN_GLOBAL 1<<11
        
/* }}} */

/* {{{ Top-level copy functions for op-array and common */
extern void   lpc_copy_op_array(zend_op_array* dst, zend_op_array* src, lpc_pool* pool);
extern void   lpc_copy_zval_ptr(zval** pdst, const zval** psrc, lpc_pool* pool);
extern void   lpc_copy_arg_info_array(zend_arg_info** pdst, const zend_arg_info* src,
                                      uint num_args, lpc_pool* pool);
/* }}} */

/*
 * copy assistance macros used by all of the _copy_ implementations 
 */
#define ALLOC_IF_NULL(dst)  if (!(dst)) pool_alloc(dst,sizeof(*dst));
#define BITWISE_COPY(src,dst) assert(src != NULL); memcpy(dst, src, sizeof(*src));
#define CHECK(p) if(!(p)) goto error

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
