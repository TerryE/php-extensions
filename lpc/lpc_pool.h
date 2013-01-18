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
  | Authors: Gopal Vijayaraghavan <gopalv@yahoo-inc.com>                 |
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Yahoo! Inc. in 2008.

   Future revisions and derivatives of this source code must acknowledge
   Yahoo! Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.

*/

#ifndef LPC_POOL_H
#define LPC_POOL_H

#include "zend.h"
/*
 * A pool is a group of dynamically allocated memory objects with a common set of properties:
 *
 *   *  They share the same memory alloc and free methods
 *   *  They are individually be created (and destroyed) by the same pool methods
 *   *  The pool DTOR will also deallocate any remaining elements in the pool.
 *
 * In the APC/LPC implementation, all element destruction is carried out (or should be) by the pool
 * DTOR, and so the element DTOR is private to the pool.
 *
 * In APC the term "REALPOOL" and "UNPOOL" were used for pools allocated in shared memory and PHP 
 * emalloc storage (as the latter was freed by the Execution engine.  A variant of this latter with  
 * a serial "BD" allocator was use for the binary load/dump routines.  The LPC implementation uses
 * two pool types, exec and serial, both of which are maintained in thread-local memory.  The serial
 * pools are declared as serial or readonly serial.
 *
 * The public interface to the pool is encapsulated in the following macros.  Note that all 
 * pool elements must be created through one of the constructors, and that no public element
 * destructor exists.  To keep these simple, all Also since the ,  to simplify
 * argument overheads.
 */

typedef struct _zend_lpc_globals zend_lpc_globals;

/* {{{ Public pool types */
typedef enum {
    LPC_EXECPOOL      = 0x0,  /* The Zend execution environment handles memory recovery */
    LPC_SERIALPOOL    = 0x1,  /* A pool in which all storage is in contiguous blocks */
    LPC_RO_SERIALPOOL = 0x2   /* A pool in which all storage is in contiguous blocks */
} lpc_pool_type_t;

typedef struct _lpc_pool {
#ifdef ZTS
    void         ***tsrm_ls;         /* the thread context in ZTS builds */
    zend_lpc_globals *gv;            /* fast pointer to the dynamic global vector in ZTS builds */
#endif
    lpc_pool_type_t type;           
    zend_uint       size;            /* sum of individual element sizes */
    zend_uint       count;           /* count of pool elements*/
    /* The following fields are only used for serial pools */
    void           *storage;         /* pointer to storage vector */
    zend_uint       available;       /* bytes available in current brick -- ditto */
    zend_uint       allocated;       /* bytes available in current brick -- ditto */
    HashTable       tags;            /* tag hash */
    /* The following fields are only used for exec pools */
    zend_uchar     *intern_copy;
} lpc_pool;
/* }}} */

/* {{{ Pool allocator macros 
 *
 * These macros have a different layout from the emalloc type in that the destination pointer is an
 * out parameter rather than a return. This is because -- in the case of serial variants -- any
 * intra-pool pointers must be correctly tagged so that they can be relocated on reload as the base
 * address of the reloaded pool will be different to that of the saved pool. There are also
 * convenience forms for allocation of zvals, HashTables and string storage to dovetail into the
 * Zend fast allocators for these. Also to keep usage simple, the macros assume that the variable
 * pool exists and is in scope and points to the current pool.
 *
 * As the pool is instantiated specific to a given thread, the TSRMLS pointer is moved into the pool
 * allowing it to be dropped from the argument list.
 */ 
#define pool_alloc(dest, size)  _lpc_pool_alloc((void **)&(dest), pool, size ZEND_FILE_LINE_CC)
#define pool_alloc_zval(dest) _lpc_pool_alloc_zval((void **)&(dest), pool ZEND_FILE_LINE_CC)
#define pool_alloc_ht(dest) _lpc_pool_alloc_ht((void **)&(dest), pool ZEND_FILE_LINE_CC)
#define pool_alloc_unaligned(dest,size) \
    _lpc_pool_alloc_unaligned((void **)&(dest), size, pool ZEND_FILE_LINE_CC)
#define pool_strdup(dst,src,type) \
    _lpc_pool_strdup((const char **)&(dst), (src), type, pool ZEND_FILE_LINE_CC)
#define pool_nstrdup(dst,dstn,src,srcn,type) \
    _lpc_pool_nstrdup((const char **)&(dst),(uint *)&(dstn), (src), (srcn), type, pool ZEND_FILE_LINE_CC)
#define pool_strcmp(dst,src)  _lpc_pool_strcmp((dst), (src), pool ZEND_FILE_LINE_CC)
#define pool_strncmp(dst,src,n)  _lpc_pool_strncmp((dst),(src),(n), pool ZEND_FILE_LINE_CC)
#define pool_memcpy(dst,src,n) _lpc_pool_memcpy((void **)&(dst),src,n,pool ZEND_FILE_LINE_CC)

#define pool_tag_ptr(p) _lpc_pool_tag_ptr((void **)&(p), pool ZEND_FILE_LINE_CC)
#define is_exec_pool() (pool->type == LPC_EXECPOOL)
#define is_copy_out()  (pool->type != LPC_EXECPOOL)
#define is_copy_in()   (pool->type == LPC_EXECPOOL)
/* }}} */

