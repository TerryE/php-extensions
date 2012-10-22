--TEST--
CacheDB create test
--SKIPIF--
<?php extension_loaded('cachedb') or die('Info: cachedb not loaded'); ?>
--FILE--
<?php
	require_once(dirname(__FILE__) .'/print_info.inc');
	$dbname = dirname(__FILE__) .'/test.db';

	/* Create 1st DB */
	(($db = cachedb_open($dbname, 'cb'))!==FALSE) || die("CacheDB: cannot create Db\n");
	cachedb_add("key1", "Content String 1");
	cachedb_add("key2", "Content String 2");
	cachedb_close($db) || die("CacheDB: Error on DB close #1\n");

	/* Basic Access checks */
	(($db = cachedb_open($dbname, 'rb'))!==FALSE) || die("CacheDB: Error reopening database\n");
	$info = cachedb_info( $db);
	(count($info[0]) == 2) || die("CacheDB: 1st DB record count != 5\n");
    cachedb_exists("key2") || die("CacheDB: key2 missing\n");
	cachedb_exists("key7") && die("CacheDB: key7 found\n");
	cachedb_add("key7", "123") && die("CacheDB: cannot add to RO db\n");
	cachedb_close($db) || die("CacheDB: Error on DB close #2\n");

	(@cachedb_fetch(4) === FALSE) || die("CacheDB: Using invalid FP should return FALSE\n");

	/* R/W Access checks */
	(($db = cachedb_open($dbname, 'wb'))!==FALSE) || die("CacheDB: Error reopening database R/W\n");
	cachedb_add("kmeta", "Another String", $db, array('name'=>'fred', 'version' => 32, )) ||
		die("CacheDB: Adding key with metadata failed\n");
	cachedb_exists("key2") || die("CacheDB: exists key2 failed\n");
	cachedb_exists("kmeta") || die("CacheDB: exists kmeta failed\n");
	cachedb_exists("kmeta", $db, $meta) &&
    	serialize($meta) == 'a:2:{s:4:"name";s:4:"fred";s:7:"version";i:32;}' ||
		die("CacheDB: fetch kmeta with metadata failed");
	($k2 = cachedb_fetch("key2")) !== FALSE || die("CacheDB: fetch key2 failed");
	is_string($k2) && $k2=="Content String 2" || die("CacheDB: key2 value incorrect");
	cachedb_close($db) || die("CacheDB: Error on DB close #3\n");

	/* Update collision check */

	(($db1 = cachedb_open($dbname, 'w'))!==FALSE) || die("CacheDB: Error reopening database #1 R/W\n");
	(($db2 = cachedb_open($dbname, 'w'))!==FALSE) || die("CacheDB: Error reopening database #2 R/W\n");

	cachedb_add("keyX", "XXX", $db1) || die("Add keyX to DB #1 failed");
	cachedb_add("keyY", "YYY", $db2) || die("Add keyY to DB #2 failed");
    cachedb_exists("k8", $db, $meta) && var_export($meta);
	
	cachedb_close($db2) || die("CacheDB: Error on DB close #4.2\n");
	cachedb_close($db1) || die("CacheDB: Error on DB close #4.1\n");

	/* Note that the $db1 update should fail because the D/B has been updated already, 
     * so it should now contain keyY and NOT keyX
     */

	/* Print Index Report check */
	(($db1 = cachedb_open($dbname, 'r'))!==FALSE) || die("CacheDB: Error reopening database RO\n");
	print_info($db);
	/* leave close to RSHUTDOWN */?>
===DONE===
--CLEAN--
<?php @unlink( $dirname(__FILE__) .'/test.db'); ?>
--EXPECT--
NDX    ZLEN    LEN OFFSET KEY                  METADATA
     0     32     24    145 key1                 
     1     32     24    177 key2                 
     2     30     22    209 kmeta                a:2:{s:4:"name";s:4:"fred";s:7:"version";i:32;}
     3     18     10    239 keyY                 
===DONE===
