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
  | Authors: Terry Ellison <Terry@ellisons.org.uk                        |
  +----------------------------------------------------------------------+

   This software includes content derived from the APC extension which was
   initially contributed to PHP by Community Connect Inc. in 2002 and revised 
   in 2005 by Yahoo! Inc. See README for further details.

   All other licensing and usage conditions are those of the PHP Group.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef LPC_H
#define LPC_H
#define PERSISTENT 1

#ifdef __DEBUG_LPC__
# define LPC_DEBUG 1
#endif

#ifdef LPC_DEBUG
# define ENTER(s) int dummy_to_be_ignored = lpc_debug_enter(#s);
extern int lpc_debug_enter(char *s);
#else 
# define ENTER(s) 
#endif
#define NOENTER(s) 

/*
 * This module defines utilities and helper functions used elsewhere in LPC.
 */

/* Commonly needed C library headers. */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* UNIX headers (needed for struct stat) */
#include <sys/types.h>
#include <sys/stat.h>
#ifndef PHP_WIN32
# include <unistd.h>
#endif

#include "php_lpc.h"
#include "lpc_pool.h"

#include "php.h"
#include "php_streams.h"
#include "zend_llist.h"

/* console display functions */
extern void lpc_error(const char *format TSRMLS_DC, ...);
extern void lpc_warning(const char *format TSRMLS_DC, ...);
extern void lpc_notice(const char *format TSRMLS_DC, ...);
extern void lpc_debug(const char *format TSRMLS_DC, ...);

/* string and text manipulation */
extern char** lpc_tokenize(const char* s, char delim TSRMLS_DC);

/* filesystem functions */

typedef struct lpc_fileinfo_t {
    char *fullpath;
    char path_buf[MAXPATHLEN];
    php_stream_statbuf st_buf;
} lpc_fileinfo_t;

typedef struct _lpc_request_context_t lpc_request_context_t;
typedef struct _lpc_cache_t lpc_cache_t; /* opaque cache type */

extern int lpc_valid_file_match(char *filename TSRMLS_DC);
extern long lpc_atol(const char *str, int str_len);
extern char *lpc_resolve_path(zval *pzv TSRMLS_DC);
extern void *lpc_resolve_symbol(const char *symbol TSRMLS_DC);

#if defined(__GNUC__)
# define LPC_UNUSED __attribute__((unused))
# define LPC_USED __attribute__((used))
# define LPC_ALLOC __attribute__((malloc))
# define LPC_HOTSPOT __attribute__((hot))
#else 
# define LPC_UNUSED
# define LPC_USED
# define LPC_ALLOC 
# define LPC_HOTSPOT 
#endif

/* Only implement regexp filters for PHP versions >= 5.2.2 otherwise it acts as a Noop */
#if HAVE_PCRE || (HAVE_BUNDLED_PCRE && (PHP_MAJOR_VERSION > 5) || (PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION > 2) ||   (PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION == 2 && PHP_RELEASE_VERSION >= 2 ))
#  define  PHP_REXEP_OK
#endif
ZEND_BEGIN_MODULE_GLOBALS(lpc)
    /* configuration parameters */
    zend_bool   enabled;                /* if true, lpc is enabled (defaults to true) */
 
    /* module variables */
    zend_bool   initialized;            /* true if module was initialized */
    zend_bool   cache_by_default;       /* true if files should be cached unless filtered out */
                                        /* false if files should only be cached if filtered in */
    long        file_update_protection; /* Age in seconds before a file is eligible to be cached -
                                           0 to disable */
    zend_bool   enable_cli;             /* Flag to override turning LPC off for CLI */
    long        max_file_size;          /* Maximum size of file, in bytes that LPC will be allowed 
                                           to cache */
    zend_bool   fpstat;                 /* true if fullpath includes should be stat'ed */
    zend_bool   canonicalize;           /* true if relative paths should be canonicalized in no-stat
                                           mode */
    zend_bool   report_autofilter;      /* true for auto-filter warnings */
    zend_uint   copied_zvals;           /* copy zvals recursion detection counter */
    zend_bool   force_file_update;      /* force files to be updated during lpc_compile_file */
    time_t      sapi_request_time;      /* the SAPI request start time is used for any timestamp
                                           validation */
