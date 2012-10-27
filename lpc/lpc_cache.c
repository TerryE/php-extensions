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

/* $Id: lpc_cache.c 307332 2011-01-10 09:01:23Z pajoye $ */

#include "lpc.h"
#include "lpc_cache.h"
#include "ext/cachedb/cachedb.h"
#include "lpc_zend.h"
#include "SAPI.h"
#include "TSRM.h"
#include "fopen_wrappers.h"
#include <zlib.h>


/* {{{ struct definition: lpc_cache_t */
struct _lpc_cache_t {
	cachedb_t     *db;
	HashTable     *index;
	time_t         mtime;
	off_t          filesize;
	dev_t          dev_id;
	ino_t          inode;
};
/* }}} */


#define CHECK(p) if(!(p)) goto error;

/* {{{ lpc_cache_create */
zend_bool lpc_cache_create(TSRMLS_D)
{ENTER(lpc_cache_create)
    lpc_cache_t  *cache        = (lpc_cache_t*) ecalloc(1, sizeof(lpc_cache_t));
	struct stat  *sb           = sapi_get_stat(TSRMLS_C);
	char         *request_path = SG(request_info).path_translated;
	char         *request_dir  = estrdup(request_path);
	size_t        dir_length   = zend_dirname(request_dir, strlen(request_path));
	char         *basename;
	size_t        basename_length;
	char         *cachepath;
	zval         *zinfo, **zlist, **zhash; 
	zval         **le, **le_len, **le_metadata;
	zval         **he,  **he_0;
	HashTable    *hhash, *hlist;
	char         *dummy;

	/* Determine the cache name from the dirname and basename of the Request path */
	php_basename(request_path, strlen(request_path), NULL, 0, &basename, &basename_length TSRMLS_CC);
	cachepath    = emalloc(dir_length + sizeof("./") + basename_length + sizeof(".cache") + sizeof(char));
	sprint(cachepath, "%s%c.%s.cache", request_dir, DEFAULT_SLASH, basename);
 
	/* Open the CacheDB using the cache name */
	CHECK(cachedb_open(&cache->db, cachepath, strlen(cachepath), "wb") == SUCCESS);
	efree(request_dir);
	efree(cachepath);

	/* Obtain the directory info from the CacheDB */
	MAKE_STD_ZVAL(zinfo);
	CHECK(cachedb_info(zinfo,cache->db)==SUCCESS);

	/* Point hlist and hhash to the two arrays in the info return */
	zend_hash_internal_pointer_reset(Z_ARRVAL_P(zinfo));
	zend_hash_get_current_data(Z_ARRVAL_P(zinfo), (void **)&zlist);
	hlist = Z_ARRVAL_PP(zlist);
	zend_hash_move_forward(Z_ARRVAL_P(zinfo));
	zend_hash_get_current_data(Z_ARRVAL_P(zinfo), (void **)&zhash);
	SEPARATE_ZVAL(zhash);
	hhash = Z_ARRVAL_PP(zhash);

	/* Loop over the two arrays together setting up list_entry and hash_entry where 
	 * hlist = array( array(filename, zlen, len, metadata), ... ) and
	 * hhash = array( filename=>array(i,db_offset), ... ) replacing the values of hhash 
	 * by array_merge (array(len), metadata) */

/////////// walk through this in gdb 
	
	for(zend_hash_internal_pointer_reset(hlist), zend_hash_internal_pointer_reset(hlist);
		zend_hash_get_current_data(hlist, (void **)&le) == SUCCESS;
		zend_hash_move_forward(hlist), zend_hash_move_forward(hhash) ) {

		/* Pick up the len and metadata from the list entry */
		zend_hash_internal_pointer_end(Z_ARRVAL_PP(le));
		zend_hash_get_current_data(Z_ARRVAL_PP(le), (void **)&le_metadata);
		zend_hash_move_backward(Z_ARRVAL_PP(le));
		zend_hash_get_current_data(Z_ARRVAL_PP(le), (void **)&le_len);

		/* set he[0]=len, unset(he[1]) and merge in the metadata */
		zend_hash_get_current_data(hhash, (void **)&he);
		zend_hash_internal_pointer_reset(Z_ARRVAL_PP(he));
		zend_hash_get_current_data(Z_ARRVAL_PP(he), (void **)&he_0);
		ZVAL_LONG(*he_0, Z_LVAL_PP(le_len));
		zend_hash_index_del(Z_ARRVAL_PP(he), 1);
		php_array_merge(Z_ARRVAL_PP(he), Z_ARRVAL_PP(le_metadata), 0 TSRMLS_CC);
	}

	/* Now split off the hhash by setting xhash to null and delete zinfo */ 
	ZVAL_NULL(*zhash);
	zval_dtor(zinfo);

	LPCG(lpc_cache) = cache;
	cache->index    = hhash;

	sb              = sapi_get_stat(TSRMLS_C);
	cache->mtime    = sb->st_mtime;
	cache->filesize = sb->st_size;
	cache->dev_id   = sb->st_dev;
	cache->inode    = sb->st_ino;

    return SUCCESS;

error:

	return FAILURE;
}

