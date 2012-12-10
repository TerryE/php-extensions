*** Derived from PHP test script lang/039 ***
Generate Debug reloc_bvec error
<?php
class MyException extends Exception {}

function Error2Exception($errno, $errstr, $errfile, $errline) {
//	throw new MyException($errstr, $errno, $errfile, $errline);
}

try {}
catch (Exception $e) { echo "Exception"; }

?>
===DONE=== 
