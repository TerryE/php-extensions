--TEST--
Bug #23951 (Defines not working in inherited classes)
--FILE--
<?php
class A {
    public $a_var = array('foo1_value', 'foo2_value');
}
class B extends A {}
?>
===DONE===
