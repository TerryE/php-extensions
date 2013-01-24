TEST Minimum - 1 function, no classes
<?php
function US($y) {
    return $y == 0 ? "America" : "Canada";
}

$x = "hello world";
echo "{$x} from Greece\n";
$y = US(0);
echo "$x from $y\n";

?>
===DONE===
