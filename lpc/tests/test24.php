TEST one required class
<?php
require "dd1.php";
class X {
    static function US($y) {
        return $y == 0 ? "America" : "Canada";
    }  
}
$x = "hello world";
echo "{$x} from Greece\n";
$y = X::US(0);
echo "$x from $y\n";
$obj = new TestClass;
$obj->set_hw("America");
TestClass::hwa();

?>
===DONE===
