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
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Community Connect Inc. in 2002
   and revised in 2005 by Yahoo! Inc. to add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.
*/

#ifndef LPC_ZEND_H
#define LPC_ZEND_H

/* Utilities for interfacing with the zend engine */

#include "lpc.h"
#include "lpc_php.h"

#ifndef Z_REFCOUNT_P
#define Z_REFCOUNT_P(pz)              (pz)->refcount
#define Z_REFCOUNT_PP(ppz)            Z_REFCOUNT_P(*(ppz))
#endif

#ifndef Z_SET_REFCOUNT_P
#define Z_SET_REFCOUNT_P(pz, rc)      (pz)->refcount = rc
#define Z_SET_REFCOUNT_PP(ppz, rc)    Z_SET_REFCOUNT_P(*(ppz), rc)
#endif

#ifndef Z_ADDREF_P
#define Z_ADDREF_P(pz)                (pz)->refcount++
#define Z_ADDREF_PP(ppz)              Z_ADDREF_P(*(ppz))
#endif

#ifndef Z_DELREF_P
#define Z_DELREF_P(pz)                (pz)->refcount--
#define Z_DELREF_PP(ppz)              Z_DELREF_P(*(ppz))
#endif

#ifndef Z_ISREF_P
#define Z_ISREF_P(pz)                 (pz)->is_ref
#define Z_ISREF_PP(ppz)               Z_ISREF_P(*(ppz))
#endif

#ifndef Z_SET_ISREF_P
#define Z_SET_ISREF_P(pz)             (pz)->is_ref = 1
#define Z_SET_ISREF_PP(ppz)           Z_SET_ISREF_P(*(ppz))
#endif

#ifndef Z_UNSET_ISREF_P
#define Z_UNSET_ISREF_P(pz)           (pz)->is_ref = 0
#define Z_UNSET_ISREF_PP(ppz)         Z_UNSET_ISREF_P(*(ppz))
#endif

#ifndef Z_SET_ISREF_TO_P
#define Z_SET_ISREF_TO_P(pz, isref)   (pz)->is_ref = isref
#define Z_SET_ISREF_TO_PP(ppz, isref) Z_SET_ISREF_TO_P(*(ppz), isref)
#endif

extern void lpc_zend_init(TSRMLS_D);
extern void lpc_zend_shutdown(TSRMLS_D);

/* offset for lpc info in op_array->reserved */
extern int lpc_reserved_offset;

#ifndef ZEND_VM_KIND_CALL /* Not currently defined by any ZE version */
# define ZEND_VM_KIND_CALL  1
#endif

#ifndef ZEND_VM_KIND /* Indicates PHP < 5.1 */
# define ZEND_VM_KIND   ZEND_VM_KIND_CALL
#endif

#if defined(ZEND_ENGINE_2) && (ZEND_VM_KIND == ZEND_VM_KIND_CALL)
# define LPC_OPCODE_OVERRIDE
#endif

#ifdef LPC_OPCODE_OVERRIDE

#ifdef ZEND_ENGINE_2_1
/* Taken from Zend/zend_vm_execute.h */
#define _CONST_CODE  0
#define _TMP_CODE    1
#define _VAR_CODE    2
#define _UNUSED_CODE 3
#define _CV_CODE     4
static inline int _lpc_opcode_handler_decode(zend_op *opline)
{
    static const int lpc_vm_decode[] = {
        _UNUSED_CODE, /* 0              */
        _CONST_CODE,  /* 1 = IS_CONST   */
        _TMP_CODE,    /* 2 = IS_TMP_VAR */
        _UNUSED_CODE, /* 3              */
        _VAR_CODE,    /* 4 = IS_VAR     */
        _UNUSED_CODE, /* 5              */
        _UNUSED_CODE, /* 6              */
        _UNUSED_CODE, /* 7              */
        _UNUSED_CODE, /* 8 = IS_UNUSED  */
        _UNUSED_CODE, /* 9              */
        _UNUSED_CODE, /* 10             */
        _UNUSED_CODE, /* 11             */
        _UNUSED_CODE, /* 12             */
        _UNUSED_CODE, /* 13             */
        _UNUSED_CODE, /* 14             */
        _UNUSED_CODE, /* 15             */
        _CV_CODE      /* 16 = IS_CV     */
    };
#ifdef ZEND_ENGINE_2_4
    return (opline->opcode * 25) + (lpc_vm_decode[opline->op1_type] * 5) + lpc_vm_decode[opline->op2_type];
#else
    return (opline->opcode * 25) + (lpc_vm_decode[opline->op1.op_type] * 5) + lpc_vm_decode[opline->op2.op_type];
#endif
}

# define LPC_ZEND_OPLINE                    zend_op *opline = execute_data->opline;
# define LPC_OPCODE_HANDLER_DECODE(opline)  _lpc_opcode_handler_decode(opline)
# if PHP_MAJOR_VERSION >= 6
#  define LPC_OPCODE_HANDLER_COUNT          ((25 * 152) + 1)
# elif defined(ZEND_ENGINE_2_4)
#  define LPC_OPCODE_HANDLER_COUNT          ((25 * 157) + 1) /* 3 new opcodes in 5.4? - separate, bind_trais, add_trait */
# elif PHP_MAJOR_VERSION >= 5 && PHP_MINOR_VERSION >= 3
#  define LPC_OPCODE_HANDLER_COUNT          ((25 * 154) + 1) /* 3 new opcodes in 5.3 - unused, lambda, jmp_set */
# else
#  define LPC_OPCODE_HANDLER_COUNT          ((25 * 151) + 1)
# endif
# define LPC_REPLACE_OPCODE(opname)         { int i; for(i = 0; i < 25; i++) if (zend_opcode_handlers[(opname*25) + i]) zend_opcode_handlers[(opname*25) + i] = lpc_op_##opname; }

#else /* ZE2.0 */
# define LPC_ZEND_ONLINE
# define LPC_OPCODE_HANDLER_DECODE(opline)  (opline->opcode)
# define LPC_OPCODE_HANDLER_COUNT           512
# define LPC_REPLACE_OPCODE(opname)         zend_opcode_handlers[opname] = lpc_op_##opname;
#endif

#ifndef ZEND_FASTCALL  /* Added in ZE2.3.0 */
#define ZEND_FASTCALL
#endif

/* Added in ZE2.3.0 */
#ifndef zend_parse_parameters_none
# define zend_parse_parameters_none() zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "")
#endif


#endif  /* LPC_OPCODE_OVERRIDE */

#ifdef ZEND_ENGINE_2_4
# define ZEND_CE_FILENAME(ce)			(ce)->info.user.filename
# define ZEND_CE_DOC_COMMENT(ce)        (ce)->info.user.doc_comment
# define ZEND_CE_DOC_COMMENT_LEN(ce)	(ce)->info.user.doc_comment_len
# define ZEND_CE_BUILTIN_FUNCTIONS(ce)  (ce)->info.internal.builtin_functions
#else
# define ZEND_CE_FILENAME(ce)			(ce)->filename
# define ZEND_CE_DOC_COMMENT(ce)        (ce)->doc_comment
# define ZEND_CE_DOC_COMMENT_LEN(ce)	(ce)->doc_comment_len
# define ZEND_CE_BUILTIN_FUNCTIONS(ce)  (ce)->builtin_functions
#endif

#endif  /* LPC_ZEND_H */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
