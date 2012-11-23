<?php
# Minimum - 1 function, no classes
function US($y) {
    return $y == 0 ? "America" : "Canada";
}

$x = "hello world";
echo "{$x} from Greece\n";
$y = US(0);
echo "$x from $y\n";

?>
===DONE===
