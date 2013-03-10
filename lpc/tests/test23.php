TEST One local function, one class with one static and one public function
<.inc
require "dd.inc";
function hw($x) {
    echo "hello world $x\n";
}
hw('from Greece');
$obj = new TestClass;
$obj->set_hw("America");
TestClass::hwa();
?>
===DONE===
