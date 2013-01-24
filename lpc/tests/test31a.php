TEST Minimal inherited class
<?php
#if (1) { 
  class A { static $a_var = 'foo'; }
#   }
class B extends A {}
?>
===DONE===
