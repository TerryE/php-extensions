TEST Exercise compile-time vs runtime binding of class inheritance
<?php

# Bound at compile time
class Y extends X {public $d = self::ZZ; }
class X { public $a = array('bar1', 'bar2'); const ZZ='Parent is X';}
if (isset($argv[1])) {
    echo "Class ", print_r(new Y, true);
}
echo "Class ", print_r(new Y, true);
?>
===DONE===
