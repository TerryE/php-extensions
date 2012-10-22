/*
   +----------------------------------------------------------------------+
   | PHP Version 5                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2010 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Author: Terry Ellison <Terry@ellisonsorg.uk                          |
   +----------------------------------------------------------------------+
 */

/* 
 * See also the documentation in php_cachedb.c.  
 *
 * The main usecase for cachedb is for a file-based cache of idempotent objects which are read  
 * frequently, but very rarely changed or extended, for example language tables in complex
 * application such as MediaWiki.  In this current version, the only method of deletion and  
 * record update is to open the database in create mode, and recreate it in its entirety.
 *
 * Like cdb, a cachedb contains an index containing key->object associations and a set of objects.
 * of known length.  However, unlike cdb:
 *
 *  - The base file is always opened read-only and can be used safely by multiple processes.  It 
 *    contains a fixed-length prefix (fingerprint+index length) followed by the index in logical
 *    record 0.  The remaining logical records store the keyed objects in record creation order: 
 *    this is because most applications will access the objects in the same order, so ordering on
 *    creation order is a good strategy to minimise seeks and serialise access to the DB.
 *
 *  - On opening the DB, the index is loaded into a PHP HashTable, and allowing subsequent records
 *    to be read by a single bulk read (proceded by a seek if not consecutive to the previous 
 *    object read).
 *
 *  - Creation of new objects IS supported (if the D/B is logically opened RW); these are written
 *    to (and can be subsequently read from) a temporary file that is local to the process; this 
 *    is created on demand with the first new object.  
 *
 *  - Any new objects that have been created are committed to the D/B on closure.  This commit is 
 *    transactionally consistent, but not guaranteed to succeed though it should rarely fail.  The
 *    main scenario where is does fail is the loser process in a race between two processes making 
 *    (the same) updates in parallel.  The commit operation creates a new database with the new 
 *    index, the old objects, and the new objects logically concatonated in object creation order.  
 *    This is then moved over the old D/B.  HOWEVER, the date-time stamp of the base file is 
 *    checked before and after the (putative) new database creation, and the commit process is 
 *    aborted if the D/B file has already been replaced by some other asyncronous process.
 *
 *  - Lastly unlike php_cdb which is implemented as a wrapper around a (non-php) clone of 
 *    Bernstein's original cdb C code, cachedb is written only to work within a PHP extension.
 *
 * === NOTES ===
 *
 * Since the scope of any DB is the invoking request, all memory is allocated and managed using the
 * emalloc and other e* routines.  I relaxed approach to clean up is taken on fatal errors, as the
 * e* class dynamic objects are automatically garbage collected as part of request shutdown.  
 * Nonetheless, all non-fatal error paths have proper cleanup.  As a general strategy any putative
 * dynamic object pointer which is not guaranteed to be initialise is assigned to NULL at declaration
 * and the local EFREE macro is used (see below) for safe cleanup.
 * 
 */

/* {{{ declarations and defines */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_streams.h"
#include "php_main.h"

#include "zend_API.h"
#include "zend_alloc.h"
#include "zend_hash.h"
#include "zend_variables.h"

#include "ext/standard/info.h"
#include "ext/standard/file.h"
#include "ext/standard/php_standard.h"
#include "ext/standard/php_string.h"
#include "ext/standard/php_smart_str.h"

#include "cachedb.h"

#include <sys/types.h>
#include <sys/stat.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <string.h>
#include <errno.h>
#include <zlib.h>

static const char _cachedb_ndx_err[]   = "Invalid index in cachedb file %s.";
static const char _cachedb_eom_err[]   = "Invalid record read in cachedb file";
static const char _cachedb_mode_err[]  = "Invalid mode %c during open of cachedb file %s";
static const char _cachedb_add_err[]   = "Internal error during open of cachedb file %s";
static const char _cachedb_close_err[] = "Internal error during close of cachedb file %s";
static const char _cachedb_write_err[] = "Internal error write to cachedb file";

#undef TRUE
#define TRUE 1

#define PEFREE(n,p) if(n) {pefree(n,p); n=NULL; }
#define EFREE(n) if(n) {efree(n); n=NULL;}
#define ZEND_HASH_DESTROY(n) if(n) {zend_hash_destroy(n);}

#define ZVAL_PTR_DTOR_NZ(n) if(n) {zval_ptr_dtor(n);}