#ifdef ZEND_ENGINE_2_4
    long        shm_strings_buffer;
#endif
    lpc_cache_t *lpc_cache;             /* the global compiler cache */
    lpc_pool    serial_pool;            /* Serial pool. Note that only one can be open at a time */
    zend_uchar *pool_buffer;            /* Shared serial pool buffer */
    zend_uint   pool_buffer_size;       /* Shared serial pool buffer size */
    zend_uint   pool_buffer_rec_size;   /* Shared serial pool buffer record size */
    zend_uint   pool_buffer_comp_size;  /* Shared serial pool buffer compressed record size */
    zend_llist  exec_pools;             /* Linked list of created exec pools */
    zend_bool   force_cache_delete;     /* Flag that the file D/B is to be deleted and further
                                           loading disabbled */
    char       *clear_cookie;           /* Name of Cookie which will force a cache clear */
    char       *clear_parameter;        /* Name of Request parameter which will force a cache clear */
    char       *current_filename;       /* pointer to filename of file being currently compiled */
    lpc_request_context_t *request_context;  /* pointer to SAPI derived context of the script being
                                                requested */
    zend_bool   resolve_paths;          /* all constant relative paths should be resolved to abdolute */
    zend_uint   debug_flags;            /* flags to allow run-time selective dump output */
    zend_uint   storage_quantum;        /* quantum for pool buffer allocation */
    zend_bool   reuse_serial_buffer;    /* if true then the serial buffer persists over the request */
    zend_uint   compression_algo;       /* 0 = none; 1 = RLE; 2 = GZ */
    JMP_BUF    *bailout;                /* used to sentence known throws from bailouts */
    HashTable   intern_hash;            /* used to create interned strings */
    zend_uchar **interns;               /* used on copy-in and out, array of LPC interns[]  */
    uint        intern_cnt;             /* used on copy-in and out, count of LPC interns[]  */
    php_stream *opcode_logger;          /* used for debug logging */

ZEND_END_MODULE_GLOBALS(lpc)

/* (the following declaration is defined in php_lpc.c) */
ZEND_EXTERN_MODULE_GLOBALS(lpc)

extern int lpc_reserved_offset;
/*
 * Because so many LPC calls are pool-related and pools are thread-specfic in the multi-thread 
 * builds, the tsrm_ls and global vector pointers are copied into the pool header in these builds 
 * thus maintaining thread reentrancy but avoiding the need to pass the TSRML_D(C) pointers through
 * down many call chains and reducing the runtime cost of referencing the Global Vector. 
 */
#ifdef ZTS
# define TSRMLS_P pool->tsrm_ls
# define TSRMLS_PC , TSRMLS_P
# define LPCG(v) TSRMG(lpc_globals_id, zend_lpc_globals *, v)
# define LPCGP(v) (pool->gv->v)
# define TSRMLS_FETCH_FROM_POOL() void ***tsrm_ls = TSRMLS_P;
# define TSRMLS_FETCH_GLOBAL_VEC() zend_lpc_globals *gv = \
    ((zend_lpc_globals *)(*((void ***)tsrm_ls))[TSRM_UNSHUFFLE_RSRC_ID(lpc_globals_id)]);
#else
# define TSRMLS_P
# define TSRMLS_PC
# define LPCG(v) (lpc_globals.v)
# define LPCGP(v) (lpc_globals.v)
# define TSRMLS_FETCH_FROM_POOL()
# define TSRMLS_FETCH_GLOBAL_VEC() zend_lpc_globals *gv = &lpc_globals;
#endif

#define LPC_COPIED_ZVALS_COUNTDOWN  1000        

# define LPC_DBG_ALLOC  (1<<0)  /* Storage Allocation */
# define LPC_DBG_RELO   (1<<1)  /* Relocation outside pool */
# define LPC_DBG_RELC   (1<<2)  /* Relocation Address check */
# define LPC_DBG_RELR   (1<<3)  /* Missed relocation report */
# define LPC_DBG_LOAD   (1<<4)  /* Load/Unload Info */
# define LPC_DBG_ENTER  (1<<5)  /* Print out function enty audit */
# define LPC_DBG_COUNTS (1<<6)  /* Print out function summary counts */
# define LPC_DBG_FILES  (1<<7)  /* Print out any file requests */
# define LPC_DBG_INTN   (1<<8)  /* Duplicate intern allocation */
# define LPC_DBG_ZVAL   (1<<9)  /* ZVAL tracking */
# define LPC_DBG_LOG_OPCODES (1<<10)  /* Opcode loggin */

