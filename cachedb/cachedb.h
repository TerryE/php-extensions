#ifndef CACHEDB_H
#define CACHEDB_H

#include <sys/stat.h>
#include "php.h"

/* {{{ Private types */ 
typedef struct _cachedb_t cachedb_t, *cachedb_pt;
/* }}} */

/* {{{ Public interface to Cache DB */
PHPAPI int _cachedb_open( cachedb_t** pdb, char *file,   size_t file_len, char *mode TSRMLS_DC);
PHPAPI int _cachedb_close(cachedb_t*  db, char mode TSRMLS_DC);
PHPAPI int _cachedb_find( cachedb_t*  db,  char  *key,   size_t key_len, zval *metadata TSRMLS_DC);
PHPAPI int _cachedb_fetch(cachedb_t*  db,  zval *value TSRMLS_DC);
PHPAPI int _cachedb_add(  cachedb_t*  db,  char  *key,   size_t key_len, zval *value, zval *metadata TSRMLS_DC);
PHPAPI int _cachedb_info( zval **info, cachedb_t* db TSRMLS_DC);
PHPAPI const struct stat *cachedb_get_sb(cachedb_t* db TSRMLS_DC);
/* }}} */

/* {{{ Public macros to make the calling code more readable */
#define cachedb_open(p,f,fl,m)    _cachedb_open(p,f,fl,m TSRMLS_CC)
#define cachedb_close(db)         _cachedb_close(db, '*' TSRMLS_CC)
#define cachedb_close2(db,m)      _cachedb_close(db, m TSRMLS_CC)
#define cachedb_find(db,k,kl,m)   _cachedb_find(db,k,kl, m TSRMLS_CC)
#define cachedb_fetch(db,v)       _cachedb_fetch(db,v TSRMLS_CC)
#define cachedb_add(db,k,kl,v,m)  _cachedb_add(db,k,kl,v,m TSRMLS_CC)
#define cachedb_info(rv,db)       _cachedb_info(&rv,db TSRMLS_CC)
/* }}} */

#endif /* CACHEDB_H */
