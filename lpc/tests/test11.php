TEST Minimum - 2 functions -- one late binding, no classes
<?php
if(1) {
    function US($y) {
        return $y == 0 ? "America" : "Canada";
    }  
}
function UK(&$x) {
		$x = "England";
		}
$x = "hello world";
echo "{$x} from Greece\n";
$y = US(0);
UK($z);
echo "$x from $y and from $z\n";
?>
===DONE===