#define LPC_POOL_OVERFLOW 1

#if SIZEOF_SHORT != 2
# error "LPC only supports 2 byte shorts"
#endif

#if SIZEOF_SIZE_T == 8

# define LPC_SERIAL_INTERNED_TEST ((unsigned short)0xbb00)
# define LPC_SERIAL_INTERNED_VALUE ((size_t)0xbb00000000000000) 
# define LPC_SERIAL_INTERNED_MASK ((size_t)0x000000000fffffff)
# define LPC_IS_SERIAL_INTERNED(p) ((unsigned short)((size_t)(p)>>48) == LPC_SERIAL_INTERNED_TEST)

#elif SIZEOF_SIZE_T == 4

# define LPC_SERIAL_INTERNED_TEST ((unsigned short)0xbb00)
# define LPC_SERIAL_INTERNED_VALUE ((size_t)0xbb0000000) 
# define LPC_SERIAL_INTERNED_MASK  ((size_t)0x0000fffff)
# define LPC_IS_SERIAL_INTERNED(p) ((unsigned short)((size_t)(p)>>16) == LPC_SERIAL_INTERNED_TEST)

#else
# error "LPC only supports 4 and 8 byte addressing"
#endif

# define LPC_SERIAL_INTERN(id) ((void *)(LPC_SERIAL_INTERNED_VALUE + id))
# define LPC_SERIAL_INTERNED_ID(ip) ((uint) ((size_t)(ip)&LPC_SERIAL_INTERNED_MASK))

# define LPC_ALLOCATE_TAG ((void *)1)

#if defined(ZEND_ENGINE_2_4)
#  define LPC_MAX_OPCODE     156       /* 3 new opcodes in 5.4 - separate, bind_trais, add_trait */
#elif defined(ZEND_ENGINE_2_3)
#  define LPC_MAX_OPCODE     153       /* 3 new opcodes in 5.3 - unused, lambda, jmp_set */
# else
#  define LPC_MAX_OPCODE     150
# endif

/*
 * The LPC bailout macros are effectively cloned from the Zend equivalents.  The Zend bailout 
 * macros resets the execution status and execution context, but in the LPC case for pool overflow, 
 * the code makes a soft recovery that enables execution to continue. 
 */ 
#define lpc_try \
    { \
        JMP_BUF *__orig_bailout = LPCG(bailout); \
        JMP_BUF  __bailout; \
        int      __jump_status; \
        LPCG(bailout) = &__bailout; \
        __jump_status = SETJMP(__bailout); \
        if (__jump_status==0) { 
#define lpc_catch \
	    } else if (__jump_status==1) { \
            LPCG(bailout) = __orig_bailout;
#define lpc_end_try() \
	    } else { \
            LPCG(bailout) = __orig_bailout; \
            zend_throw(); \
        } \
	    LPCG(bailout) = __orig_bailout; \
    }
#define lpc_throw_storage_overflow()	LONGJMP(*LPCG(bailout), 1)


/* {{{ lpc_vm_get_opcode_handler
       This is a copy of Zend/zend_vm_execute.c:zend_vm_get_opcode_handler() */
#define _LPC_CONST_CODE  0
#define _LPC_TMP_CODE    1
#define _LPC_VAR_CODE    2
#define _LPC_UNUSED_CODE 3
#define _LPC_CV_CODE     4
extern const int lpc_vm_decode[];
static zend_always_inline opcode_handler_t lpc_vm_get_opcode_handler(zend_op* op)
{
		return zend_opcode_handlers[op->opcode * 25 + lpc_vm_decode[op->op1.op_type] * 5 + lpc_vm_decode[op->op2.op_type]];
}
/* }}} */


#endif /* LPC_H */
/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
