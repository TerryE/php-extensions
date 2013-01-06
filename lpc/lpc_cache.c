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

   This software was derived from the APC extension which was initially 
   contributed to PHP by Community Connect Inc. in 2002 and revised in 2005 
   by Yahoo! Inc. See README for further details.
 
   All other licensing and usage conditions are those of the PHP Group.
*/

#include <zlib.h>

#include "SAPI.h"

#include "lpc.h"
#include "lpc_cache.h"
#include "lpc_pool.h"
#include "lpc_request.h"

typedef struct _cachedb_t cachedb_t, *cachedb_pt;

/* {{{ private struct definitions: lpc_cache_t */
struct _lpc_cache_t {
    cachedb_t           *db;           /* CacheDB database to be used to hold the filesysten copy */
    HashTable           *index;        /* Local index array of filename=>array(...) */
    time_t               mtime;        /* mtime of the cache */
    off_t                filesize;
    lpc_request_context_t *context;
};
/* }}} */

/* {{{ private global references to cachedb extension */
static struct _cachedb {
    int (*_cachedb_open)(cachedb_t** pdb, char *file,   size_t file_len, char *mode TSRMLS_DC);
    int (*_cachedb_close)(cachedb_t*  db, char mode TSRMLS_DC);
    int (*_cachedb_find)(cachedb_t*  db,  char  *key,   size_t key_len, zval *metadata TSRMLS_DC);
    int (*_cachedb_fetch)(cachedb_t*  db,  zval *value TSRMLS_DC);
    int (*_cachedb_add)(cachedb_t*  db,  char  *key,   size_t key_len, 
                       zval *value, zval *metadata TSRMLS_DC);
    int (*_cachedb_info)(zval **info, cachedb_t* db TSRMLS_DC);
} cdb;

/* {{{ Public macros to make the calling code more readable */
#define cachedb_open(p,f,fl,m)   cdb._cachedb_open(p,f,fl,m TSRMLS_CC)
#define cachedb_close(db)        cdb._cachedb_close(db, '*' TSRMLS_CC)
#define cachedb_close2(db,m)     cdb._cachedb_close(db, m TSRMLS_CC)
#define cachedb_find(db,k,kl,m)  cdb._cachedb_find(db,k,kl, m TSRMLS_CC)
#define cachedb_fetch(db,v)      cdb._cachedb_fetch(db,v TSRMLS_CC)
#define cachedb_add(db,k,kl,v,m) cdb._cachedb_add(db,k,kl,v,m TSRMLS_CC)
#define cachedb_info(rv,db)      cdb._cachedb_info(&rv,db TSRMLS_CC)
/* }}} */

/* {{{ Some application-friendly synonyms for some of the hash functions used. */

/* Note that by convention PHP includes the terminating zero in string keys, hence the +1 on these lengths */ 
#define hash_find(e,k,v) zend_hash_find(e, k, k##_length+1, (void **) &v)
#define hash_index_find(e,i,v) zend_hash_index_find(e, i, (void **) &v)
#define hash_add_next_index_zval(h,v) zend_hash_next_index_insert(h, &v, sizeof(zval *), NULL)
#define hash_add(h,k,v) zend_hash_add(h,k,strlen(k)+1, &v, sizeof(zval *), NULL)
#define hash_add(h,k,v) zend_hash_add(h,k,strlen(k)+1, &v, sizeof(zval *), NULL)
#define hash_copy(to,fm,dmy) zend_hash_copy(to, fm, (copy_ctor_func_t) zval_add_ref, (void *)&dmy, sizeof(zval*))
#define hash_count(h) zend_hash_num_elements(h)
#define hash_init(h,c) zend_hash_init(h, c, NULL, ZVAL_PTR_DTOR, 0)
#define hash_reset(h) zend_hash_internal_pointer_reset(h)
#define hash_get(h,e) zend_hash_get_current_data(h, (void **) &e)
#define hash_key(h,s,l) zend_hash_get_current_key_ex(h, &s, &s##_length, &l, 0, 0)
#define hash_next(h) zend_hash_move_forward(h)
#define hash_get_first_zv(h,pzv) hash_reset(h); hash_get(h, pzv); 
#define hash_get_next_zv(h,pzv) hash_next(h); hash_get(h, pzv); 
#define hash_get_last_zv(h,pzv) zend_hash_internal_pointer_end(h); hash_get(h, pzv);
#define hash_get_prev_zv(h,pzv) zend_hash_move_backwards(h); hash_get(h, pzv); 

