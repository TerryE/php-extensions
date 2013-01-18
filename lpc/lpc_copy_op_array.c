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
  | Authors: Terry Ellison <Terry@ellisons.org.uk>                       |
  +----------------------------------------------------------------------+

   This software was derived from the APC extension which was initially 
   contributed to PHP by Community Connect Inc. in 2002 and revised in 2005 
   by Yahoo! Inc. See README for further details.

   All other licensing and usage conditions are those of the PHP Group.
*/

#include "lpc.h"
#include "lpc_copy_op_array.h"
#include "lpc_op_table.h"
#include "lpc_hashtable.h"
#include "lpc_copy_function.h"
#include "lpc_copy_class.h"
#include "lpc_string.h"
#include "ext/standard/php_var.h"

/*
 * Opcode processing has two variants: one for copy-out and one for copy-in. LPC needs to carry out
 * various integrity checks and one-off per op_array processing. Copy-out once-per-compile and
 * copy-in once-per-request, so it makes sense to hoist as much of this processing as possible to
 * the copy-out phase and keep copy-in as lean as possible.
 *
 * The main function lpc_copy_op_array uses helper routines for copying zvals. As these are minimal
 * in the case of copy-in and runtime is important, the copy-in variants are either inlined or
 * declared as inline scope. The copy out variants are explicit functions.
 */
 
static void copy_zval_out(zval* dst, const zval* src, lpc_pool* pool);
static void copy_opcodes_out(zend_op_array *dst, zend_op_array *src, lpc_pool* pool);
/* The public lpc_copy_op_array and lpc_copy_zval_ptr templates are in lpc_copy_op_array.h */

/*
 * Some helper macros to abbreviate the source code.  Note that the if(src)... macros don't need
 * the corresponding "else src=NULL" because if the src is NULL then the base bitwise copy does 
 * this for you already.
 */ 
