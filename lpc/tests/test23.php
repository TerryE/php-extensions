<?php
# one local function
#included class with one static and one punblic function
require "dd.php";
function hw($x) {
    echo "hello world $x\n";
}
hw('from Greece');
$obj = new TestClass;
$obj->set_hw("America");
TestClass::hwa();
?>
===DONE===