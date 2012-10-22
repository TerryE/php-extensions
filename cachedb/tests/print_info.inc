<?php
function print_info($db) {
	list( $list, $hash ) = cachedb_info($db);
	reset( $hash );
	echo "\n   NDX    ZLEN    LEN OFFSET KEY                  METADATA\n";
	while( list( $key, $entry ) = each( $hash )) {
		list( $i, $offset ) = $entry;
		$entry = $list[$i];
		list( $key2, $zlen, $len ) = $entry;
		if( $key != $key2 ) {
			$key &= " (mismatch - $key2)";
		}
		$meta = (count($entry) == 4) ? serialize($entry[3]) : "";
		printf( "%6d %6d %6d %6d %-20.20s %s\n", $i, $zlen, $len, $offset, $key, $meta);
	}
}

