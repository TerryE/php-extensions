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

/* $Id: lpc_main.h 307215 2011-01-07 09:54:00Z gopalv $ */

#ifndef LPC_MAIN_H
#define LPC_MAIN_H

#include "lpc_pool.h"
#include "lpc_serializer.h"

/*
 * This module provides the primary interface between PHP and LPC.
 */

extern int lpc_module_init(int module_number TSRMLS_DC);
extern int lpc_module_shutdown(TSRMLS_D);
extern int lpc_process_init(int module_number TSRMLS_DC);
extern int lpc_process_shutdown(TSRMLS_D);
extern int lpc_request_init(TSRMLS_D);
extern int lpc_request_shutdown(TSRMLS_D);

typedef enum _lpc_copy_type {
    LPC_NO_COPY = 0,
    LPC_COPY_IN_OPCODE,
    LPC_COPY_OUT_OPCODE,
    LPC_COPY_IN_USER,
    LPC_COPY_OUT_USER
} lpc_copy_type;

typedef struct _lpc_context_t
{
    lpc_pool *pool;
    lpc_copy_type copy;
    unsigned int force_update:1;
} lpc_context_t;

/* {{{ struct lpc_serializer_t */
typedef struct lpc_serializer_t lpc_serializer_t;
struct lpc_serializer_t {
    const char *name;
    lpc_serialize_t serialize;
    lpc_unserialize_t unserialize;
    void *config;
};
/* }}} */

lpc_serializer_t* lpc_get_serializers(TSRMLS_D);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
