<?php
$x=1;
$y=TRUE;
$z= $y ? "true" :  "false";

echo strlen($z),"\n";
echo strlen($GLOBALS['z']),"\n";

$zz = array ('a'=>1,'x'=>2,'b'=>3);
eval("\$GLOBALS['test']=\$zz;");
echo count($test), "\n";
?>
===DONE===

