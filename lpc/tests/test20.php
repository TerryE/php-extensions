TEST Single external builtin class
<?php
$obj = new stdClass;

$obj->fred = 23;

echo "fred=",$obj->fred,"\n";
?>
===DONE===
