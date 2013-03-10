TEST simple compile-time class inheritance  
<?php
if(1) {
class A {

    const XX = 'from A';
    const YY = 'from A';
    static $a;
/*

    static public $b;
    static protected $c;
*/
    static protected function funA($i) { return "funA says $i ";}
    static function funB($i) { return "funB says $i ";}

    public function funC($i) { return "funC says $i ";}
    public function funD($i) { return "funD says $i ";}

    protected function funE($i) { return "funE says $i ";}
    protected function funF($i) { return "funF says $i ";}
    protected function funG($i) { return "funG says $i ";}
    protected function funH($i) { return "funH says $i ";}

    private function funI($i) { return "funI says $i ";}
    private function funJ($i) { return "funJ says $i ";}

}
}
class B extends A {

    static $b;
    const YY = 'from B';
    const ZZ = 'from B';
    static function funB($i) { return "funB shouts $i ";}
    static function funB1($i) { return "funC shouts $i ";}

    public function funD($i) { return "funD shouts $i ";}
    public function funD1($i) { return "funE shouts $i ";}

    protected function funF($i) { return "funF shouts $i ". B::YY . "\n";}
    public function funG($i) { return "funG shouts $i and ". $this->funF($i);}

    public function funJ($i) { return "funG says $i ";}
    private function funJ1($i) { return "funH says $i ";}

}
$b = new B();

echo A::funB("1"),B::funB("1"),B::funB1("1"),"\n";
echo $b->funC("1"),$b->funD("1"),$b->funG("1"),$b->funG("1"),"\n";
print_r(get_class_methods ( $b ))
/*
echo "\n\n---> Invoke __call via callback.\n";
try {
	call_user_func(array($b, 'unknownCallback'), 1,2,3);
} catch (Exception $e) {
	echo "Exception caught OK; continuing.\n";
}
*/
?>
===DONE===