#define CHECK(n,e) if (!(n)) { error_type = e; goto error; }
#define CHECKA(n) CHECK((n),'A')
#define CHECKM(n) CHECK((n),'M')

/* Some application-friendly synonyms for some of the hash functions used.  Note that by
   convention PHP includes the terminating zero in string keys, hence the +1 on these lengths */ 
#define hash_find(e,k,v) zend_hash_find(e, k, k##_length+1, (void **) &v)
#define hash_index_find(e,i,v) zend_hash_index_find(e, i, (void **) &v)
#define hash_add_next_index_zval(h,v) zend_hash_next_index_insert(h, &v, sizeof(zval *), NULL)
#define hash_add(h,k,v) zend_hash_add(h,k,k##_length+1, &v, sizeof(zval *), NULL)
#define hash_copy(to,fm) zend_hash_copy(to, fm, (copy_ctor_func_t) zval_add_ref, (void *)&dummy, sizeof(zval*))
#define hash_count(h) zend_hash_num_elements(h)

#define hash_init(h,c) zend_hash_init(h, c, NULL, ZVAL_PTR_DTOR, 0)
#define hash_reset(h) zend_hash_internal_pointer_reset(h)
#define hash_get(h,e) zend_hash_get_current_data(h, (void **) &e)
#define hash_key(h,s,l) zend_hash_get_current_key_ex(h, &s, &s##_length, &l, 0, 0)
#define hash_next(h) zend_hash_move_forward(h)

#define filelength sb.sb.st_size 

/* internal cachedb functions */
static int cachedb_read_var(php_stream *fp, int is_binary, zval *value, size_t zlen, size_t len TSRMLS_DC);
static int cachedb_write_var(php_stream *fp, int is_binary, zval *value, size_t *zlen, size_t *len TSRMLS_DC);
static int cachedb_load_index(cachedb_t* db TSRMLS_DC);
static void cachedb_db_dtor(cachedb_t** pdb TSRMLS_DC);

/* }}} */

/* {{{ proto boolean _cachedb_open(struct* db, string file, int file_length, char mode)
   Open a cachedb database in the given access mode */

/* A logical DB is stored in the filesystem as one or two files and is opened in one of 3 modes:
 *   r: Read.   The DB must exist and records can only be read
 *   w: Write.  The DB may exist and records can be read or written
 *   c: Create/Truncate.  An existing DB may exist, but it is ignored and a new one created
 *
 * The first base file is opened readonly if it exists if the mode is 'r' or 'w'. It can therefore be 
 * safely shared amongst asyncronous threads/processes.  The second temporary file is private to the 
 * thread and is opened rw on first record addition if the mode is 'c' or 'w' and so is not 
 * opened in this function.
 *
 * The record index is also read into a HashTable from the base file on opening.
 */

PHPAPI int _cachedb_open(cachedb_t** pdb, char *file, size_t file_length, char *mode TSRMLS_DC)
{
	cachedb_t      *db     = NULL;
	cachedb_file_t *base   = NULL;
	char           *opened = NULL;
	int				mode_length = strlen(mode);
	char            error_type  = ' ';

	if (!pdb || !file || !file_length || mode_length == 0 || mode_length > 2) {
		return FAILURE;
	}

	/* Zero-fill then entire cachedb control block */
	db = (cachedb_t*) ecalloc(sizeof(cachedb_t), 1);
	base   = &(db->base_file);	

	/* Initialize base name and length and ditto the containing directory */

	base->name              = estrndup(file, file_length);
	base->name_length       = file_length;
	base->dir               = estrndup(file, file_length);
	base->dir_length        = php_dirname(base->dir, file_length);

	db->tmp_file.dir        = estrdup(base->dir);
	db->tmp_file.dir_length = base->dir_length;

	switch(mode[0]) {
		case 'r':
			base->fp = php_stream_open_wrapper(
				base->name, "rb", 
				IGNORE_URL|REPORT_ERRORS|STREAM_MUST_SEEK, 
				&opened);
			db->mode ='r';
			break;
		case 'w': 
			if( base->fp = php_stream_open_wrapper(
					base->name, "rb", 
					IGNORE_URL|STREAM_MUST_SEEK, 
					&opened)) {
					db->mode = 'w';
				break;
			}
			/* no break if file doesn't exist: treate as create. */
		case 'c':
			db->mode = 'c';
			break;
		default:
			php_error_docref(NULL TSRMLS_CC, E_ERROR, _cachedb_mode_err, db->mode, db->base_file.name);
			cachedb_db_dtor(&db TSRMLS_CC);
			return FAILURE;
	}

	EFREE(opened);

	db->is_binary == (mode_length == 2 && mode[1] == 'b');

	/* Load the DB file stats or set a dummy create statrec in the case of a create */
	if (db->base_file.fp) {
		CHECKA(!php_stream_stat(db->base_file.fp, &(db->base_file.sb)));
	} else {
		/* Initialise the base file stat block if it exists.  We do this because the
		 * "Don't commit change if base file updated" rule still applies for a create.
		 * However the effective file length is still forced to 0 for indexing to work. 
		 */
		stat(db->base_file.name, &db->base_file.sb.sb);
		db->base_file.filelength = 0;
	}

	if(cachedb_load_index(db TSRMLS_CC)==SUCCESS){
		*pdb = db;
		return SUCCESS;   /* nornal return */
	}

error:

	/* index failed to load */
	cachedb_db_dtor(&db TSRMLS_CC);
	php_error_docref(NULL TSRMLS_CC, E_ERROR, "Corrupted index load in cachedb open", mode);
	return FAILURE;
}
/* }}} */

