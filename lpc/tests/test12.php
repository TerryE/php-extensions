<?php
# Minimum - 2 functions -- one late binding, no classes
class X { const A=X::A;}
var_dump(X::A);
?>
===DONE===
