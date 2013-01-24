TEST one class with self-referencing constant -- generates error.
<?php
class X { const A=X::A;}
var_dump(X::A);
?>
===DONE===