#define CHECK(p) if(!(p)) goto error;
/* }}} */

/* {{{ lpc_cache_create */
zend_bool lpc_cache_create(uint *max_module_len TSRMLS_DC)
{ENTER(lpc_cache_create)
    lpc_cache_t           *cache = (lpc_cache_t*) ecalloc(1, sizeof(lpc_cache_t));
    struct stat           *sb;
    zend_uint              max_len = 0;
    lpc_request_context_t *r_cxt;
    zval         **zctxt, *zinfo, **zlist;
    HashTable    *hlist, *hindex;
    char         *dummy;

    if (!cdb._cachedb_open) {
        if ((cdb._cachedb_open  = lpc_resolve_symbol("_cachedb_open" TSRMLS_CC)) &&
            (cdb._cachedb_close = lpc_resolve_symbol("_cachedb_close" TSRMLS_CC)) &&
            (cdb._cachedb_find  = lpc_resolve_symbol("_cachedb_find" TSRMLS_CC)) &&
            (cdb._cachedb_fetch = lpc_resolve_symbol("_cachedb_fetch" TSRMLS_CC)) &&
            (cdb._cachedb_add   = lpc_resolve_symbol("_cachedb_add" TSRMLS_CC)) &&
            (cdb._cachedb_info  = lpc_resolve_symbol("_cachedb_info" TSRMLS_CC))) {
        } else {
            lpc_warning("Cannot map CacheDB extension, falling back to default compile" TSRMLS_CC);
            return FAILURE;
        }
    }

    cache->context = r_cxt = LPCG(request_context);

    /* Open the CacheDB using the cache name and obtain the directory info */
    CHECK(cachedb_open(&cache->db, r_cxt->cachedb_fullpath,
                       strlen(r_cxt->cachedb_fullpath), "wb") == SUCCESS);
    MAKE_STD_ZVAL(zinfo);
    CHECK(cachedb_info(zinfo,cache->db)==SUCCESS);

    /* Point hlist to the first of the two arrays in the info return */
    hash_get_first_zv(Z_ARRVAL_P(zinfo), zlist);
    hlist = Z_ARRVAL_PP(zlist);

    hindex = emalloc(sizeof(HashTable));
    hash_init(hindex, hash_count(hlist));

    if( hash_count(hlist) > 0 ) {
       /*
        * The cache files exists. Get the 1st entry, the cache context record and validate that the
        * cache context is still valid. Note that this record is a funny in that the record payload
        * is a dummy and all of the material content is in the metadata fields. The compression algo
        * is cached as this persists for the life of the cache.
        */
        zval **zcontext, **zctxt_metadata, **zPHP_version, **zdir, **zbasename, **zmtime, 
             **zfilesize, **zcomp_algo, **zle, *zie; 
 
        hash_get_first_zv(hlist, zcontext);
        hash_get_last_zv(Z_ARRVAL_PP(zcontext),  zctxt_metadata);

        hash_get_first_zv(Z_ARRVAL_PP(zctxt_metadata),zPHP_version);
        hash_get_next_zv(Z_ARRVAL_PP(zctxt_metadata),zdir);
        hash_get_next_zv(Z_ARRVAL_PP(zctxt_metadata),zbasename);
        hash_get_next_zv(Z_ARRVAL_PP(zctxt_metadata),zmtime);
        hash_get_next_zv(Z_ARRVAL_PP(zctxt_metadata),zfilesize);
        hash_get_next_zv(Z_ARRVAL_PP(zctxt_metadata),zcomp_algo);

        if(!strcmp(Z_STRVAL_PP(zPHP_version), r_cxt->PHP_version) &&
           !strcmp(Z_STRVAL_PP(zdir), r_cxt->request_dir) &&
           !strcmp(Z_STRVAL_PP(zbasename), r_cxt->request_basename) &&
           Z_LVAL_PP(zmtime) == r_cxt->request_mtime &&
           Z_LVAL_PP(zfilesize) == r_cxt->request_filesize &&
           !r_cxt->clear_flag_set) {
            LPCG(compression_algo) = Z_LVAL_PP(zcomp_algo); /* Use the DB ver of the omp algo */
           /*
            * The cache is OK to use so loop over the remaining list array setting up zle to
            * enumerate the elements of: 
            *     hlist = array( array(filename, zlen, len, metadata), ... ) 
            * and creating a new 
            *     index = array( filename=>array(len, mtime, filesize, pool_len), ... ) 
            */

            for (hash_next(hlist); hash_get(hlist, zle) == SUCCESS; hash_next(hlist)) {
                zval **zfilename, /* **zzlen, */ **zlen, **zmetadata;

                /* Pick up the len and metadata from the list entry */
                hash_get_first_zv(Z_ARRVAL_PP(zle), zfilename);
                hash_next(Z_ARRVAL_PP(zle));                   /* skip zzlen */ 
                hash_get_next_zv(Z_ARRVAL_PP(zle),  zlen);
                if (Z_LVAL_PP(zlen) > max_len) {
                    max_len = Z_LVAL_PP(zlen);
                }
                hash_get_next_zv(Z_ARRVAL_PP(zle),  zmetadata);

                MAKE_STD_ZVAL(zie);
                array_init_size(zie, 4);
                add_next_index_long(zie, Z_LVAL_PP(zlen));
                php_array_merge(Z_ARRVAL_P(zie), Z_ARRVAL_PP(zmetadata), 0 TSRMLS_CC);

                hash_add(hindex,Z_STRVAL_PP(zfilename),zie);
            }
        } else {
            /* There is a mismatch so unlink the cache and turn of caching for the remainder
             * of this request.  The next will rebuild a new cache. */
            LPCG(lpc_cache) = cache;
            lpc_cache_clear(TSRMLS_C);
            lpc_cache_destroy(TSRMLS_C);
            return FAILURE;
        }
    
    } else {
        /* This is a new cache file.  Create the context record and add it to the cache */
        zval *zctxt_metadata, *zdummy;
        int dummy = 0;
        char context_key[] = "_ context _";

        MAKE_STD_ZVAL(zdummy);  /* cachedb currently doesn't support 0 length records */
        ZVAL_STRINGL(zdummy, (char *) &dummy, sizeof(dummy), 1); 

        MAKE_STD_ZVAL(zctxt_metadata);
        array_init_size(zctxt_metadata, 5);
        add_next_index_string(zctxt_metadata, r_cxt->PHP_version, 1);
        add_next_index_string(zctxt_metadata, r_cxt->request_dir, 1);
        add_next_index_string(zctxt_metadata, r_cxt->request_basename, 1);
        add_next_index_long(zctxt_metadata, r_cxt->request_mtime);
        add_next_index_long(zctxt_metadata, r_cxt->request_filesize);
        add_next_index_long(zctxt_metadata, LPCG(compression_algo));

        CHECK(cachedb_add(cache->db, context_key, sizeof(context_key)-1, 
                          zdummy, zctxt_metadata)==SUCCESS);
        zval_dtor(zdummy);
        FREE_ZVAL(zdummy);

        if (!Z_DELREF_P(zctxt_metadata)) {
            zval_dtor(zctxt_metadata);
            FREE_ZVAL(zctxt_metadata);
        }
    }

    /* Now destroy the zinfo zval, because we're done with it */
    zval_dtor(zinfo);
    FREE_ZVAL(zinfo);

    LPCG(lpc_cache) = cache;
    cache->index    = hindex;

    sb              = sapi_get_stat(TSRMLS_C);
    cache->mtime    = sb->st_mtime;
    cache->filesize = sb->st_size;

    *max_module_len = max_len;
    return SUCCESS;

error:
    lpc_warning("Cannot create cache %s.  Falling back to default compile" TSRMLS_CC, r_cxt->cachedb_fullpath);
    return FAILURE;
}