/* {{{ proto boolean _cachedb_close(struct db, char mode)
   Close the cachedb, if necessary replacing the db with an updated version */
PHPAPI int _cachedb_close(cachedb_t* db, char force_mode TSRMLS_DC)
{
	php_stream *new = NULL;
	struct stat sb;
	char *new_tmpname = NULL;
	int  base_ok;
	char error_type  = ' ';

	if (db->mode != 'r' && force_mode != 'r' && db->tmp_file.next_pos > 0) {
		size_t  len, zlen;
		char   *zbuf = NULL;
		zval   *list;
		zval   *tmp;
		cachedb_header_t hdr={"",0,0};
		size_t  dummy;

		/* The DB was opened in c or w mode and extra records have been added */ 
		new = php_stream_fopen_temporary_file(db->base_file.dir, ".cachedb_tmp_", &new_tmpname);

		/* write placeholder header (will soon be overwritten) */		
		CHECKA(php_stream_write(new, (const char *) &hdr, sizeof(hdr))==sizeof(hdr));
      
		/* Make shallow copies of index_list into a zval list */
		MAKE_STD_ZVAL(list);
		array_init_size(list, zend_hash_num_elements(db->index_list));
		zend_hash_copy(Z_ARRVAL_P(list), db->index_list, (copy_ctor_func_t) zval_add_ref, (void *)&tmp, sizeof(zval*));
		CHECKA(cachedb_write_var(new, 0, list, &zlen, &len TSRMLS_CC)==SUCCESS);
		zval_ptr_dtor(&list);

		/* Overwrite header with correct contents */
		memcpy(hdr.fingerprint, CACHEDB_HEADER_FINGERPRINT, sizeof(CACHEDB_HEADER_FINGERPRINT)-1);
		hdr.zlen = zlen;
		hdr.len  = len;
		php_stream_seek(new, 0, SEEK_SET);
		CHECKA(php_stream_write(new, (const char *) &hdr, sizeof(hdr))==sizeof(hdr));
		php_stream_seek(new, 0, SEEK_END);

		/* Now append base if it exists and temp file contents */
		if(db->base_file.fp) {
			php_stream_seek(db->base_file.fp, db->base_file.header_length, SEEK_SET);
			php_stream_copy_to_stream_ex(db->base_file.fp, new, PHP_STREAM_COPY_ALL, &dummy);
		}
		php_stream_seek(db->tmp_file.fp, 0, SEEK_SET);
		php_stream_copy_to_stream_ex(db->tmp_file.fp, new, PHP_STREAM_COPY_ALL, &dummy);
		php_stream_close(new);
	}

	if (db->tmp_file.fp) {
		php_stream_close(db->tmp_file.fp);
		db->tmp_file.fp = NULL;
	}

	if (db->base_file.fp) {
		php_stream_close(db->base_file.fp);
		db->base_file.fp = NULL;
	}

	if (new_tmpname) {
		int stat_status;
		/* If we've created a new temp file it may need moving to the old one 
		 *  - if the base file existed and still has the same mtime
         *  - if the base file didn't exist and still doesn't.
		 */
		stat_status = stat(db->base_file.name, &sb);

		if (db->base_file.sb.sb.st_size > 0) { /* size>0 means it existed */
			/* Most FS now store mtimes stamped to the nS, but we can't guarantee this so
             * the file-change check is based on the dev + inode + mtime 
			 */
			base_ok = (stat_status == 0) && 
				(db->base_file.sb.sb.st_ino   == sb.st_ino) &&
				(db->base_file.sb.sb.st_dev   == sb.st_dev) &&
				(db->base_file.sb.sb.st_mtime == sb.st_mtime);
		} else { /* it didn't exist or was unchanged */
			base_ok = (stat_status == -1) ||
			   ((db->base_file.sb.sb.st_ino   == sb.st_ino) &&
				(db->base_file.sb.sb.st_dev   == sb.st_dev) &&
				(db->base_file.sb.sb.st_mtime == sb.st_mtime));
		}

		if (base_ok) {
			rename(new_tmpname, db->base_file.name);
		} else {
			unlink(new_tmpname);
		}
		EFREE(new_tmpname);
	}	
	
	cachedb_db_dtor(&db TSRMLS_CC);
	return SUCCESS;

error:

	php_error_docref(NULL TSRMLS_CC, E_ERROR, _cachedb_mode_err, db->mode, db->base_file.name);
	cachedb_db_dtor(&db TSRMLS_CC);
	return FAILURE;
}
/* }}} */

