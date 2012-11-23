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

#include "lpc_zend.h"
#include "lpc_compile.h"

/* true global */
int lpc_reserved_offset;

#ifdef LPC_OPCODE_OVERRIDE

static opcode_handler_t *lpc_original_opcode_handlers;
static opcode_handler_t lpc_opcode_handlers[LPC_OPCODE_HANDLER_COUNT];

#define LPC_EX_T(offset)                    (*(temp_variable *)((char*)execute_data->Ts + offset))

#ifdef ZEND_ENGINE_2_4
static zval *lpc_get_zval_ptr(zend_uchar op_type, znode_op *node, zval **freeval, zend_execute_data *execute_data TSRMLS_DC)
{
    *freeval = NULL;

    switch (op_type) {
        case IS_CONST:
            return node->zv;
        case IS_VAR:
            return LPC_EX_T(node->var).var.ptr;
        case IS_TMP_VAR:
            return (*freeval = &LPC_EX_T(node->var).tmp_var);
        case IS_CV:
        {
            zval ***ret = &execute_data->CVs[node->var];

            if (!*ret) {
                zend_compiled_variable *cv = &EG(active_op_array)->vars[node->var];

                if (zend_hash_quick_find(EG(active_symbol_table), cv->name, cv->name_len+1, cv->hash_value, (void**)ret)==FAILURE) {
                    lpc_notice("Undefined variable: %s" TSRMLS_CC, cv->name);
                    return &EG(uninitialized_zval);
                }
            }
            return **ret;
        }
        case IS_UNUSED:
        default:
            return NULL;
    }
}
#else
static zval *lpc_get_zval_ptr(znode *node, zval **freeval, zend_execute_data *execute_data TSRMLS_DC)
{
    *freeval = NULL;

    switch (node->op_type) {
        case IS_CONST:
            return &(node->u.constant);
        case IS_VAR:
            return LPC_EX_T(node->u.var).var.ptr;
        case IS_TMP_VAR:
            return (*freeval = &LPC_EX_T(node->u.var).tmp_var);
#ifdef ZEND_ENGINE_2_1
        case IS_CV:
        {
            zval ***ret = &execute_data->CVs[node->u.var];

            if (!*ret) {
                zend_compiled_variable *cv = &EG(active_op_array)->vars[node->u.var];

                if (zend_hash_quick_find(EG(active_symbol_table), cv->name, cv->name_len+1, cv->hash_value, (void**)ret)==FAILURE) {
                    lpc_notice("Undefined variable: %s" TSRMLS_CC, cv->name);
                    return &EG(uninitialized_zval);
                }
            }
            return **ret;
        }
#endif
        case IS_UNUSED:
        default:
            return NULL;
    }
}
#endif