/* }}} */

/* {{{ lpc_cache_destroy */
void lpc_cache_destroy(TSRMLS_D)
{ENTER(lpc_cache_destroy)
    lpc_cache_t  *cache = LPCG(lpc_cache);
    if (cache->index) {
        zend_hash_destroy(cache->index);
        efree(cache->index);
    }
        
    if (cache->db) cachedb_close(cache->db);

    efree(cache);
}
/* }}} */

/* {{{ lpc_cache_clear */
void lpc_cache_clear(TSRMLS_D)
{ENTER(lpc_cache_clear)
    lpc_cache_t  *cache = LPCG(lpc_cache);
    if (cache->db) cachedb_close2(cache->db, 'r');
    cache->db = NULL;
    remove(cache->context->cachedb_fullpath);
    LPCG(enabled) = 0;
}

/* }}} */

/* {{{ lpc_cache_insert */
void lpc_cache_insert(lpc_cache_key_t *key,  zend_uchar *compressed_buffer,
                      zend_uint compressed_length, zend_uint pool_length TSRMLS_DC)
{ENTER(lpc_cache_insert)
    lpc_cache_t  *cache;
    char         *zbuf;
    zval          buffer, *metadata, *index_entry;

    cache = LPCG(lpc_cache);

    INIT_ZVAL(buffer); ZVAL_STRINGL(&buffer, compressed_buffer, compressed_length, 0);

    /* Build the metadata zval for the pool buffer and add the pool buffer to the CacheDB */
    MAKE_STD_ZVAL(metadata);
    array_init_size(metadata, 3);
    add_next_index_long(metadata, key->mtime);
    add_next_index_long(metadata, key->filesize);
    add_next_index_long(metadata, (ulong) pool_length);

    CHECK(cachedb_add(cache->db, (char *) key->filename, 
                      key->filename_length, &buffer, metadata)==SUCCESS);

    /* No DTOR for the buffer zval as the compressed_buffer will be cleaned up by the pool DTOR */

    /* Update the cache index with the new entry */
    MAKE_STD_ZVAL(index_entry);
    array_init_size(index_entry, 4);
    add_next_index_long(index_entry, compressed_length);
    php_array_merge(Z_ARRVAL_P(index_entry), Z_ARRVAL_P(metadata), 0 TSRMLS_CC);

    if(Z_DELREF_P(metadata)==0) {
        zval_dtor(metadata);
        efree(metadata);
    }

    hash_add(cache->index, key->filename, index_entry);
    return;

error:
    lpc_error("Intrenal failure during insert of cache entry for %s" TSRMLS_CC, key->filename);
}
/* }}} */

