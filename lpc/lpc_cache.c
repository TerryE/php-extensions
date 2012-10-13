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
#include "lpc_zend.h"
#include "SAPI.h"
#include "TSRM.h"
#include "ext/standard/md5.h"

/* TODO: rehash when load factor exceeds threshold */

#define CHECK(p) { if ((p) == NULL) return NULL; }

/* {{{ key_equals */
#define key_equals(a, b) (a.inode==b.inode && a.device==b.device)
/* }}} */

/* {{{ hash */
static unsigned int hash(lpc_cache_key_t key)
{ENTER(hash)
    return (unsigned int)(key.data.file.device + key.data.file.inode);
}
/* }}} */

/* {{{ string_nhash_8 */
#define string_nhash_8(s,len) (unsigned int)(zend_inline_hash_func(s, len))
/* }}} */

/* {{{ make_prime */
static int const primes[] = {
  257, /*   256 */
  521, /*   512 */
 1031, /*  1024 */
 2053, /*  2048 */
 3079, /*  3072 */
 4099, /*  4096 */
 5147, /*  5120 */
 6151, /*  6144 */
 7177, /*  7168 */
 8209, /*  8192 */
 9221, /*  9216 */
10243, /* 10240 */
0      /* sentinel */
};

static int make_prime(int n)
{ENTER(make_prime)
    int *k = (int*)primes; 
    while(*k) {
        if((*k) > n) return *k;
        k++;
    }
    return *(k-1);
}
/* }}} */

/* {{{ make_slot */
slot_t* make_slot(lpc_cache_key_t key, lpc_cache_entry_t* value, slot_t* next, time_t t TSRMLS_DC)
{ENTER(make_slot)
    slot_t* p = lpc_pool_alloc(value->pool, sizeof(slot_t));

    if (!p) return NULL;

	if(key.type == LPC_CACHE_KEY_FPFILE) {
        char *fullpath = (char*) lpc_pstrdup(key.data.fpfile.fullpath, value->pool TSRMLS_CC);
        if (!fullpath) {
            return NULL;
        }
        key.data.fpfile.fullpath = fullpath;
    }
    p->key = key;
    p->value = value;
    p->next = next;
    p->creation_time = t;
    p->access_time = t;
    p->deletion_time = 0;
    return p;
}
/* }}} */

/* {{{ free_slot */
static void free_slot(slot_t* slot TSRMLS_DC)
{ENTER(free_slot)
    lpc_pool_destroy(slot->value->pool);
}
/* }}} */

/* {{{ remove_slot */
static void remove_slot(lpc_cache_t* cache, slot_t** slot TSRMLS_DC)
{ENTER(remove_slot)
    slot_t* dead = *slot;
    *slot = (*slot)->next;

    cache->header->mem_size -= dead->value->mem_size;
    cache->header->num_entries--;
    if (dead->value->ref_count <= 0) {
        free_slot(dead TSRMLS_CC);
    }
    else {
        dead->next = cache->header->deleted_list;
        dead->deletion_time = time(0);
        cache->header->deleted_list = dead;
    }
}
/* }}} */

/* {{{ lpc_cache_create */
lpc_cache_t* lpc_cache_create(TSRMLS_D)
{ENTER(lpc_cache_create)
int size_hint;
    lpc_cache_t *cache;
    int num_slots;

    num_slots = make_prime(size_hint > 0 ? size_hint : 2000);

    cache = (lpc_cache_t*) emalloc(sizeof(lpc_cache_t));

    cache->addr = ecalloc(1, sizeof(cache_header_t) + num_slots*sizeof(slot_t*));

    cache->header = (cache_header_t*) cache->addr;
    cache->header->deleted_list = NULL;
    cache->header->start_time = time(NULL);
 
    cache->slots = (slot_t**) (((char*) cache->addr) + sizeof(cache_header_t));
    cache->num_slots = num_slots;

    return cache;
}
/* }}} */

/* {{{ lpc_cache_destroy */
void lpc_cache_destroy(lpc_cache_t *cache TSRMLS_DC)
{ENTER(lpc_cache_destroy)
	if (cache->addr != NULL) {
		efree(cache->addr);
	}
    efree(cache);
}
/* }}} */

/* {{{ lpc_cache_clear */
void lpc_cache_clear(lpc_cache_t* cache TSRMLS_DC)
{ENTER(lpc_cache_clear)
    int i;

    if(!cache) return;

    cache->header->start_time = time(NULL);
 
    for (i = 0; i < cache->num_slots; i++) {
        slot_t* p = cache->slots[i];
        while (p) {
            remove_slot(cache, &p TSRMLS_CC);
        }
        cache->slots[i] = NULL;
    }

    memset(&cache->header->lastkey, 0, sizeof(lpc_keyid_t));
}
/* }}} */

