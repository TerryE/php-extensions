/*
  +----------------------------------------------------------------------+
  | LPC                                                                  |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006-2011 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt.                                 |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Gopal Vijayaraghavan <gopalv@php.net>                       | 
  +----------------------------------------------------------------------+

 */

/* $Id: $ */

#ifndef LPC_SERIALIZER_H
#define LPC_SERIALIZER_H

/* this is a shipped .h file, do not include any other header in this file */
#define LPC_SERIALIZER_NAME(module) module##_lpc_serializer
#define LPC_UNSERIALIZER_NAME(module) module##_lpc_unserializer

#define LPC_SERIALIZER_ARGS unsigned char **buf, size_t *buf_len, const zval *value, void *config TSRMLS_DC
#define LPC_UNSERIALIZER_ARGS zval **value, unsigned char *buf, size_t buf_len, void *config TSRMLS_DC

typedef int (*lpc_serialize_t)(LPC_SERIALIZER_ARGS);
typedef int (*lpc_unserialize_t)(LPC_UNSERIALIZER_ARGS);

typedef int (*lpc_register_serializer_t)(const char* name, 
                                        lpc_serialize_t serialize, 
                                        lpc_unserialize_t unserialize,
                                        void *config TSRMLS_DC);

/*
 * ABI version for constant hooks. Increment this any time you make any changes 
 * to any function in this file.
 */
#define LPC_SERIALIZER_ABI "0"
#define LPC_SERIALIZER_CONSTANT "\000apc_register_serializer-" LPC_SERIALIZER_ABI

#if !defined(LPC_UNUSED)
# if defined(__GNUC__)
#  define LPC_UNUSED __attribute__((unused))
# else 
# define LPC_UNUSED
# endif
#endif

static LPC_UNUSED int lpc_register_serializer(const char* name, 
                                    lpc_serialize_t serialize,
                                    lpc_unserialize_t unserialize,
                                    void *config TSRMLS_DC)
{
    zval *lpc_magic_constant = NULL;
    
    ALLOC_INIT_ZVAL(lpc_magic_constant);

    if (zend_get_constant(LPC_SERIALIZER_CONSTANT, sizeof(LPC_SERIALIZER_CONSTANT)-1, lpc_magic_constant TSRMLS_CC)) { 
        if(lpc_magic_constant) {
            lpc_register_serializer_t register_func = (lpc_register_serializer_t)(Z_LVAL_P(lpc_magic_constant));
            if(register_func) {
                zval_dtor(lpc_magic_constant);
                return register_func(name, serialize, unserialize, NULL TSRMLS_CC);
           }
       }
    }

    zval_dtor(lpc_magic_constant);

    return 0;
}

#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
