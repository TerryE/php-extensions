TEST Exercise compile-time vs runtime binding of class inheritance
<?php

# Bound at compile time
class X { public $a = array('bar1', 'bar2'); const ZZ='Parent is X';}
class Y extends X {public $d = self::ZZ; }
echo "Class ", print_r(new Y, true);

# Bound at runtime
if (isset($argv[1])) {
    class A { public $a = array('foo1', 'foo2'), $b = 12; const ZZ='Parent is A(1)'; }
} else {
    class A { public $a = array('bar1', 'bar2'); const ZZ='Parent is A(2)';}
}
class B extends A {public $d = self::ZZ; }
echo "Class ", print_r(new B, true);

#Bound at runtime by inclusion
require( dirname(__FILE__)."/test15-classC.inc" );
class D extends C {public $d = self::ZZ; }
echo "Class ", print_r(new D, true);
require( dirname(__FILE__)."/test15-classE.inc" );
?>
===DONE===
