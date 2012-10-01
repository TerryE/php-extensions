#ifndef PHP_CACHEDB_H
#define PHP_CACHEDB_H
#ifndef CACHEDB_H
# include "cachedb.h"
#endif
#include "php.h"

extern zend_module_entry cachedb_module_entry;
#define phpext_cachedb_ptr &cachedb_module_entry

#endif /* PHP_CACHEDB_H */
