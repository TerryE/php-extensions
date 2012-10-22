#ifndef CACHEDB_H
#define CACHEDB_H

#include "php.h"

typedef struct _cachedb_rec_t {
	char       *key;
    size_t      key_length;
	int         is_base;
    off_t       start;
	size_t      zlen;
	size_t      len;
} cachedb_rec_t;

typedef struct _cachedb_file_t {
	char               *name;
    size_t              name_length;
	char               *dir;	
    size_t              dir_length;
	off_t               next_pos;
	size_t              header_length;
	php_stream         *fp;
	php_stream_statbuf  sb;
} cachedb_file_t;

typedef struct _cachedb_t {
	cachedb_file_t base_file;
	cachedb_file_t tmp_file;
	HashTable     *index_list;
	HashTable     *index_hash;
	cachedb_rec_t  last_find;
	int			   is_binary;
	char           mode;
} cachedb_t, *cachedb_pt;

#define CACHEDB_HEADER_FINGERPRINT "cachedb-"
typedef struct _cachedb_header_t {
	char       fingerprint[8];
	size_t     zlen;
	size_t     len;
} cachedb_header_t;

PHPAPI int _cachedb_open( cachedb_t** pdb, char *file,   size_t file_len, char *mode TSRMLS_DC);
PHPAPI int _cachedb_close(cachedb_t*  db, char mode TSRMLS_DC);
PHPAPI int _cachedb_find( cachedb_t*  db,  char  *key,   size_t key_len, zval *metadata TSRMLS_DC);
PHPAPI int _cachedb_fetch(cachedb_t*  db,  zval *value TSRMLS_DC);
PHPAPI int _cachedb_add(  cachedb_t*  db,  char  *key,   size_t key_len, zval *value, zval *metadata TSRMLS_DC);
PHPAPI int _cachedb_info( zval **info, cachedb_t* db TSRMLS_DC);

#define cachedb_open(p,f,fl,m)    _cachedb_open(p,f,fl,m TSRMLS_CC)
#define cachedb_close(db)         _cachedb_close(db, '*' TSRMLS_CC)
#define cachedb_close2(db,m)      _cachedb_close(db, m TSRMLS_CC)
#define cachedb_find(db,k,kl,m)   _cachedb_find(db,k,kl, m TSRMLS_CC)
#define cachedb_fetch(db,v)       _cachedb_fetch(db,v TSRMLS_CC)
#define cachedb_add(db,k,kl,v,m)  _cachedb_add(db,k,kl,v,m TSRMLS_CC)
#define cachedb_info(rv,db)       _cachedb_info(&rv,db TSRMLS_CC)

#endif /* CACHEDB_H */
