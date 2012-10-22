/*
   +----------------------------------------------------------------------+
   | PHP Version 5                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2012 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Author: Terry Ellison <Terry@ellisonsorg.uk>                         |
   +----------------------------------------------------------------------+
 */

/* Cachedb is influenced by D.J.Bernstein's cdb-0.75 (http://cr.yp.to/cachedb.html) and this is 
 * acknowledged with thanks.  Like cdb, cachedb is a file-based (piecewise) constant D/B. However
 * unlike cdb, it is also influenced by the relative exponential growth in memory and processing 
 * capacity of current systems whilst physical I/O performance has remained pretty constant.  The 
 * main usecase is for a file-based cache of idempotent objects which are read frequently, but very 
 * rarely changed or extended, e.g. language tables in complex application such as MediaWiki.  The 
 * only form of deletion and record update is to open the database in truncate mode.
 *
 * The implementation is made up of two files: cachedb.c and php_cachedb.c with coresponding 
 * headers.  The cachedb c and h files are designed to be callable from any PHP extension. See file
 * cachedb.c for the main documentation on its functionality.  The php_cachedb c and h files enable
 * cachedb to loaded as a standalone extension (and tested standalone).
 *
 * I had hoped to implement cachedb as a plugin extension to DBA, but unfortunately DBA does not 
 * provide an extensible API to allow the addition of extra DBA handlers, and patching the original
 * isn't practical for production evaluation as this would demote a core extension to EXPERIMENTAL.  
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_cachedb.h"

#include <sys/types.h>
#include <fcntl.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

/* PHP Includes */
#include "ext/standard/file.h"
#include "ext/standard/info.h"
#include "ext/standard/php_string.h"

static PHP_MINIT_FUNCTION(cachedb);
static PHP_MSHUTDOWN_FUNCTION(cachedb);
static PHP_MINFO_FUNCTION(cachedb);
static PHP_FUNCTION(cachedb_open);
static PHP_FUNCTION(cachedb_exists);
static PHP_FUNCTION(cachedb_fetch);
static PHP_FUNCTION(cachedb_add);
static PHP_FUNCTION(cachedb_info);
static PHP_FUNCTION(cachedb_close);

/* {{{ arginfo 
*/
ZEND_BEGIN_ARG_INFO_EX(arginfo_cachedb_open, 0, 0, 1)
	ZEND_ARG_INFO(0, path)
	ZEND_ARG_INFO(0, mode)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_cachedb_exists, 0, 0, 1)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, handle)
	ZEND_ARG_INFO(1, metadata)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_cachedb_fetch, 0, 0, 1)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, handle)
	ZEND_ARG_INFO(1, metadata)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_cachedb_add, 0, 0, 2)
	ZEND_ARG_INFO(0, key)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, handle)
	ZEND_ARG_INFO(0, metadata)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_cachedb_info, 0, 0, 0)
	ZEND_ARG_INFO(0, handle)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_cachedb_close, 0, 0, 0)
	ZEND_ARG_INFO(0, handle)
	ZEND_ARG_INFO(0, mode)
ZEND_END_ARG_INFO()
/* }}} */

/* {{{ cachedb_functions[]
 */
const zend_function_entry cachedb_functions[] = {
	PHP_FE(cachedb_open,   arginfo_cachedb_open)
	PHP_FE(cachedb_exists, arginfo_cachedb_exists)
	PHP_FE(cachedb_fetch,  arginfo_cachedb_fetch)
	PHP_FE(cachedb_add,    arginfo_cachedb_add)
	PHP_FE(cachedb_info,   arginfo_cachedb_info)
	PHP_FE(cachedb_close,  arginfo_cachedb_close)
	PHP_FE_END
};
/* }}} */

#ifdef ZTS
#  define CACHEDB_G(v) TSRMG(cachedb_globals_id, zend_cachedb_globals *, v)
#else
#  define CACHEDB_G(v) (cachedb_globals.v)
#endif
//extern int cachedb_globals_id;
//extern zend_cachedb_globals cachedb_globals;

#define CHECK_HANDLE(d,h)   \
	if (h >= 0 && h < MAX_DB_FILES && (CACHEDB_G(db))[h] != NULL) {  \
		d = (CACHEDB_G(db))[h];  \
	} else {  \
		RETURN_FALSE;  \
	}

/* {{{ Globals and Module struct */

#define MAX_DB_FILES 10
ZEND_BEGIN_MODULE_GLOBALS(cachedb)
	cachedb_pt db[MAX_DB_FILES];
ZEND_END_MODULE_GLOBALS(cachedb)

ZEND_DECLARE_MODULE_GLOBALS(cachedb)

PHP_RINIT_FUNCTION(cachedb);
PHP_RSHUTDOWN_FUNCTION(cachedb);
PHP_MINFO_FUNCTION(cachedb);