/* {{{ lpc_cache_insert */
int lpc_cache_insert(lpc_cache_t* cache, lpc_cache_key_t key, lpc_cache_entry_t* value, lpc_context_t *ctxt, time_t t TSRMLS_DC)
{ENTER(lpc_cache_insert)
    int rval;
    slot_t** slot;

    if (!value) {
        return 0;
    }

    lpc_debug("Inserting [%s]" TSRMLS_CC, value->data.file.filename);

    if(key.type == LPC_CACHE_KEY_FILE) slot = &cache->slots[hash(key) % cache->num_slots];
    else slot = &cache->slots[string_nhash_8(key.data.fpfile.fullpath, key.data.fpfile.fullpath_len) % cache->num_slots];

    while(*slot) {
      if(key.type == (*slot)->key.type) {
        if(key.type == LPC_CACHE_KEY_FILE) {
            if(key_equals((*slot)->key.data.file, key.data.file)) {
                /* If existing slot for the same device+inode is different, remove it and insert the new version */
                if (ctxt->force_update || (*slot)->key.mtime != key.mtime) {
                    remove_slot(cache, slot TSRMLS_CC);
                    break;
                }
                return 0;
            }
        } else {   /* LPC_CACHE_KEY_FPFILE */
            if(!memcmp((*slot)->key.data.fpfile.fullpath, key.data.fpfile.fullpath, key.data.fpfile.fullpath_len+1)) {
                /* Hrm.. it's already here, remove it and insert new one */
                remove_slot(cache, slot TSRMLS_CC);
                break;
            }
        }
      }
      slot = &(*slot)->next;
    }

    if ((*slot = make_slot(key, value, *slot, t TSRMLS_CC)) == NULL) {
        return -1;
    }

    value->mem_size = ctxt->pool->size;
    cache->header->mem_size += ctxt->pool->size;
    cache->header->num_entries++;

    return 1;
}
/* }}} */

/* {{{ lpc_cache_find_slot */
static inline slot_t* lpc_cache_find_slot(lpc_cache_t* cache, lpc_cache_key_t key, time_t t TSRMLS_DC)
{ENTER(lpc_cache_find_slot)
    slot_t** slot;
    volatile slot_t* retval = NULL;

    if(key.type == LPC_CACHE_KEY_FILE) slot = &cache->slots[hash(key) % cache->num_slots];
    else slot = &cache->slots[string_nhash_8(key.data.fpfile.fullpath, key.data.fpfile.fullpath_len) % cache->num_slots];

    while (*slot) {
      if(key.type == (*slot)->key.type) {
        if(key.type == LPC_CACHE_KEY_FILE) {
            if(key_equals((*slot)->key.data.file, key.data.file)) {
                if((*slot)->key.mtime != key.mtime) {
                    remove_slot(cache, slot TSRMLS_CC);
                    return NULL;
                }
                (*slot)->access_time = t;
                retval = *slot;
                 return (slot_t*)retval;
            }
        } else {  /* LPC_CACHE_KEY_FPFILE */
            if(!memcmp((*slot)->key.data.fpfile.fullpath, key.data.fpfile.fullpath, key.data.fpfile.fullpath_len+1)) {
                /* TTL Check ? */
                (*slot)->access_time = t;
                retval = *slot;
                return (slot_t*)retval;
            }
        }
      }
      slot = &(*slot)->next;
    }
     return NULL;
}
/* }}} */

/* {{{ lpc_cache_find */
lpc_cache_entry_t* lpc_cache_find(lpc_cache_t* cache, lpc_cache_key_t key, time_t t TSRMLS_DC)
{ENTER(lpc_cache_find)
    slot_t * slot = lpc_cache_find_slot(cache, key, t TSRMLS_CC);
    return (slot) ? slot->value : NULL;
}
/* }}} */

/* {{{ lpc_cache_release */
void lpc_cache_release(lpc_cache_t* cache, lpc_cache_entry_t* entry TSRMLS_DC)
{ENTER(lpc_cache_release)
	entry->ref_count--;
}
/* }}} */

