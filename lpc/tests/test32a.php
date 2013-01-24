TEST Garbage collection on compile-time bound inheritance of a static function  
<?php
if (1) { class A { static function fred() {} } }
class B extends A { }
?>
==DONE==