/* {{{ lpc_cache_retrieve */
lpc_pool* 	lpc_cache_retrieve(lpc_cache_key_t *key, void **entry_rec TSRMLS_DC)
{ENTER(lpc_cache_retrieve)
    lpc_cache_t *cache = LPCG(lpc_cache);
    zval       **index_entry, **length, **pool_length, db_rec;
    zend_uchar  *buffer;
    zend_uint    compressed_length, uncompressed_length;

    if (hash_find(cache->index, key->filename, index_entry) == FAILURE) {
        return NULL;
    }
    CHECK(cachedb_find(cache->db, (char *) key->filename, key->filename_length, 0) == SUCCESS);

    hash_get_first_zv(Z_ARRVAL_PP(index_entry), length);
    hash_get_last_zv(Z_ARRVAL_PP(index_entry), pool_length);

    compressed_length   = Z_LVAL_PP(length);
    uncompressed_length = Z_LVAL_PP(pool_length);
   /*
    * cachedb_fetch() returns a string zval. It is layered over a php_stream_copy_to_mem() which 
    * peamllocs the return buffer, so this must be pefreed after decompression.  The temporary zval
    * db_rec is set up to accept the fetch.
    */
    lpc_pool_storage( uncompressed_length, compressed_length, &buffer TSRMLS_CC);
    INIT_ZVAL(db_rec); ZVAL_STRINGL(&db_rec, buffer, compressed_length, 0);

    CHECK(cachedb_fetch(cache->db, &db_rec) == SUCCESS &&
         Z_STRLEN(db_rec) == compressed_length);

    return lpc_pool_create(LPC_RO_SERIALPOOL, (void**) entry_rec TSRMLS_CC);

error:
    lpc_error("Internal failure during retrieval of cache entry for %s" TSRMLS_CC, key->filename);
    return NULL;
}
/* }}} */

