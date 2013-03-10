<?php
#included class with one static and one punblic function
class TestClass3 extends TestClass {
    public $x;
    public static function get() {
        return "The answer is 42.";
    }
}