/* {{{ proto boolean _cachedb_find(struct db)
   Set the record position at the specified key, returning a boolean to indicate if the key exists */
PHPAPI int _cachedb_find(cachedb_t* db, char *key, size_t key_length, zval *metadata TSRMLS_DC)
{
	zval          **entry = NULL;
	zval          **ndx, **start, **zlen, **len, **meta=NULL;
	cachedb_rec_t *rec = &(db->last_find);
	char           error_type  = ' ';
	
	if (hash_find(db->index_hash, key, entry)==SUCCESS) {

		HashTable *entry_hash = Z_ARRVAL_PP(entry);
		HashTable *entry_list;

		CHECKA(hash_index_find(entry_hash, 0, ndx) == SUCCESS);
		CHECKA(hash_index_find(entry_hash, 1, start) == SUCCESS);
		CHECKA(hash_index_find(db->index_list, Z_LVAL_PP(ndx), entry) == SUCCESS);

		entry_list = Z_ARRVAL_PP(entry);
		CHECKA(hash_index_find(entry_list, 1, zlen) == SUCCESS);
		CHECKA(hash_index_find(entry_list, 2, len) == SUCCESS);

		rec->key        = key;
    	rec->key_length = key_length;
		rec->is_base    = (Z_LVAL_PP(start) < db->base_file.filelength);
		rec->start      = rec->is_base ? Z_LVAL_PP(start) : Z_LVAL_PP(start) - db->base_file.filelength;
		rec->zlen       = Z_LVAL_PP(zlen);
		rec->len        = Z_LVAL_PP(len);

		/* return any metadata if it exists and the metadata argument has been supplied */
		if (metadata && hash_index_find(entry_list, 3, meta) == SUCCESS) {
			zval *tmp_zval;
			zval_dtor(metadata);
			array_init(metadata);
			zend_hash_copy(HASH_OF(metadata), HASH_OF(*meta), 
			               (copy_ctor_func_t) zval_add_ref, 
  			               (void *) &tmp_zval, sizeof(zval *));
		}
		return SUCCESS;

	} else {

		memset(rec, 0, sizeof(cachedb_rec_t));
		return FAILURE;
	}

error:
	/* Find only uses internal stuctures so any CHECKA errors are fatal and should abort */	
	php_error_docref(NULL TSRMLS_CC, E_ERROR, "invalid find for %s in file %s", key, db->base_file.name);
	return FAILURE;
}
/* }}} */

/* {{{ proto boolean _cachedb_fetch(struct db, zval &value)
   Fetch the current record */
