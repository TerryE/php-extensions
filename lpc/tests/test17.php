TEST Exercise different require paths
<?php
class x {const FILE = "./test15-classC.inc";};
$a =  dirname(__FILE__)."/dd.inc"; 
$notset=true;
require_once("$a" );
require_once("./dd1.inc" );
require_once(dirname(__FILE__)."/dd1.inc" );
require_once(x::FILE);
?>
===DONE===
