<?php
#included class with one static and one punblic function
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