/* {{{ lpc_cache_make_file_key */
int lpc_cache_make_file_key(lpc_cache_key_t* key,
                       const char* filename,
                       const char* include_path,
                       time_t t
                       TSRMLS_DC)
{ENTER(lpc_cache_make_file_key)
    struct stat *tmp_buf=NULL;
    struct lpc_fileinfo_t *fileinfo = NULL;
    int len;

    assert(key != NULL);

    if (!filename || !SG(request_info).path_translated) {
        lpc_debug("No filename and no path_translated - bailing" TSRMLS_CC);
        goto cleanup;
    }

    len = strlen(filename);
    if(LPCG(fpstat)==0) {
        if(IS_ABSOLUTE_PATH(filename,len)) {
            key->data.fpfile.fullpath = filename;
            key->data.fpfile.fullpath_len = len;
            key->mtime = t;
            key->type = LPC_CACHE_KEY_FPFILE;
            goto success;
        } else if(LPCG(canonicalize)) {

            fileinfo = lpc_php_malloc(sizeof(lpc_fileinfo_t) TSRMLS_CC);

            if (lpc_search_paths(filename, include_path, fileinfo TSRMLS_CC) != 0) {
                lpc_warning("lpc failed to locate %s - bailing" TSRMLS_CC, filename);
                goto cleanup;
            }

            if(!VCWD_REALPATH(fileinfo->fullpath, LPCG(canon_path))) {
                lpc_warning("realpath failed to canonicalize %s - bailing" TSRMLS_CC, filename);
                goto cleanup;
            }

            key->data.fpfile.fullpath = LPCG(canon_path);
            key->data.fpfile.fullpath_len = strlen(LPCG(canon_path));
            key->mtime = t;
            key->type = LPC_CACHE_KEY_FPFILE;
            goto success;
        }
        /* fall through to stat mode */
    }

    fileinfo = lpc_php_malloc(sizeof(lpc_fileinfo_t) TSRMLS_CC);

    assert(fileinfo != NULL);

    if(!strcmp(SG(request_info).path_translated, filename)) {
        tmp_buf = sapi_get_stat(TSRMLS_C);  /* Apache has already done this stat() for us */
    }

    if(tmp_buf) {
        fileinfo->st_buf.sb = *tmp_buf;
    } else {
        if (lpc_search_paths(filename, include_path, fileinfo TSRMLS_CC) != 0) {
            lpc_debug("Stat failed %s - bailing (%s) (%d)" TSRMLS_CC, filename,SG(request_info).path_translated);
            goto cleanup;
        }
    }

    if(LPCG(max_file_size) < fileinfo->st_buf.sb.st_size) {
        lpc_debug("File is too big %s (%d - %ld) - bailing" TSRMLS_CC, filename,t,fileinfo->st_buf.sb.st_size);
        goto cleanup;
    }

    /*
     * This is a bit of a hack.
     *
     * Here I am checking to see if the file is at least 2 seconds old.  
     * The idea is that if the file is currently being written to then its
     * mtime is going to match or at most be 1 second off of the current
     * request time and we want to avoid caching files that have not been
     * completely written.  Of course, people should be using atomic 
     * mechanisms to push files onto live web servers, but adding this
     * tiny safety is easier than educating the world.  This is now
     * configurable, but the default is still 2 seconds.
     */
    if(LPCG(file_update_protection) && (t - fileinfo->st_buf.sb.st_mtime < LPCG(file_update_protection)) && !LPCG(force_file_update)) {
        lpc_debug("File is too new %s (%d - %d) - bailing" TSRMLS_CC,filename,t,fileinfo->st_buf.sb.st_mtime);
        goto cleanup;
    }

    key->data.file.device = fileinfo->st_buf.sb.st_dev;
    key->data.file.inode  = fileinfo->st_buf.sb.st_ino;

    /*
     * If working with content management systems that like to munge the mtime, 
     * it might be appropriate to key off of the ctime to be immune to systems
     * that try to backdate a template.  If the mtime is set to something older
     * than the previous mtime of a template we will obviously never see this
     * "older" template.  At some point the Smarty templating system did this.
     * I generally disagree with using the ctime here because you lose the 
     * ability to warm up new content by saving it to a temporary file, hitting
     * it once to cache it and then renaming it into its permanent location so
     * set the lpc.stat_ctime=true to enable this check.
     */
    if(LPCG(stat_ctime)) {
        key->mtime  = (fileinfo->st_buf.sb.st_ctime > fileinfo->st_buf.sb.st_mtime) ? fileinfo->st_buf.sb.st_ctime : fileinfo->st_buf.sb.st_mtime; 
    } else {
        key->mtime = fileinfo->st_buf.sb.st_mtime;
    }
    key->type = LPC_CACHE_KEY_FILE;

success: 

    if(fileinfo != NULL) {
        lpc_php_free(fileinfo TSRMLS_CC);
    }

    return 1;

cleanup:
    
    if(fileinfo != NULL) {
        lpc_php_free(fileinfo TSRMLS_CC);
    }

    return 0;
}
/* }}} */