PHPAPI int _cachedb_fetch(cachedb_t* db, zval *value TSRMLS_DC)
{
	cachedb_rec_t         *rec           = &(db->last_find);
	size_t                 zlen          = rec->zlen;
	int                    is_base_fetch = rec->is_base;
	cachedb_file_t        *file          = is_base_fetch ? &(db->base_file) : &(db->tmp_file);
	char                   error_type    = ' ';

	if (zlen == 0) {
		return 0;    /* last find failed so can't do a fetch */
	}

	if (rec->start != file->next_pos) {
		php_stream_seek(file->fp, rec->start, SEEK_SET);
	}

	if ( cachedb_read_var(file->fp, db->is_binary, value, zlen, rec->len TSRMLS_CC) == SUCCESS) {
		file->next_pos = rec->start + zlen;
		return SUCCESS;
	} else {
		rec->start = 0;
		return FAILURE;
	}
}
/* }}} */

/* {{{ proto boolean _cachedb_add(struct db, string key, int key_length, vzal value)
   Add a pending record to the cachedb */
PHPAPI int _cachedb_add(cachedb_t* db, char *key, size_t key_length, zval *value, zval *metadata TSRMLS_DC)
{
	void           *dummy;
	zval           *tmp;
	size_t          len, zlen, ndx;
	cachedb_file_t *tf = &(db->tmp_file);
	char            error_type  = ' ';

	if (db->mode=='r' || hash_find(db->index_hash, key, dummy) == SUCCESS) {
		return FAILURE; /* Cannot add to a R/O DB or if the key already exists! */
	}

	CHECKA(!db->is_binary || Z_TYPE_P(value) == IS_STRING);
 
	if (tf->fp == 0) {
		char           *opened = NULL;

		/* Open and initialise temporary output file which holds added records */
		tf->fp = php_stream_fopen_temporary_file(tf->dir, ".cachedb_otmp_", &opened);
  		CHECKA(tf->fp > 0 && opened != NULL && strlen(opened) > 0);

		/* collect the filename and unlink the file so that it is automatical garbage collected on closure */
		tf->name        = opened;
		tf->name_length = strlen(opened);
		unlink(tf->name);
		tf->next_pos    = 0;
	}

	/* Any additions are written to the end of the temporary file */ 
	if (tf->next_pos != tf->filelength) {
		php_stream_seek(tf->fp, 0, SEEK_END);
		CHECKA(php_stream_tell(tf->fp) == tf->filelength);
	}
	CHECKA(cachedb_write_var(tf->fp, db->is_binary, value, &zlen, &len TSRMLS_CC)==SUCCESS);
	tf->filelength += zlen;
	tf->next_pos    = tf->filelength;

	/* Update index_list and index_hash */
	ndx = zend_hash_num_elements(db->index_list);
	MAKE_STD_ZVAL(tmp);
	array_init_size(tmp, (metadata ? 4 : 3));
	add_next_index_stringl(tmp, key, key_length, 1);
	add_next_index_long(tmp, zlen);
	add_next_index_long(tmp, len);
	if (metadata) {
		add_next_index_zval(tmp, metadata);
		Z_ADDREF_P(metadata);
	}
	hash_add_next_index_zval(db->index_list, tmp);

	MAKE_STD_ZVAL(tmp);
	array_init_size(tmp, 2);
	add_next_index_long(tmp, ndx);
	add_next_index_long(tmp, db->base_file.filelength + (tf->next_pos -zlen) );
	hash_add(db->index_hash, key, tmp);

	return SUCCESS;

error:
	php_error_docref(NULL TSRMLS_CC, E_ERROR, _cachedb_add_err, db->base_file.name);
	return FAILURE;

}
/* }}} */

/* {{{ proto boolean _cachedb_info(struct db)
   Return a copy of the cachedb index */
PHPAPI int _cachedb_info( zval **info, cachedb_t* db TSRMLS_DC)
{
	zval *list, *hash;
	zval *dummy = NULL;
	char error_type  = ' ';

	/* Make shallow copies of index_list and index_hash into zvals */
	MAKE_STD_ZVAL(list);
	array_init_size(list, zend_hash_num_elements(db->index_list));
	hash_copy(Z_ARRVAL_P(list), db->index_list);

	MAKE_STD_ZVAL(hash);
	array_init_size(hash, zend_hash_num_elements(db->index_hash));
	hash_copy(Z_ARRVAL_P(hash), db->index_hash);

	array_init_size(*info, 2);
	add_next_index_zval(*info, list);
	add_next_index_zval(*info, hash);

	return SUCCESS;
}
/* }}} */

