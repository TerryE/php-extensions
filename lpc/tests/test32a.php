--TEST--
Ensure exceptions are handled properly when thrown in a statically declared __call.  
--FILE--
<?php
class A {
	function __call($strMethod, $arrArgs) {
		@var_dump($this);
		throw new Exception;
		echo "You should not see this";
	}
}
$a = new A();

echo "---> Invoke __call via simple method call.\n";
try {
	$a->unknown();
} catch (Exception $e) {
	echo "Exception caught OK; continuing.\n";
}
?>
==DONE==
