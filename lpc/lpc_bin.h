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
  | Authors: Brian Shire <shire@php.net>                                 |
  +----------------------------------------------------------------------+

 */

/* $Id: lpc_bin.h 307048 2011-01-03 23:53:17Z kalle $ */

#ifndef LPC_BINDUMP_H
#define LPC_BINDUMP_H

#include "lpc.h"
#include "lpc_php.h"
#include "ext/standard/basic_functions.h"

/* LPC binload flags */
#define LPC_BIN_VERIFY_MD5    1 << 0
#define LPC_BIN_VERIFY_CRC32  1 << 1

typedef struct _lpc_bd_entry_t {
    unsigned char type;
    uint num_functions;
    uint num_classes;
    lpc_cache_entry_value_t val;
} lpc_bd_entry_t;

typedef struct _lpc_bd_t {
    unsigned int size;
    int swizzled;
    unsigned char md5[16];
    php_uint32 crc;
    unsigned int num_entries;
    lpc_bd_entry_t *entries;
    int num_swizzled_ptrs;
    void ***swizzled_ptrs;
} lpc_bd_t;

lpc_bd_t* lpc_bin_dump(HashTable *files TSRMLS_DC);
int lpc_bin_load(lpc_bd_t *bd, int flags TSRMLS_DC);

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
