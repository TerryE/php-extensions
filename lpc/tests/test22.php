<?php
#one class with 1 static and one public function
class TestClass {
	private static $a = ' ';
	private $b;
	static function hwa() {
		$txt = self::$a;
    		echo "hello world $txt\n";
	}
	public function set_hw($x) {
    		$this->b=$x;
		self::$a = "from {$this->b}";
	}
}
function hw($x) {
    echo "hello world $x\n";
}
hw('from Greece');
$obj = new TestClass;
$obj->set_hw("America");
TestClass::hwa();
?>
===DONE===
