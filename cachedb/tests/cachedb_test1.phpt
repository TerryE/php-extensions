--TEST--
CacheDB create test
--SKIPIF--
<?php extension_loaded('cachedb') or die('Info: cachedb not loaded'); ?>
--FILE--
<?php
	require_once(dirname(__FILE__) .'/print_info.inc');
	$dbname = dirname(__FILE__) .'/test.db';

	/* Create 1st DB */
	(($db = cachedb_open($dbname, 'c'))!==FALSE) || die("CacheDB: cannot create Db\n");
	cachedb_add("key1", "Content String 1", $db);
	cachedb_add("key2", "Content String 2", $db);
	cachedb_add("key3", array("Content String 3a", "Content String 3b"), $db);
	cachedb_add("key4", array("string"=>"Another Content String"), $db);
	cachedb_add("key5", NULL, $db);
	cachedb_close($db) || die("CacheDB: Error on DB close #1\n");

	/* Basic Access checks */
	(($db = cachedb_open($dbname, 'r'))!==FALSE) || die("CacheDB: Error reopening database\n");
	$info = cachedb_info( $db);
	(count($info[0]) ==5) || die("CacheDB: 1st DB record count != 5\n");
	cachedb_exists("key2") || die("CacheDB: key2 missing\n");
	cachedb_exists("key7") && die("CacheDB: key7 found\n");
	cachedb_add("key7", 123, $db) && die("CacheDB: cannot add to RO db\n");
	is_null(cachedb_fetch("key5")) || die("CacheDB: cannot fetch NULL value\n");
	cachedb_close($db) || die("CacheDB: Error on DB close #2\n");

	(cachedb_fetch(4) === FALSE) || die("CacheDB: Using invalid FP should return FALSE\n");

	/* R/W Access checks */
	(($db = cachedb_open($dbname, 'w'))!==FALSE) || die("CacheDB: Error reopening database R/W\n");
	cachedb_add("k8", array("String","XXX",11,23,4), $db) || die("CacheDB: add #1 existing db failed\n");
	cachedb_exists("key2") || die("CacheDB: exists key2 failed\n");
	cachedb_exists("k8") || die("CacheDB: exists k8 failed\n");
	serialize(cachedb_fetch("k8", $db)) == 'a:5:{i:0;s:6:"String";i:1;s:3:"XXX";i:2;i:11;i:3;i:23;i:4;i:4;}' || 
		die("CacheDB: fetch k8 failed");
	($k4 = cachedb_fetch("key4", $db)) !== FALSE || die("CacheDB: fetch key4 failed");
	(is_array($k4) && ($k4["string"]=="Another Content String")) || die("CacheDB: key4 value incorrect");
	cachedb_close($db) || die("CacheDB: Error on DB close #3\n");

	/* Update collision check */

	(($db1 = cachedb_open($dbname, 'w'))!==FALSE) || die("CacheDB: Error reopening database #1 R/W\n");
	(($db2 = cachedb_open($dbname, 'w'))!==FALSE) || die("CacheDB: Error reopening database #2 R/W\n");

	cachedb_add("keyX", "XXX", $db1) || die("Add keyX to DB #1 failed");
	cachedb_add("keyY", "YYY", $db2) || die("Add keyY to DB #2 failed");

	cachedb_close($db2) || die("CacheDB: Error on DB close #4.2\n");
	cachedb_close($db1) || die("CacheDB: Error on DB close #4.1\n");

	/* Note that the $db1 update should fail because the D/B has been updated already, 
     * so it should now contain keyY and NOT keyX
     */

	/* Print Index Report check */
	(($db1 = cachedb_open($dbname, 'r'))!==FALSE) || die("CacheDB: Error reopening database RO\n");
	print_info($db);
	/* leave close to RSHUTDOWN */
?>
===DONE===
--CLEAN--
<?php @unlink( $dirname(__FILE__) .'/test.db'); ?>
--EXPECT--

   NDX    ZLEN    LEN OFFSET KEY
     0     32     24    137 key1
     1     32     24    169 key2
     2     51     64    201 key3
     3     52     49    252 key4
     4     10      2    304 key5
     5     56     63    314 k8
     6     18     10    370 keyY
===DONE===
