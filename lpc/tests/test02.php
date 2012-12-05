<?php
$x=1;
$y=TRUE;
$z= $y ? "true" :  "false";

echo strlen($z),"\n";

$zz = array ('a'=>1,'x'=>2,'b'=>3);
ksort($zz);
var_dump($zz);
echo "$_SERVER[SCRIPT_FILENAME]\n";
$o = new stdclass;
$o->fred=$zz;
echo $o->fred['x'],"\n";
?>
===DONE===