/* {{{ Public pool functions
 * All the pool allocator functions pass the pool as an arg and so don't need a TSRMLS_DC argument.
 * They also use source line forwarding so that any debug messages and Zend memory leek reports 
 * refer back to the originating upper level call. Their use is macro-wrapped to hide this hassle
 * at a source level. 
 *
 * Note that pool_memcpy and pool_nstrdup are functionally different.  pool_nstrdup returns an
 * interned reference on copy-out, whereas memcpy does a bitwise copy and the destination reference
 * is always size_t aligned.  Also note that pool_nstrdup is deliberately NOT pool_strndup because 
 * it has an important functional difference from strndup() in that n bytes are always copied 
 * because the Zend engine supports zero-embedded strings, and uses them in mangled function and
 * class names.
 */
extern void _lpc_pool_alloc(void **dest, lpc_pool* pool, uint size ZEND_FILE_LINE_DC);
extern void _lpc_pool_alloc_zval(void **dest, lpc_pool* pool ZEND_FILE_LINE_DC);
extern void _lpc_pool_alloc_ht(void **dest, lpc_pool* pool ZEND_FILE_LINE_DC);
extern void _lpc_pool_memcpy(void **dest, const void* p, uint n, lpc_pool* pool ZEND_FILE_LINE_DC);
extern void _lpc_pool_strdup(const char **d, const char* s, 
                             zend_bool type, lpc_pool* pool ZEND_FILE_LINE_DC);
extern void _lpc_pool_nstrdup(const char **d, uint *dn, const char* s, uint sn, 
                              zend_bool type, lpc_pool* pool ZEND_FILE_LINE_DC);
extern int  _lpc_pool_strcmp(const char* s1, const char* s2, lpc_pool* pool ZEND_FILE_LINE_DC);
extern int  _lpc_pool_strncmp(const char* s1, const char* s2, uint sn, 
                              lpc_pool* pool ZEND_FILE_LINE_DC);
extern void _lpc_pool_tag_ptr(void **ptr, lpc_pool* pool ZEND_FILE_LINE_DC);
/* }}} */

/* {{{ Pool module init/shutdown */
extern int lpc_pool_init(uint max_module_len TSRMLS_DC);
extern void lpc_pool_shutdown(TSRMLS_D);
/* }}} */

/* {{{ Pool create / destroy functions
 * Serial pools now support the use of a single persistent pool storage area across all serial pools 
 * created within a single request.  This reduces storage fragmentation and the dynamic allocation 
 * overheads at runtime. However, this complicates the API for create / destroy functions, so this 
 * is now restructured to make a clearer definition of the scope of the pool functions and the
 * invoking upper-level code. The life cycles for a serial pool follows one of two paths depending
 * on whether a copy-out or copy-in is in progress:
 *
 *    ==Copy-out to serial pool==                  ==Copy-in to R/O serial pool==   
 *    lpc_pool_storage   IN  maximum_length        lpc_pool_storage   IN  compressed_length
 *                       IN  compressed_length                        IN  uncompressed_length
 *                       NULL                                         OUT compressed_buffer
 *    N/A                                          <readin compressed buffer> 
 *    lpc_pool_create    IN  type                  lpc_pool_create    IN  type  
 *                       -   [not used]                               OUT *first_rec
 *                       RTN *pool                                    RTN *pool          
 *    <copy out source using alloc rtns>           <access source> 
 *    lpc_pool_serialize IN  *pool                 N/A
 *                       OUT compressed_length
 *                       OUT uncompressed_length
 *                       RTN *compressed_buf
 *    <write to cache file>                        N/A
 *    lpc_pool_destroy   IN  *pool                 lpc_pool_destroy   IN  *pool
 *
 * Exec pools are only created as the destination for copy-in and here only the create and destroy
 * functions are used; the lcp_pool_storage() function is only used for serial pools.
 *
 * Also note that the copy-out allocators can bailout with an raise a LPC_POOL_OVERFLOW status, and
 * the upper logic must recover and retry the lpc_pool_create() ... lpc_pool_destroy() sequence (as
 * the create will allocate extra pool memory on retry). 
 */
extern void        lpc_pool_storage(zend_uint, zend_uint, zend_uchar** TSRMLS_DC);
/*  Booking RO serial storage       rec_size   comp_size  &comp_buffer                      */
/*  Booking    serial storage       0          0          NULL                              */
/*  Extending  serial storage       incr       0          NULL (after Overflow retry)       */
/*  Conditionally freeing       (unsigned) -1  0          NULL                              */

extern lpc_pool*   lpc_pool_create(lpc_pool_type_t type, void** first_rec TSRMLS_DC);
extern zend_uchar* lpc_pool_serialize(lpc_pool* pool, zend_uint* compressed_size, 
                                      zend_uint* record_size);
extern void lpc_pool_destroy(lpc_pool* pool);

/* }}} */
#endif /* LPC_POOL_H */