/* {{{ lpc_cache_make_file_entry */
lpc_cache_entry_t* lpc_cache_make_file_entry(const char* filename,
                                        zend_op_array* op_array,
                                        lpc_function_t* functions,
                                        lpc_class_t* classes,
                                        lpc_context_t* ctxt
                                        TSRMLS_DC)
{ENTER(lpc_cache_make_file_entry)
    lpc_cache_entry_t* entry;
    lpc_pool* pool = ctxt->pool;

    entry = (lpc_cache_entry_t*) lpc_pool_alloc(pool, sizeof(lpc_cache_entry_t));
    if (!entry) return NULL;

    entry->data.file.filename  = lpc_pstrdup(filename, pool TSRMLS_CC);
    if(!entry->data.file.filename) {
        lpc_debug("lpc_cache_make_file_entry: entry->data.file.filename is NULL - bailing" TSRMLS_CC);
        return NULL;
    }
    lpc_debug("lpc_cache_make_file_entry: entry->data.file.filename is [%s]" TSRMLS_CC,entry->data.file.filename);
    entry->data.file.op_array  = op_array;
    entry->data.file.functions = functions;
    entry->data.file.classes   = classes;

    entry->data.file.halt_offset = lpc_file_halt_offset(filename TSRMLS_CC);

    entry->type = LPC_CACHE_ENTRY_FILE;
    entry->ref_count = 0;
    entry->mem_size = 0;
    entry->pool = pool;
    return entry;
}
/* }}} */

/* {{{ */
static zval* lpc_cache_link_info(lpc_cache_t *cache, slot_t* p TSRMLS_DC)
{ENTER(lpc_cache_link_info)
    zval *link = NULL;
    char md5str[33];

    ALLOC_INIT_ZVAL(link);

    if(!link) {
        return NULL;
    }

    array_init(link);

    if(p->value->type == LPC_CACHE_ENTRY_FILE) {
        add_assoc_string(link, "type", "file", 1);
        if(p->key.type == LPC_CACHE_KEY_FILE) {

            #ifdef PHP_WIN32
            {
            char buf[20];
            sprintf(buf, "%I64d",  p->key.data.file.device);
            add_assoc_string(link, "device", buf, 1);

            sprintf(buf, "%I64d",  p->key.data.file.inode);
            add_assoc_string(link, "inode", buf, 1);
            }
            #else
            add_assoc_long(link, "device", p->key.data.file.device);
            add_assoc_long(link, "inode", p->key.data.file.inode);
            #endif

            add_assoc_string(link, "filename", p->value->data.file.filename, 1);
        } else { /* This is a no-stat fullpath file entry */
            add_assoc_long(link, "device", 0);
            add_assoc_long(link, "inode", 0);
            add_assoc_string(link, "filename", (char*)p->key.data.fpfile.fullpath, 1);
        }
        if (LPCG(file_md5)) {
               make_digest(md5str, p->key.md5);
               add_assoc_string(link, "md5", md5str, 1);
        } 
    }

    add_assoc_long(link, "mtime", p->key.mtime);
    add_assoc_long(link, "creation_time", p->creation_time);
    add_assoc_long(link, "deletion_time", p->deletion_time);
    add_assoc_long(link, "access_time", p->access_time);
    add_assoc_long(link, "ref_count", p->value->ref_count);
    add_assoc_long(link, "mem_size", p->value->mem_size);

    return link;
}
/* }}} */

/* {{{ lpc_cache_info */
zval* lpc_cache_info(lpc_cache_t* cache, zend_bool limited TSRMLS_DC)
{ENTER(lpc_cache_info)
    zval *info = NULL;
    zval *list = NULL;
    zval *deleted_list = NULL;
    zval *slots = NULL;
    slot_t* p;
    int i, j;

    if(!cache) return NULL;

    ALLOC_INIT_ZVAL(info);

    if(!info) {
        return NULL;
    }

    array_init(info);
    add_assoc_long(info, "num_slots", cache->num_slots);
    add_assoc_long(info, "num_entries", cache->header->num_entries);

    if(!limited) {
        /* For each hashtable slot */
        ALLOC_INIT_ZVAL(list);
        array_init(list);

        ALLOC_INIT_ZVAL(slots);
        array_init(slots);

        for (i = 0; i < cache->num_slots; i++) {
            p = cache->slots[i];
            j = 0;
            for (; p != NULL; p = p->next) {
                zval *link = lpc_cache_link_info(cache, p TSRMLS_CC);
                add_next_index_zval(list, link);
                j++;
            }
            add_next_index_long(slots, j);
        }

        /* For each slot pending deletion */
        ALLOC_INIT_ZVAL(deleted_list);
        array_init(deleted_list);

        for (p = cache->header->deleted_list; p != NULL; p = p->next) {
            zval *link = lpc_cache_link_info(cache, p TSRMLS_CC);
            add_next_index_zval(deleted_list, link);
        }
        
        add_assoc_zval(info, "cache_list", list);
        add_assoc_zval(info, "deleted_list", deleted_list);
        add_assoc_zval(info, "slot_distribution", slots);
    }

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
