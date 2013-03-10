<?php
   $dir = dirname(__FILE__);
   echo file_exists( "$dir/dd.inc" ), "\n";
   $fh = fopen("$dir/dd1.inc", "r") );
   fclose($fh);
   require_once("$dir/dd2.inc");
?>
===DONE===
