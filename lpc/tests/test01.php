<?php
$x=1;
$y=TRUE;
$z= $y ? "true" :  "false";

if ($y) {echo "value is $z and $x\n";} else {echo "oops";}
$zz = array (1,2,3);
foreach ($zz as $i) echo "i = $i\n";
$ind_zz = "zz";
var_dump($$ind_zz);
?>
===DONE===