/* {{{ proto boolean cachedb_load_index(struct db)
   Load the initial index from the DB */

/* The DB index is in two formats: On disk, it is maintained in the form of a compressed serialized
 * array where the i'th element is the three element zval array: [file_name, compressed_length, 
 * uncompressed_length].  In memory a second keyed array is built on loading to simplify lookup: 
 * file_name => array(element_index,file_offset).
 */
static int cachedb_load_index(cachedb_t* db TSRMLS_DC)
{
	cachedb_header_t    header;
	zval               *index      = NULL;
	HashTable          *index_list = NULL;
	HashTable          *index_hash = NULL;
	uint                ndx_start  = sizeof(header);
	char                error_type = ' ';

	if (db->base_file.fp > 0) {
		zval **entry = NULL;

		CHECKA(php_stream_read(db->base_file.fp, (char *) &header, sizeof(header)) == sizeof(header) &&
		       memcmp(header.fingerprint, CACHEDB_HEADER_FINGERPRINT, sizeof(CACHEDB_HEADER_FINGERPRINT)-1)==0);
		MAKE_STD_ZVAL(index);
		CHECKA(cachedb_read_var(db->base_file.fp, 0, index, 
		                                   header.zlen, header.len TSRMLS_CC) == SUCCESS &&
		       Z_TYPE_P(index) == IS_ARRAY);

		ndx_start                  += header.zlen;
		db->base_file.next_pos      = ndx_start;
		db->base_file.header_length = ndx_start;

		/* Extract the Hashtable ptr from index and free its zval storage */
		index_list = Z_ARRVAL_P(index); 
		EFREE(index); 

		/* Initialise another HashTable the same size as index_list for keyed access */
		ALLOC_HASHTABLE(index_hash);
		hash_init(index_hash, hash_count(index_list));

		/* loop over index_list to build the index_hash */
		for (hash_reset(index_list); hash_get(index_list, entry) == SUCCESS; hash_next(index_list)) {

			HashTable* entry_hash = Z_ARRVAL_PP(entry);
			zval **zkey, **zlen, *tmp;
			char *dummy, *key;
			uint  dummy_length, key_length, entry_length;
			ulong ndx;

			CHECKA(Z_TYPE_PP(entry) == IS_ARRAY); 
			entry_length = hash_count(entry_hash);
			CHECKA(entry_length == 3 || entry_length == 4);

			/* Retrieve the next value and its index. Pick out the file path */
            CHECKA(hash_key(index_list, dummy, ndx) == HASH_KEY_IS_LONG);
			CHECKA(hash_index_find(entry_hash, 0, zkey) == SUCCESS);
			CHECKA(hash_index_find(entry_hash, 1, zlen) == SUCCESS);
			key        = Z_STRVAL_PP(zkey);
			key_length = Z_STRLEN_PP(zkey);

			/* Set index_hash[key] = array(ndx,nxt_start); */
			MAKE_STD_ZVAL(tmp);
			array_init_size(tmp,2);
			add_next_index_long(tmp, ndx);
			add_next_index_long(tmp, ndx_start);

			hash_add(index_hash,key,tmp);

			ndx_start += Z_LVAL_PP(zlen);
		}

	} else { /* DB creation starts with an empty index array and hash */

		ALLOC_HASHTABLE(index_list);
		hash_init(index_list, 0);
		ALLOC_HASHTABLE(index_hash);
		hash_init(index_hash, 0);
		ndx_start = 0;
	}

	CHECKA(ndx_start==(db->base_file.filelength));
	db->index_list    = index_list;
	db->index_hash    = index_hash;

	return SUCCESS;

error:

	/* Don't bother with freeing memory as this will be swept up by cachedb_db_dtor() */
	php_error_docref(NULL TSRMLS_CC, E_ERROR, _cachedb_ndx_err, db->base_file.name);
	return FAILURE;
}
/* }}} */

/* {{{ proto boolean cachedb_read_var(php_stream fp, bool is_binary, zval &value)
   Fetch the current record */