/* }}} */

/* {{{ lpc_cache_destroy */
void lpc_cache_destroy(TSRMLS_D)
{ENTER(lpc_cache_destroy)
	lpc_cache_t  *cache = LPCG(lpc_cache);
	zend_hash_destroy(cache->index);
	efree(cache->index);
	efree(cache);
}
/* }}} */

/* {{{ lpc_cache_clear */
void lpc_cache_clear(TSRMLS_D)
{ENTER(lpc_cache_clear)
	lpc_cache_t  *cache = LPCG(lpc_cache);
/////////////////////////// TBD
}

/* }}} */

/* {{{ lpc_cache_insert */
int lpc_cache_insert(lpc_cache_key_t *key, 
					 lpc_cache_entry_t* cache_entry, 
				     lpc_context_t *ctxt TSRMLS_DC)
////////////// sort out entry vs value ....
{ENTER(lpc_cache_insert)
    int           pool_length;
	void         *pool_buffer;
	lpc_cache_t  *cache = LPCG(lpc_cache);
	char         *zbuf;
	size_t        zbuf_length;
	zval buffer, metadata, *index_entry;

    if (!key || !cache_entry || !ctxt || ctxt->pool->type != LPC_SERIALPOOL) {
        return 0;
    }

	pool_length = lpc_pool_unload(ctxt->pool, &pool_buffer, cache_entry);

	/* Allocate zbuf len based on worst case for compression, then compress and free original buffer */
	zbuf_length = compressBound(pool_length) + 1;
	zbuf = (char *) emalloc(zbuf_length);
	CHECK(compress(zbuf, &zbuf_length, pool_buffer, pool_length) == Z_OK);
	efree(pool_buffer);
	INIT_ZVAL(buffer); ZVAL_STRINGL(&buffer, zbuf, zbuf_length, 0);

	/* Build the metadata zval for the pool buffer and add the pool buffer to the CacheDB */
	INIT_ZVAL(metadata);
	array_init_size(&metadata, 3);
	add_next_index_long(&metadata, key->mtime);
	add_next_index_long(&metadata, key->filesize);
	add_next_index_long(&metadata, pool_length);

	CHECK(cachedb_add(cache->db, (char *) key->filename, key->filename_length, &buffer, &metadata)==SUCCESS);
	zval_dtor(&buffer);

	/* Update the cache index with the new entry */
	MAKE_STD_ZVAL(index_entry);
	array_init_size(index_entry, 4);
	add_next_index_long(index_entry, zbuf_length);
	php_array_merge(Z_ARRVAL_P(index_entry), Z_ARRVAL(metadata), 0 TSRMLS_CC);
	zval_dtor(&metadata);

//////// Check indirection level in gdb
	zend_hash_add(cache->index, key->filename, key->filename_length, &index_entry, sizeof(zval *), NULL);

    return 1;

error:

	return 0;
}
/* }}} */

/* {{{ lpc_cache_find */
lpc_cache_entry_t* lpc_cache_find(lpc_cache_key_t *key TSRMLS_DC)
{ENTER(lpc_cache_find)
 	lpc_cache_t *cache = LPCG(lpc_cache);

    const char   *filename;
    int           filename_length;
    time_t        mtime;                 /* the mtime of this cached entry */
	size_t        filesize;
    unsigned char type;
	zval        **index_entry, **length, *db_rec;
	char         *buf;
	size_t        buf_length;
	lpc_cache_entry_t *entry;

	if (zend_hash_find(cache->index, key->filename, key->filename_length+1,
	                   (void **) &index_entry) == FAILURE) {
		return NULL;
	}
	CHECK(zend_hash_index_find(Z_ARRVAL_PP(index_entry), 3, (void **) &length) == SUCCESS);

	CHECK(cachedb_find(cache->db, (char *) key->filename, key->filename_length, 0) == SUCCESS);
	buf        = emalloc(Z_LVAL_PP(length)+1);
	buf_length = Z_LVAL_PP(length);
	buf[Z_LVAL_PP(length)] = '\0';      /* zero terminate buf to simply debugging */

	MAKE_STD_ZVAL(db_rec);
	CHECK(cachedb_fetch(cache->db, db_rec) == SUCCESS);
	CHECK(uncompress(buf, &buf_length, Z_STRVAL_P(db_rec), Z_STRLEN_P(db_rec)) == Z_OK &&
		  buf_length==Z_LVAL_PP(length));
	zval_dtor(db_rec); efree(db_rec);
	entry = lpc_pool_load(buf, buf_length);

    return entry;

error:
/////// need a php_error_docref() here;
/////// clean up ?

	return NULL;
}
/* }}} */