zend_module_entry cachedb_module_entry = {
	STANDARD_MODULE_HEADER_EX,
	NULL,
	NULL,
	"cachedb",                   /* extension name */
	cachedb_functions,           /* function list */
	NULL,                        /* No process startup */
	NULL,                        /* process shutdown */
	PHP_RINIT(cachedb),          /* request startup */
	PHP_RSHUTDOWN(cachedb),      /* request shutdown */
	PHP_MINFO(cachedb),          /* extension info */
	PHP_VERSION,                 /* extension version */
	PHP_MODULE_GLOBALS(cachedb), /* globals descriptor */
	NULL,                        /* No globals ctor */
	NULL,                        /* No globals dtor */
	NULL,                        /* No post deactivate */
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_CACHEDB
ZEND_GET_MODULE(cachedb)
#endif
/* }}} */

/* {{{ PHP Request Initialisation Function
 * The only request initation is to zero out the global db array 
 */
PHP_RINIT_FUNCTION(cachedb)
{
	cachedb_t **p;
	int i, *pi;

	p = CACHEDB_G(db);
	for (i=0; i<MAX_DB_FILES; i++) {
		*p++ = NULL;
	}
	return SUCCESS;
}
/* }}} */

/* {{{ PHP Request Shutdown Function
 * The only request shutdown is to close any open DBs (readonly, that is any
 * pending additions at dumped -- the penalty of not doing an explicit close).  
 * The corresponding the global db array entries are nulled.
 */
PHP_RSHUTDOWN_FUNCTION(cachedb)
{
	cachedb_t **p;
	int i;

	p = CACHEDB_G(db);
	for (i=0; i<MAX_DB_FILES; i++, p++) {
		if (*p != NULL) {
			cachedb_close2(*p, 'r');
			*p = NULL;
		}
	}
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(cachedb)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "CacheDB Support", "Enabled");
	php_info_print_table_end();
}
/* }}} */

/* {{{ proto handle cachedb_open(string file, string mode)
   Opens a new cachedb file */
PHP_FUNCTION(cachedb_open)
{
	char       *file;   /* The file to open */
	char       *mode = NULL;   /* The mode to open the stream with */
	int         file_length, mode_length, i;
	cachedb_t **pdb;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &file, &file_length, &mode, &mode_length) == FAILURE || 
        mode_length == 0 || mode_length > 2) {
		return; 
	}

	/* Search for empty slot in the global db array */ 
	pdb = CACHEDB_G(db);
	for (i=0; i<MAX_DB_FILES; i++, pdb++) {
		if (*pdb == NULL) {
		   /* The slot is available.  Note that mode[0] is [rwc]:
			* r: Read
			* w: Write
			* c: Create/Truncate
			* however the open function validates this.
			*/
			if (cachedb_open(pdb, file, file_length, mode)==SUCCESS) {
				RETURN_LONG(i);
			} else {
				RETURN_FALSE;
			}				
		}
	}
	RETURN_FALSE;
}
/* }}} */

/* {{{ proto boolean cachedb_exists(string key[[, int handle], array metadata])
   Check if a key exists in the cache */
PHP_FUNCTION(cachedb_exists)
{
	char        *key;        /* The key to be checked */
	int          key_length;
	zval        *metadata=NULL;   /* Optional to be returned */
	long         handle=0;   /* The handle to be used (default 0) */
	cachedb_t   *db;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|lz/", &key, &key_length, &handle, &metadata) == FAILURE) {
		return;
	}

	CHECK_HANDLE(db,handle);
	RETURN_BOOL(cachedb_find(db, key, key_length, metadata)==SUCCESS);
}
/* }}} */

/* {{{ proto string cachedb_fetch(string key[[, int handle], array metadata] )
   Reads the value for a given key and returns FALSE on key missing */
PHP_FUNCTION(cachedb_fetch)
{
	char        *key=NULL;        /* The key of record to be fetched */
	int          key_length=0;
	zval        *metadata=NULL;        /* Optional to be returned */

	long         handle=0;        /* The handle to be used (default 0) */
	cachedb_t   *db;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|lz/", &key, &key_length, &handle, &metadata) == FAILURE) {
		return;
	}

	CHECK_HANDLE(db,handle);
	if (cachedb_find(db, key, key_length, metadata)==FAILURE) {
		RETURN_FALSE;
	}

	if(return_value_used) {
		cachedb_fetch(db, return_value);
	}
}
/* }}} */

/* {{{ proto boolean cachedb_add(string key, string value[[, int handle], array metadata])
   Add a key with the given value returns FALSE on failure e.g. key already exists */
PHP_FUNCTION(cachedb_add)
{
	char            *key;           /* The key of record to be added */
	int              key_length;
	zval            *value;         /* The value to be set */
	int              value_length;
	zval            *metadata=NULL; /* Optional to be added */
	long             handle=0;      /* The handle to be used (default 0) */
	cachedb_t       *db;
	cachedb_rec_t    entry;
	int              status;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sz|la", &key, &key_length, &value, &handle, &metadata) == FAILURE) {
		return;
	}

	CHECK_HANDLE(db,handle);

	status = (cachedb_add(db, key, key_length, value, metadata)==SUCCESS);

	RETURN_BOOL(status);
}
/* }}} */

/* {{{ proto handle cachedb_info([int handle])
   Returns an info array on the specified DB  */
PHP_FUNCTION(cachedb_info)
{
	long             handle=0;   /* The handle to be used (default 0) */
	cachedb_t       *db;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &handle) == FAILURE) {
		return;
	}

	CHECK_HANDLE(db,handle);
	cachedb_info(return_value, db);
}
/* }}} */

/* {{{ proto boolean cachedb_close(string mode[, int handle])
   Closes a cachedb DB, optionally committing additions or truncating the DB */
PHP_FUNCTION(cachedb_close)
{
	char       *mode=NULL;   /* The mode to close the stream with */
	int         mode_length, i;
	long        handle=0;   /* The handle to be used (default 0) */
	cachedb_t **pdb;
	cachedb_t   *db;
	int         status;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|ls", &handle, &mode, &mode_length) == FAILURE ||
        (mode != NULL && mode_length!=1)) {
		return;
	}

	pdb = CACHEDB_G(db);
	CHECK_HANDLE(db,handle);
	
	status = cachedb_close2(db, (mode ? mode[0] : '*'));

	pdb[handle] = NULL;

	RETURN_BOOL(status==SUCCESS);
}
/* }}} */
/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: sw=4 ts=4 fdm=marker
 * vim<600: sw=4 ts=4
 */