#define TAG_SETPTR(dst,src) dst = src; pool_tag_ptr(dst);
#define POOL_STRDUP_FLD(fld) if(src->fld) pool_strdup(dst->fld,src->fld, 0)
#define POOL_ESTRDUP_FLD(fld) if(src->fld) pool_strdup(dst->fld,src->fld, 1)
#define POOL_MEMCPY(dst,src,n) if(src) pool_memcpy(dst,src,n)
#define POOL_MEMCPY_FLD(fld,n) POOL_MEMCPY(dst->fld,src->fld,n)
#define POOL_NSTRDUP_FLD(fld) if (src->fld) \
    pool_nstrdup(dst->fld, dst->fld##_len, src->fld, src->fld##_len, 0)
#define POOL_ENSTRDUP(dst,src) if (src) \
    pool_nstrdup(dst, dst##_len, src, src##_len, 1)
#define POOL_ENSTRDUP_FLD(fld) POOL_ENSTRDUP(dst->fld,src->fld)

/*
 * Whereas the changes introduced in 2.1 to 2.3 Zend engines were mostly additional 
 * attributes, the zend_op structure changed significantly at 2.4.  The following 
 * macro variants for 2.4 and pre 2.4 encapsulate these changes to permit the use of 
 * a common code base to handle most of the zend_op processing.
 */
#ifdef ZEND_ENGINE_2_4

//////////  TODO: These 2.4 macros haven't been debugged yet
#  define COPY_ZNODE_IN_IF_CONSTANT(fld) if (dst_zo->fld ## _type == IS_CONST) \
    dst_zo->fld.literal = src_zo->fld.literal - src->literals + dst->literals
#  define COPY_ZNODE_OUT_IF_CONSTANT(fld) COPY_ZNODE_IN_IF_CONSTANT(fld)
#  define CONST_PZV(zo_op) zo_op.zv
#  define ZO_EA_IS_FETCH_GLOBAL(zo_op) ((zo_op->extended_value & ZEND_FETCH_TYPE_MASK) == ZEND_FETCH_GLOBAL)
#  define ZOP_TYPE_IS_CONSTANT_STRING(zo_op) \
    (zo_op ## _type == IS_CONST) && (Z_TYPE_P(zo_op.zv) == IS_STRING)
#  define IS_AUTOGLOBAL_SETFLAG(member) (!strcmp(Z_STRVAL_P(zo->op1.zv), #member)) {flags->member = 1;}
#  define ZOP_TYPE_IS_CONSTANT_ARRAY(zo_op) \
    (zo_op ## _type == IS_CONST && Z_TYPE_P(zo_op.zv) == IS_CONSTANT_ARRAY)
#  define JUMP_ADDR(op) op.jmp_addr

#else

#  define COPY_CONST_ZVAL_IN_IF_CONST(fld) if (dst_zo->fld.op_type == IS_CONST) \
    { copy_zval_in(&dst_zo->fld.u.constant, &src_zo->fld.u.constant, pool); }
#  define COPY_CONST_ZVAL_OUT_IF_CONST(fld) if (dst_zo->fld.op_type == IS_CONST) \
    { copy_zval_out(&dst_zo->fld.u.constant, &src_zo->fld.u.constant, pool); }
#  define CONST_PZV(zo_op) &(zo_op.u.constant)
#  define ZO_EA_IS_FETCH_GLOBAL(zo_op) (zo_op->op2.u.EA.type == ZEND_FETCH_GLOBAL)
#  define ZOP_TYPE_IS_CONSTANT_STRING(zo_op) \
    (zo_op.op_type == IS_CONST) && (Z_TYPE(zo_op.u.constant) == IS_STRING)
#  define IS_AUTOGLOBAL_SETFLAG(member) (!strcmp(Z_STRVAL_P(const_pzv), #member)) {flags->member = 1;}
#  define ZOP_TYPE_IS_CONSTANT_ARRAY(zo_op) \
    (zo_op.op_type == IS_CONST && Z_TYPE(zo_op.u.constant) == IS_CONSTANT_ARRAY)
#  define JUMP_ADDR(op) op.u.jmp_addr

#endif

/* {{{ lpc_copy_zval 
       Copy zval explicitly copying only used fields */
static void copy_zval_out(zval* dst, const zval* src, lpc_pool* pool)
{ENTER(copy_zval_out)

    memset(dst, 0, sizeof(zval));
   /*
    * APC uses a hashTable APCG(copied_zvals) area, and inserting each zval ref into this and 
    * exiting on repeat insert. This adds a hash insert to every zval copy.  LPC uses a more 
    * runtime-efficient strategy: this kind of cyclic referencing shouldn't occur in generated code,
    * so a simple countdown-based method based on a maximum number of zval copies per zend_op is
    * used to detect long cycles and abort the copy with a fatal error if this occurs.
    */
    if (LPCGP(copied_zvals)-- == 0) {
        lpc_error("Fatal: Circular reference found in compiled zvals " TSRMLS_PC ZEND_FILE_LINE_CC);
        zend_bailout();
    }

   /* code uses refcount=2 for consts */
    Z_SET_REFCOUNT_P(dst, Z_REFCOUNT_P((zval*)src));
    Z_SET_ISREF_TO_P(dst, Z_ISREF_P((zval*)src));
    Z_TYPE_P(dst) = Z_TYPE_P((zval*)src);

    switch (Z_TYPE_P(src) & IS_CONSTANT_TYPE_MASK) {

        case IS_BOOL:
            Z_LVAL_P(dst) = Z_BVAL_P(src) != 0;
            break;

        case IS_RESOURCE:
        case IS_LONG:
            Z_LVAL_P(dst) = Z_LVAL_P(src);
            break;

        case IS_DOUBLE:
            Z_DVAL_P(dst) = Z_DVAL_P(src);
            break;

        case IS_NULL:
            break;

        case IS_CONSTANT:
        case IS_STRING:
            if (Z_STRVAL_P(src)) {
#ifdef LPC_DEBUG
                if (LPCGP(debug_flags)&LPC_DBG_ZVAL)  /* Intern tracking */
                    lpc_debug("ZVAL copy-out type %u:%u/%u %08lx" TSRMLS_PC,
                              Z_TYPE_P((zval*)src), Z_ISREF_P((zval*)src),
                              Z_REFCOUNT_P((zval*)src), Z_STRVAL_P((zval*)src));
#endif
                pool_nstrdup(Z_STRVAL_P(dst), Z_STRLEN_P(dst), Z_STRVAL_P(src), Z_STRLEN_P(src), 0);
            }
            break;

        case IS_ARRAY:
        case IS_CONSTANT_ARRAY:
#ifdef LPC_DEBUG
                if (LPCGP(debug_flags)&LPC_DBG_ZVAL)  /* Intern tracking */
                    lpc_debug("ZVAL copy-out HASH %u" TSRMLS_PC, src->value.ht->nNumOfElements);
#endif
            pool_alloc_ht(Z_ARRVAL_P(dst));
            COPY_HT_P(value.ht, lpc_copy_zval_ptr, zval *, NULL, NULL);
            break;

        case IS_OBJECT: 
            dst->type = IS_NULL;
            break;

        default:
            assert(0);
    }
}
/* }}} */

/* {{{ copy_zval_in 
    Note that this an absolute lightweight version for copy-in inlining of zvals as used in compiled
    opcodes. It assume that the underlying zval data has already been bitwise copied. 
    No circular reference detection is employed:  this would have been picked up at copy out. */
static zend_always_inline void copy_zval_in(zval* dst, const zval* src, lpc_pool* pool)
{
    memcpy(dst, src, sizeof(zval));

    /* code uses refcount=2 for consts */
    Z_SET_REFCOUNT_P(dst, Z_REFCOUNT_P((zval*)src));
    Z_SET_ISREF_TO_P(dst, Z_ISREF_P((zval*)src));

    switch (Z_TYPE_P(src) & IS_CONSTANT_TYPE_MASK) {
        case IS_CONSTANT:
        case IS_STRING:
            if (Z_STRVAL_P(src)) {
#ifdef LPC_DEBUG
                if (LPCGP(debug_flags)&LPC_DBG_ZVAL)  /* Intern tracking */
                    lpc_debug("ZVAL copy-in type %u:%u/%u %08lx" TSRMLS_PC,
                              Z_TYPE_P((zval*)src), Z_ISREF_P((zval*)src),
                              Z_REFCOUNT_P((zval*)src), Z_STRVAL_P((zval*)src));
#endif
                pool_nstrdup(Z_STRVAL_P(dst), Z_STRLEN_P(dst), Z_STRVAL_P(src), Z_STRLEN_P(src), 1);
            }
            break;

        case IS_ARRAY:
        case IS_CONSTANT_ARRAY:
#ifdef LPC_DEBUG
            if (LPCGP(debug_flags)&LPC_DBG_ZVAL)  /* Intern tracking */
                lpc_debug("ZVAL copy-in HASH %u" TSRMLS_PC, src->value.ht->nNumOfElements);
#endif
            pool_alloc_ht(Z_ARRVAL_P(dst));
            COPY_HT_P(value.ht, lpc_copy_zval_ptr, zval *, NULL, NULL);
            break;

        default:
            break;
    }
}
/* }}} */

/* {{{ zend_vm_get_opcode_handler
       This is a copy of Zend/zend_vm_execute.c:zend_vm_get_opcode_handler() */
#define _CONST_CODE  0
#define _TMP_CODE    1
#define _VAR_CODE    2
#define _UNUSED_CODE 3
#define _CV_CODE     4
static const int zend_vm_decode[] = {
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

static zend_always_inline opcode_handler_t zend_vm_get_opcode_handler(zend_op* op)
{
		return zend_opcode_handlers[op->opcode * 25 + zend_vm_decode[op->op1.op_type] * 5 + zend_vm_decode[op->op2.op_type]];
}
/* }}} */

#define COPY_FLD(fld) dst_zo->fld = src_zo->fld
#define COPY_ZNODE(node) copy_znode_out(&dst_zo->node, &src_zo->node,pool)
#define IS_AG_THEN_SET_FLAG(initial,var) \
(Z_STRVAL_P(const_pzv)[1] == initial && !strcmp(Z_STRVAL_P(const_pzv), #var)) flags|=LPC_FLAG_##var

/* {{{ copy_opcodes_out */
static void copy_opcodes_out(zend_op_array *dst, zend_op_array *src, lpc_pool* pool)
{ENTER(copy_opcodes_out)
   /*
    * Now do the remaining deep copy, relocations and fixups of the opcode array.  The copy in and
    * out versions are different for the reasons explained above. T he copy-out is out-of-lined
    * to simply code structure this also sets the LPC opflags which apply to this op_array.  These 
    * flags are kept in an op_array reserved field. lpc_zend_init() calls zend_get_resource_handle()
    * to allocate this field and set lpc_reserved_offset.  As this is the same offset for all LPC 
    * threads, this can be and is a true global.
    */
    lpc_opflags_t flags = (lpc_opflags_t) ((size_t) dst->reserved[lpc_reserved_offset]);
    zend_op *src_zo, *dst_zo;
    zend_op *src_zo_last = src->opcodes + src->last;
    TSRMLS_FETCH_FROM_POOL();

    pool_memcpy(dst->opcodes, src->opcodes, sizeof(zend_op) * src->last);

    for (src_zo = src->opcodes, dst_zo = dst->opcodes; 
         src_zo < src_zo_last; 
         src_zo++, dst_zo++) {

#ifdef LPC_DEBUG
    if (LPCGP(debug_flags)&LPC_DBG_LOG_OPCODES) 
        php_stream_printf(LPCG(opcode_logger) TSRMLS_CC, "%u,%u,%u\n", src_zo->opcode,
                          zend_vm_decode[src_zo->op1.op_type], zend_vm_decode[src_zo->op2.op_type]);
#endif

       if( src_zo->handler == zend_vm_get_opcode_handler(src_zo)) {
            dst_zo->handler = NULL;
        }

        zend_uchar opcode_flags = opcode_table[src_zo->opcode];
        LPCGP(copied_zvals) = LPC_COPIED_ZVALS_COUNTDOWN;
       /*
        * Using constant relative paths generates a path search per include at runtime which is bad 
        * news for performance so if lpc.resolve_paths is set, convert these to absolute paths by 
        * resolving against the include_path. This is done to the SOURCE constant -- hence why it
        * is done BEFORE the op1 copy.
        */

        if (LPCG(resolve_paths) && 
            (opcode_flags & LPC_OP_INCLUDE) && 
            ZOP_TYPE_IS_CONSTANT_STRING(src_zo->op1)) {

            zval *const_pzv = CONST_PZV(src_zo->op1);
            zval *include_type = CONST_PZV(src_zo->op2);           
            char *resolved_path;

            if (Z_LVAL_P(include_type) != ZEND_EVAL &&
                (resolved_path = lpc_resolve_path(const_pzv TSRMLS_CC))) {
               /*
                * If the path is not absolute or explicitly current dir relative, then it is 
                * resolved against the include_path, script path and current working directory. The
                * op1 parameter is then overwritten with the new fullpath constant.  This leaves
                * the old string dangling in the serial pool, but what the hell. 
                */
#ifdef ZEND_ENGINE_2_4
//////////// TODO: this all needs revisiting as part of 2.4 regression
                pool_alloc(dst_zo->op1.literal, sizeof(zend_literal));
                Z_STRLEN_P(const_pzv) = strlen(resolved_path);
                pool_memcpy(Z_STRVAL_P(const_pzv), resolved_path, Z_STRLEN_P(const_pzv)+1);
                Z_SET_REFCOUNT_P(const_pzv, 2);
                Z_SET_ISREF_P(const_pzv);
                dst_zo->op1.literal->hash_value = zend_hash_func(Z_STRVAL_P(const_pzv), 
                                                                 Z_STRLEN_P(const_pzv) + 1);
#else
//////////// TODO:  may need more checking v.v. LVAL clone rather than overwrite
                efree(Z_STRVAL_P(const_pzv));
                Z_STRLEN_P(const_pzv) = strlen(resolved_path);
                Z_STRVAL_P(const_pzv) = resolved_path;
#endif
            }
        }
#ifdef ZEND_ENGINE_2_4
////// TODO: APC treat this as a bitwise copy but need to check this and add this code.
#else
        COPY_CONST_ZVAL_OUT_IF_CONST(result);
        COPY_CONST_ZVAL_OUT_IF_CONST(op1);
        COPY_CONST_ZVAL_OUT_IF_CONST(op2);
#endif
        if (opcode_flags & LPC_OP_JMP_OP1) {
            JUMP_ADDR(dst_zo->op1) = dst->opcodes + (JUMP_ADDR(src_zo->op1) - src->opcodes);
            pool_tag_ptr(JUMP_ADDR(dst_zo->op1));
            flags |= LPC_FLAG_HAS_JUMPS;
                
        } else if (opcode_flags & LPC_OP_JMP_OP2) {
            JUMP_ADDR(dst_zo->op2) = dst->opcodes + (JUMP_ADDR(src_zo->op2) - src->opcodes);
            pool_tag_ptr(JUMP_ADDR(dst_zo->op2));
            flags |= LPC_FLAG_HAS_JUMPS;

        } else if((opcode_flags & LPC_OP_FETCH_OUT) &&
                PG(auto_globals_jit) &&
                ZOP_TYPE_IS_CONSTANT_STRING(src_zo->op1) &&
                ZO_EA_IS_FETCH_GLOBAL(src_zo)) {
           /*
            * Some extra functionality is needed to support the auto_globals_jit INI parameter. When
            * this is enabled, $_SERVER and related variables are JiT created when first referenced
            * instead of at request initiation -- some extra complexity for a small performance
            * gain. Loading an op array which refers to must trigger this process, hence the
            * appropriate flag bits are set during the copy-out scan to simplify this at copy-in. 
            */  
            zval *const_pzv = CONST_PZV(src_zo->op1);

            if (Z_STRLEN_P(const_pzv) > 1 && 
                Z_STRVAL_P(const_pzv)[0] == '_') {
                if IS_AG_THEN_SET_FLAG('G',_GET);
                else if IS_AG_THEN_SET_FLAG('P',_POST);
                else if IS_AG_THEN_SET_FLAG('C',_COOKIE);
                else if IS_AG_THEN_SET_FLAG('S',_SERVER);
                else if IS_AG_THEN_SET_FLAG('E',_ENV);
                else if IS_AG_THEN_SET_FLAG('F',_FILES);
                else if IS_AG_THEN_SET_FLAG('R',_REQUEST);
                else if IS_AG_THEN_SET_FLAG('S',_SESSION);
                else if (zend_is_auto_global(Z_STRVAL_P(const_pzv), Z_STRLEN_P(const_pzv) TSRMLS_CC)) {
                    flags &= LPC_FLAG_UNKNOWN_GLOBAL;
                }
            }
#ifdef ZEND_ENGINE_2_4     /* in Zend 2.4 GLOBALS is also treated as a JiT variable */
            else IS_AG_THEN_SET_FLAG('L',GLOBALS);
#endif
        } else {
            if(!(opcode_flags & LPC_OP_OK)) {
                assert(0);   /* something is very wrong if we get here */
            }   
        }
    } /* for each zend_op */

    dst->reserved[lpc_reserved_offset] = (void *)((size_t)flags);
}
#undef COPY_ZNODE
#undef IS_AG_THEN_SET_FLAG
/* }}} */

/* {{{ lpc_copy_op_array */
void lpc_copy_op_array(zend_op_array* dst, zend_op_array* src, lpc_pool* pool)
{ENTER(lpc_copy_op_array)
    zend_uint i;
    TSRMLS_FETCH_FROM_POOL();
/*
 * The op_array deep copy follows the usual pattern: bitwise copy the base record to copy all 
 * scalar fields, and handle all referenced fields on a case-by-case basis.  Most are pointers to
 * immutable data (such as strings) that can be simply pool_memcpy'ed.  Some like the scope and 
 * arg_info are only set within the context of an op_array associated with a class or function and 
 * set by that class / function copy.  One of the reserved fields is used to hold the LPC flags. 
 * Here is the summary of the "rich" fields.
 *   
 *  char                   *function_name   [E] pool_memcpy if not null           
 *  zend_class_entry       *scope           [R] set by owning class copy if class op array 
 *  union _zend_function   *prototype       [R] nulled as zend_do_inheritance will re-look this up               
 *  zend_arg_info          *arg_info        [E] deep pool_memcpy if not nul                           
 *  zend_uint              *refcount        [E] pool_memcpy if not nul                          
 *  zend_op                *opcodes         [E] See below                 
 *  zend_compiled_variable *vars            [E] deep copy the array using pool_memcpy                    
 *  zend_brk_cont_element  *brk_cont_array  [E] pool_memcpy if not nul                                  
 *  zend_try_catch_element *try_catch_array [E] pool_memcpy if not nul                                  
 *  HashTable              *static_variables[R] HT copy                            
 *  zend_op                *start_op        [R] processed with opcodes     
 *  char                   *filename        [R] pool_memcpy if not nul                           
 *  char                   *doc_comment     [E] pool_memcpy if not nul                              
 *  void                   *reserved[]          used to hold LPC flags
 *
 *  [E] denotes a field which is explicitly efree'd in the Zend op_array DTOR
 *  [R] denotes a secondary reference which is not efreed.
 * 
 * To make this easier to code, this scan is driven by a table enerated by parsing the main 
 * zend_vm_def.h file during the development cycle.
 *
TODO: Add processing of literals and interned strings for Zend 2.4
 */ 

    /* start with a bitwise copy of the array */
    memcpy(dst, src, sizeof(*src));

   /*
    * Copy the function_name, arg_info array, recount, vars, brk_cont_array, try_catch_array,
    * filename, doc_comment and literals as per above summary
    */
    POOL_ESTRDUP_FLD(function_name);

    POOL_MEMCPY_FLD(arg_info, src->num_args * sizeof(zend_arg_info));
    for(i=0; i < dst->num_args; i++) { /* dst->num_args = 0 if dst->arg_info is NULL */
        zend_arg_info *src_ai = src->arg_info + i;
        zend_arg_info *dst_ai = dst->arg_info + i;
        POOL_ENSTRDUP(dst_ai->name, src_ai->name);
        POOL_ENSTRDUP(dst_ai->class_name, src_ai->class_name);
    }
 
    POOL_MEMCPY_FLD(refcount, sizeof(zend_uint));

#ifdef ZEND_ENGINE_2_1 /* PHP 5.1 */
    POOL_MEMCPY_FLD(vars, sizeof(src->vars[0]) * src->last_var);
    for(i = 0; i <  src->last_var; i++) { /* dst->last_var = 0 if dst->vars is NULL */
        POOL_ENSTRDUP(dst->vars[i].name, src->vars[i].name);
    }
#endif
    POOL_MEMCPY_FLD(brk_cont_array, sizeof(zend_brk_cont_element) * src->last_brk_cont);
    POOL_MEMCPY_FLD(try_catch_array, sizeof(zend_try_catch_element) * src->last_try_catch);

    /* copy the table of static variables */
    if (src->static_variables) {
        pool_alloc_ht(dst->static_variables);
        LPCGP(copied_zvals) = LPC_COPIED_ZVALS_COUNTDOWN;
        COPY_HT_P(static_variables, lpc_copy_zval_ptr, 
                  zend_property_info *, NULL, NULL);
    }

    dst->filename = is_copy_in() ? LPCG(current_filename) : NULL;

    POOL_ENSTRDUP_FLD(doc_comment);

#ifdef ZEND_ENGINE_2_4                    /* Pooled literals introduced in Zend 2.4 */
    if (src->literals) {
        zend_literal *p, *q, *end;

        POOL_MEMCPY_FLD(literals, sizeof(zend_literal) * src->last_literal);
        end = dst->literals + ;
        for (i = 0; i < dst->last_literal; i++ ) {
            LPCGP(copied_zvals) = LPC_COPIED_ZVALS_COUNTDOWN;
            copy_zval_out(dst->literals[i].constant, src->literals[i].constant, pool);
            pool_tag_ptr(dst->literals[i].constant);
        }
    }
#endif
   /*
    * Now do the remaining deep copy, relocations and fixups of the opcode array.  The copy in and
    * out versions are different for the reasons explained above. The copy-out is out-of-lined
    * to simply code structure.
    */
    if (is_copy_out()) {

        copy_opcodes_out(dst, src, pool);

    } else { /* copy-in */
        
        zend_op *src_zo, *dst_zo;
        zend_op *src_zo_last = src->opcodes + src->last;
        /* See copy_opcodes_out below for details on the LPC flag bits */ 
        lpc_opflags_t flags = (lpc_opflags_t) ((size_t) src->reserved[lpc_reserved_offset]);
 
#define FETCH_AUTOGLOBAL_IF_FLAGGED(member) \
    if(flags&LPC_FLAG_##member) \
        (void) zend_is_auto_global(#member, sizeof(#member) - 1 TSRMLS_CC)

        if (flags&LPC_FLAG_JIT_GLOBAL){
            FETCH_AUTOGLOBAL_IF_FLAGGED(_GET);
            FETCH_AUTOGLOBAL_IF_FLAGGED(_POST);
            FETCH_AUTOGLOBAL_IF_FLAGGED(_COOKIE);
            FETCH_AUTOGLOBAL_IF_FLAGGED(_SERVER);
            FETCH_AUTOGLOBAL_IF_FLAGGED(_ENV);
            FETCH_AUTOGLOBAL_IF_FLAGGED(_FILES);
            FETCH_AUTOGLOBAL_IF_FLAGGED(_REQUEST);
            FETCH_AUTOGLOBAL_IF_FLAGGED(_SESSION);
#ifdef ZEND_ENGINE_2_4
            FETCH_AUTOGLOBAL_IF_FLAGGED(GLOBALS);
#endif
        }

        /* do the base bitwise copy of the oplines */
        POOL_MEMCPY_FLD(opcodes, sizeof(zend_op) * src->last);

        for (src_zo = src->opcodes, dst_zo = dst->opcodes; 
             src_zo < src_zo_last; 
             src_zo++, dst_zo++) {

        if( src_zo->handler == NULL) {
            dst_zo->handler = zend_vm_get_opcode_handler(src_zo);
        }

            zend_uchar opcode_flags = opcode_table[dst_zo->opcode];
           /*
            * The only type of pointer-based elements are in the zvals stored in the constant field
            * of constant znodes in the op_array for Zend Engine < 2.4 are constants. These need to
            * be deep-copied.  For Zend 2.4 these have been replaced by literals and immutable
            * strings
            */  
#ifdef ZEND_ENGINE_2_4
////// TODO: APC treat this as a bitwise copy but need to check this.
#else
            COPY_CONST_ZVAL_IN_IF_CONST(result);
            COPY_CONST_ZVAL_IN_IF_CONST(op1);
            COPY_CONST_ZVAL_IN_IF_CONST(op2);
#endif
     
            if (opcode_flags & LPC_OP_JMP_OP1) {
                JUMP_ADDR(dst_zo->op1) = dst->opcodes + (JUMP_ADDR(src_zo->op1) - src->opcodes);
            } else if (opcode_flags & LPC_OP_JMP_OP2) {
                JUMP_ADDR(dst_zo->op2) = dst->opcodes + (JUMP_ADDR(src_zo->op2) - src->opcodes);
            } else if (opcode_flags & LPC_OP_FETCH_IN &&
                       flags & LPC_FLAG_UNKNOWN_GLOBAL &&
                       PG(auto_globals_jit) &&
                       ZOP_TYPE_IS_CONSTANT_STRING(src_zo->op1) &&
                       ZO_EA_IS_FETCH_GLOBAL(src_zo)) {
                zval *const_pzv = CONST_PZV(src_zo->op1);
//// TODO: raise bug with APC.  Unknown autoglobals don't need to begin with _. Only a convention
                (void) zend_is_auto_global(Z_STRVAL_P(const_pzv), Z_STRLEN_P(const_pzv) TSRMLS_CC);
            }  
        }
    }
}
/* }}} */

/* {{{ lpc_copy_zval_ptr */
void lpc_copy_zval_ptr(zval** pdst, const zval** psrc, lpc_pool* pool)
{ENTER(lpc_copy_zval_ptr)
    pool_alloc_zval(*pdst);
    if (is_copy_in()) {
        copy_zval_in(*pdst, *psrc, pool);
    } else {
        copy_zval_out(*pdst, *psrc, pool);
    }
}
/* }}} */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