/* {{{ lpc_cache_make_file_key */
lpc_cache_key_t* lpc_cache_make_file_key(char *filename TSRMLS_DC)
{ENTER(lpc_cache_make_file_key)
	const char            *include_path = (const char*) PG(include_path);
    lpc_cache_key_t       *key          = ecalloc(1, sizeof(lpc_cache_key_t));
	php_stream            *fp           = NULL;
	struct stat           *sb;
	char                  *opened       = NULL;
    int len;

    CHECK(filename && SG(request_info).path_translated);
 
    len = strlen(filename);

	/* Unlike APC, LPC uses a percentage for fpstat, so if 0 < fpstat < 100, a random number 
     * is generated to decide whether stat validation is bypassed.  The randomness doesn't 
     * need to be strong, so rand() plus the request time is good enough.  
	 *	
	 * Also as the cash is request script-specific and invalidated on change of this location,
	 * relative filenames are permitted as cacheable. */
    if (strcmp(SG(request_info).path_translated, filename)==0) {
		/* this is the request_scriptname being compiled -- always validate the timestamp etc. */
		key->type            = LPC_REQUEST_SCRIPT;
		sb                   = sapi_get_stat(TSRMLS_C);
		key->mtime           = sb->st_mtime;
		key->filesize        = sb->st_size;
		key->dev_id          = sb->st_dev;
		key->inode           = sb->st_ino;
		key->filename_length = strlen(filename);

	} else if (LPCG(fpstat)==0 || (((time_t) rand() + LPCG(sapi_request_time)) % 100) < LPCG(fpstat)) {

		/* Bypass file stat functionality and use the cache entry based on the filename */
        key->filename        = estrdup(filename);
        key->filename_length = strlen(filename);
		key->type            = LPC_NOSTAT;

    } else {

		/* Open the file without then with the include path to work out if it is being used
		 * If the file can't be opened, then the simplest thing to do is to pass back a nil 
         * response so the caller fails back to the standard zend compile module and let it 
         * deal with the error.  */

		fp = php_stream_open_wrapper(filename, "r", IGNORE_URL|STREAM_MUST_SEEK|IGNORE_PATH, &opened);
		key->type		  = (fp) ? LPC_PATH_IGNORED	: LPC_PATH_USED;

		CHECK(fp || (fp = php_stream_open_wrapper(filename, "r", IGNORE_URL|STREAM_MUST_SEEK, &opened)));
		CHECK(php_stream_stat(fp, (php_stream_statbuf *)sb));

		key->type            = LPC_FSTAT;
        key->filename        = estrdup(filename);
        key->filename_length = strlen(filename);
		key->fp              = fp;
		key->mtime           = sb->st_mtime;
		key->filesize        = sb->st_size;
		key->dev_id          = sb->st_dev;
		key->inode           = sb->st_ino;
		efree(sb);

		/* Ditto fail back to the standard zend compile module if the age of the file is less than
         * a fixed (e.g. 2sec) old.  Let the next request cache it :-)  */
		CHECK(LPCG(sapi_request_time) - key->mtime < LPCG(file_update_protection));

		/* Note that some CMS like to munge the mtime, but in this case tough shit: don't use LPC. */
    }
    return key;

error:

/////////////// TODO: Add error notice here    
    if(fp) { php_stream_close(fp); }
    if(key) { efree(key); key = NULL; }
    if(opened) {pefree(opened,1); }
    return NULL;
}
/* }}} */

/* {{{ lpc_cache_make_file_entry */
lpc_cache_entry_t* lpc_cache_make_file_entry(const char* filename,
											 zend_op_array* op_array,
											 lpc_function_t* functions,
											 lpc_class_t* classes,
											 lpc_context_t* ctxt  TSRMLS_DC)
{ENTER(lpc_cache_make_file_entry)
    lpc_cache_entry_t* entry;
    lpc_pool* pool = ctxt->pool;

    entry = (lpc_cache_entry_t*) lpc_pool_alloc(pool, sizeof(lpc_cache_entry_t));

#   define TAG_SETPTR(p,exp) p = exp; lpc_pool_tag_ptr(p, pool);
    TAG_SETPTR(entry->filename, lpc_pstrdup(filename, pool TSRMLS_CC));
    lpc_debug("lpc_cache_make_file_entry: entry->data.file.filename is [%s]" TSRMLS_CC,entry->filename);
    TAG_SETPTR(entry->op_array, op_array);
    TAG_SETPTR(entry->functions, functions);
    TAG_SETPTR(entry->classes, classes);

    entry->halt_offset = lpc_file_halt_offset(filename TSRMLS_CC);

    entry->type = LPC_CACHE_ENTRY_FILE;
    entry->mem_size = 0;
    entry->pool = pool;
    return entry;
}
/* }}} */

/* {{{ lpc_cache_info */
zval* lpc_cache_info(zend_bool limited TSRMLS_DC)
{ENTER(lpc_cache_info)
	lpc_cache_t *cache = LPCG(lpc_cache);
    zval *info = NULL;
    if(!cache) return NULL;
    return info;
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