/* {{{ lpc_cache_make_key */
lpc_cache_key_t* lpc_cache_make_key(zend_file_handle* handle, const char* include_path TSRMLS_DC)
{ENTER(lpc_cache_make_key)
    lpc_cache_key_t  *key             = ecalloc(1, sizeof(lpc_cache_key_t));
    char             *filename        = handle->filename;
    uint              filename_length;
    int               handle_filename = (handle->type == ZEND_HANDLE_FILENAME);
    char             *buf; 
    size_t            buf_length;
    zval            **index_entry = NULL, **recsize, **mtime, **filesize, **pool_length;
    int               mismatch, stat_file;

    if(filename==NULL) {
        return NULL;
    }

    filename_length = strlen(filename);

    /* Lookup to see if filename already exists in index */
    if (hash_find(LPCG(lpc_cache)->index, filename, index_entry) == SUCCESS) {

        hash_get_first_zv(Z_ARRVAL_PP(index_entry), recsize);
        hash_get_next_zv( Z_ARRVAL_PP(index_entry), mtime);
        hash_get_next_zv( Z_ARRVAL_PP(index_entry), filesize);
        hash_get_next_zv( Z_ARRVAL_PP(index_entry), pool_length);
    }

    /* Unlike APC, LPC uses a percentage for fpstat, so if 0 < fpstat < 100, a random number 
     * is generated to decide whether stat validation is bypassed.  The randomness doesn't 
     * need to be strong, so rand() plus the request time is good enough. */
    stat_file = (index_entry != NULL && LPCG(fpstat)>=0 && 
                 (((time_t) rand() + LPCG(sapi_request_time)) % 100) < LPCG(fpstat));

    if (index_entry != NULL && !stat_file) {

        /* Cache hits and it's OK to use the cache details */

        key->mtime           = Z_LVAL_PP(mtime);
        key->filesize        = Z_LVAL_PP(filesize);
        key->type            = LPC_CACHE_LOOKUP;
    } else { 
        struct stat *sb      = emalloc(sizeof*sb);
        
        /* If this is a cache miss or the stat value requires a file check then open the file
         * using the standard zend fixup utility.  If the fixup fails then  the simplest thing
         * to do is to pass back a nil response so the caller fails back to the standard zend 
         * compile module to let it deal with the error, because depending the type this might
         * be acceptable to the application.  */

        if ((zend_stream_fixup(handle, &buf, &buf_length TSRMLS_CC) == FAILURE) ||
            stat(filename, sb) != 0) {
            efree(key);
            efree(sb);
            return NULL;
            }

        if (stat_file) { 
            if ((Z_LVAL_PP(mtime) != sb->st_mtime) || (Z_LVAL_PP(filesize) != sb->st_size)) {
                /* There is a mismatch between the cached and on filesystem versions. Something is wrong !! */
                lpc_cache_clear(TSRMLS_C);
                efree(sb);
                efree(key);
                zend_llist_add_element(&CG(open_files), handle);
                return NULL;
            } else { 
                /* stat was consistent with the cache so close the file and use the cache version */
                zend_file_handle_dtor(handle TSRMLS_CC);
                key->mtime    = Z_LVAL_PP(mtime);
                key->filesize = Z_LVAL_PP(filesize);
                key->type     = LPC_CACHE_LOOKUP;
            }
        } else { /* its a new file to compile*/

            key->mtime     = sb->st_mtime;
            key->filesize  = sb->st_size;
            key->type      = index_entry ? LPC_CACHE_LOOKUP : LPC_CACHE_MISS;
            efree(sb);
        }
    }

    key->filename        = estrdup(filename);
    key->filename_length = strlen(filename);
    
        /* Ditto fail back to the standard zend compile module if the age of the file is less than
         * a fixed (e.g. 2sec) old.  Let the next request cache it :-) */
///////// TODO:     CHECK((LPCG(sapi_request_time) - key->mtime) > LPCG(file_update_protection));

    return key;

}
/* }}} */

/* {{{ lpc_cache_free_key */
void lpc_cache_free_key(lpc_cache_key_t* key TSRMLS_DC)
{ENTER(lpc_cache_free_key);
    efree(key->filename);
    if (key->fp) php_stream_close(key->fp);
    efree(key);
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
