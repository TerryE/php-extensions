TEST class constants and inheritance
<?php
  class C
  {
      const X = D::A;
      public static $a = array(K => D::A, D::A => K, self::X =>0);
  }
  
  class D extends C
  {
      const A = "hello";
  }
  
//  define('K', "nasty");
  
  var_dump(C::X, C::$a, D::X, D::$a, D::X, D::$a);
?>
===DONE===