static int cachedb_read_var(php_stream *fp, int is_binary, zval *value, size_t zlen, size_t len TSRMLS_DC)
{
	char                  *buf        = NULL;
	const unsigned char   *p;
	char                   error_type  = ' ';

	if (is_binary) {
		MAKE_STD_ZVAL(value);
		CHECKA(len == php_stream_copy_to_mem(fp, &buf, len, 0));
		ZVAL_STRINGL(value, buf, len, 0);

	} else {
		char                  *zbuf       = NULL;
		php_unserialize_data_t var_hash;
		size_t                 buf_length = len;
		int                    status;

		/* copy relevant stream to zbuf and update the relevant next pos */
		CHECKA(zlen == php_stream_copy_to_mem(fp, &zbuf, zlen, 0));

		/* uncompress the buffer and free the zbuf */
		buf = emalloc(buf_length+1);
		buf[buf_length]=(char) 0;      /* zero terminate buf to simply debugging */
		CHECKA(uncompress(buf, &buf_length, zbuf, zlen)==Z_OK && buf_length==len);
		PEFREE(zbuf,0);

		/* Unserialize the buffer into the returned zval value. 
		   Note that this zval has been preallocated and initialised by the caller */ 
		p = (const unsigned char*) buf;
		PHP_VAR_UNSERIALIZE_INIT(var_hash);
	 	status = php_var_unserialize(&value, &p, p + buf_length, &var_hash TSRMLS_CC);
		EFREE(buf);
		PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
		CHECKA(status);
	}
	return SUCCESS;

error:
	php_error_docref(NULL TSRMLS_CC, E_ERROR, _cachedb_eom_err);
	return FAILURE;
}
/* }}} */

/* {{{ proto boolean cachedb_write_var(php_stream fp, zval &value)
   Append the current record to the specified file */
static int cachedb_write_var(php_stream *fp, int is_binary, zval *value, size_t *zlen, size_t *len TSRMLS_DC)
{
	size_t               buf_length;
	char                 error_type  = ' ';

	if (is_binary) {
		const char *buf;
		CHECKA(Z_TYPE_P(value) == IS_STRING);

		buf = (const char *) Z_STRVAL_P(value);
		buf_length = Z_STRLEN_P(value);

		CHECKA(php_stream_write(fp, buf, buf_length) == buf_length);

		*zlen = buf_length;

	} else { /* is serializable */
		size_t               zbuf_length;
		php_serialize_data_t var_hash;
		char                *zbuf      = NULL;
		zval                *var       = value;
		smart_str            buf       = {NULL, 0, 0};

		/* Serialize zval list into buf then destroy list*/
		PHP_VAR_SERIALIZE_INIT(var_hash);
		php_var_serialize(&buf, &var, &var_hash TSRMLS_CC);
		PHP_VAR_SERIALIZE_DESTROY(var_hash);	
		buf_length = buf.len;

		/* Allocate zbuf len based on worst case for compression, then compress and free original buffer */
		zbuf_length = compressBound(buf_length) + 1;
		zbuf = (char *) emalloc(zbuf_length);
		CHECKA(compress(zbuf, &zbuf_length, buf.c, buf_length) == Z_OK);
		smart_str_free(&buf);

		/* Now write out the buffer to file */
		CHECKA(php_stream_write(fp, (const char *) zbuf, zbuf_length) == zbuf_length);
		efree(zbuf);
		*zlen = zbuf_length;
	}

	*len  = buf_length;
	return SUCCESS;

error:
	php_error_docref(NULL TSRMLS_CC, E_ERROR, _cachedb_write_err);
	return FAILURE;
}
/* }}} */

/* {{{ proto void cachedb_db_dtor(struct db)
   Cachedb record destructor */
static void cachedb_db_dtor(cachedb_t** pdb TSRMLS_DC)
{
	cachedb_t *db = *pdb;

	if(db->base_file.fp) {
		php_stream_close(db->base_file.fp);
	}
	if(db->tmp_file.fp) {
		php_stream_close(db->tmp_file.fp);
		unlink(db->tmp_file.name);
	}
	EFREE(db->base_file.name);	
	EFREE(db->base_file.dir);	
	EFREE(db->tmp_file.name);	
	EFREE(db->tmp_file.dir);	
	
	ZEND_HASH_DESTROY(db->index_list);
	EFREE(db->index_list);
	ZEND_HASH_DESTROY(db->index_hash);
	EFREE(db->index_hash);
	EFREE(db);
	*pdb = NULL;
}
/* }}} */