static int ZEND_FASTCALL lpc_op_ZEND_INCLUDE_OR_EVAL(ZEND_OPCODE_HANDLER_ARGS)
{ENTER(lpc_op_ZEND_INCLUDE_OR_EVAL)
    LPC_ZEND_OPLINE
    zval *freeop1 = NULL;
    zval *inc_filename = NULL, tmp_inc_filename;
    char realpath[MAXPATHLEN];
    php_stream_wrapper *wrapper;
    char *path_for_open;
    char *full_path = NULL;
    int ret = 0;
    lpc_opflags_t* flags = NULL;

#ifdef ZEND_ENGINE_2_4
    if (opline->extended_value != ZEND_INCLUDE_ONCE &&
        opline->extended_value != ZEND_REQUIRE_ONCE) {
        return lpc_original_opcode_handlers[LPC_OPCODE_HANDLER_DECODE(opline)](ZEND_OPCODE_HANDLER_ARGS_PASSTHRU);
    }

    inc_filename = lpc_get_zval_ptr(opline->op1_type, &opline->op1, &freeop1, execute_data TSRMLS_CC);
#else
    if (Z_LVAL(opline->op2.u.constant) != ZEND_INCLUDE_ONCE &&
        Z_LVAL(opline->op2.u.constant) != ZEND_REQUIRE_ONCE) {
        return lpc_original_opcode_handlers[LPC_OPCODE_HANDLER_DECODE(opline)](ZEND_OPCODE_HANDLER_ARGS_PASSTHRU);
    }

    inc_filename = lpc_get_zval_ptr(&opline->op1, &freeop1, execute_data TSRMLS_CC);
#endif

    if (Z_TYPE_P(inc_filename) != IS_STRING) {
        tmp_inc_filename = *inc_filename;
        zval_copy_ctor(&tmp_inc_filename);
        convert_to_string(&tmp_inc_filename);
        inc_filename = &tmp_inc_filename;
    }

    wrapper = php_stream_locate_url_wrapper(Z_STRVAL_P(inc_filename), &path_for_open, 0 TSRMLS_CC);

    if (wrapper != &php_plain_files_wrapper || !(IS_ABSOLUTE_PATH(path_for_open, strlen(path_for_open)) || (full_path = expand_filepath(path_for_open, realpath TSRMLS_CC)))) {
        /* Fallback to original handler */
        if (inc_filename == &tmp_inc_filename) {
            zval_dtor(&tmp_inc_filename);
        }
        return lpc_original_opcode_handlers[LPC_OPCODE_HANDLER_DECODE(opline)](ZEND_OPCODE_HANDLER_ARGS_PASSTHRU);
    }

    if (!full_path) {
    	full_path = path_for_open;
    }
    if (zend_hash_exists(&EG(included_files), realpath, strlen(realpath) + 1)) {
#ifdef ZEND_ENGINE_2_4
        if (!(opline->result_type & EXT_TYPE_UNUSED)) {
            ALLOC_INIT_ZVAL(LPC_EX_T(opline->result.var).var.ptr);
            ZVAL_TRUE(LPC_EX_T(opline->result.var).var.ptr);
        }
#else
        if (!(opline->result.u.EA.type & EXT_TYPE_UNUSED)) {
            ALLOC_INIT_ZVAL(LPC_EX_T(opline->result.u.var).var.ptr);
            ZVAL_TRUE(LPC_EX_T(opline->result.u.var).var.ptr);
        }
#endif
        if (inc_filename == &tmp_inc_filename) {
            zval_dtor(&tmp_inc_filename);
        }
        if (freeop1) {
            zval_dtor(freeop1);
        }
        execute_data->opline++;
        return 0;
    }

    if (inc_filename == &tmp_inc_filename) {
        zval_dtor(&tmp_inc_filename);
    }

    if(lpc_reserved_offset != -1) {
        /* Insanity alert: look into lpc_compile.c for why a void** is cast to a lpc_opflags_t* */
        flags = (lpc_opflags_t*) & (execute_data->op_array->reserved[lpc_reserved_offset]);
    }

    if(flags && flags->deep_copy == 1) {
        /* Since the op array is a local copy, we can cheat our way through the file inclusion by temporarily 
         * changing the op to a plain require/include, calling its handler and finally restoring the opcode.
         */
#ifdef ZEND_ENGINE_2_4
        opline->extended_value = (opline->extended_value == ZEND_INCLUDE_ONCE) ? ZEND_INCLUDE : ZEND_REQUIRE;
        ret = lpc_original_opcode_handlers[LPC_OPCODE_HANDLER_DECODE(opline)](ZEND_OPCODE_HANDLER_ARGS_PASSTHRU);
        opline->extended_value = (opline->extended_value == ZEND_INCLUDE) ? ZEND_INCLUDE_ONCE : ZEND_REQUIRE_ONCE;
#else
        Z_LVAL(opline->op2.u.constant) = (Z_LVAL(opline->op2.u.constant) == ZEND_INCLUDE_ONCE) ? ZEND_INCLUDE : ZEND_REQUIRE;
        ret = lpc_original_opcode_handlers[LPC_OPCODE_HANDLER_DECODE(opline)](ZEND_OPCODE_HANDLER_ARGS_PASSTHRU);
        Z_LVAL(opline->op2.u.constant) = (Z_LVAL(opline->op2.u.constant) == ZEND_INCLUDE) ? ZEND_INCLUDE_ONCE : ZEND_REQUIRE_ONCE;
#endif
    } else {
        ret = lpc_original_opcode_handlers[LPC_OPCODE_HANDLER_DECODE(opline)](ZEND_OPCODE_HANDLER_ARGS_PASSTHRU);
    }

    return ret;
}

void lpc_zend_init(TSRMLS_D)
{
    zend_extension dummy_ext;
    lpc_reserved_offset = zend_get_resource_handle(&dummy_ext); 
    assert(lpc_reserved_offset == dummy_ext.resource_number);
    assert(lpc_reserved_offset != -1);
    assert(sizeof(lpc_opflags_t) <= sizeof(void*));
    if (!LPCG(include_once)) {
        /* If we're not overriding the INCLUDE_OR_EVAL handler, then just skip this malarkey */
        return;
    }

    memcpy(lpc_opcode_handlers, zend_opcode_handlers, sizeof(lpc_opcode_handlers));

    /* 5.0 exposes zend_opcode_handlers differently than 5.1 and later */
#ifdef ZEND_ENGINE_2_1
    lpc_original_opcode_handlers = zend_opcode_handlers;
    zend_opcode_handlers = lpc_opcode_handlers;
#else
    lpc_original_opcode_handlers = lpc_opcode_handlers;
#endif

    LPC_REPLACE_OPCODE(ZEND_INCLUDE_OR_EVAL);
}

void lpc_zend_shutdown(TSRMLS_D)
{ENTER(lpc_zend_shutdown)
    if (!LPCG(include_once)) {
        /* Nothing changed, nothing to restore */
        return;
    }

#ifdef ZEND_ENGINE_2_1
    zend_opcode_handlers = lpc_original_opcode_handlers;
#else
    memcpy(zend_opcode_handlers, lpc_original_opcode_handlers, sizeof(lpc_opcode_handlers));
#endif
}

#else /* Opcode Overrides unavailable */

void lpc_zend_init(TSRMLS_D) { }
void lpc_zend_shutdown(TSRMLS_D) { }

#endif /* LPC_OPCODE_OVERRIDE */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
