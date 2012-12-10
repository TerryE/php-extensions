*** Derived from PHP test script lang/038 ***
Convert warnings to exceptions
<?php

function Error2Exception($errno, $errstr, $errfile, $errline)
{
//if (is_numeric($errno))
    echo "\nError $errno caught:\n    error $errstr\n    at line $errline\n";
}
set_error_handler('Error2Exception', E_ERROR | E_PARSE | E_CORE_ERROR | E_COMPILE_ERROR );

$con = fopen("/tmp/a_file_that_does_not_exist",'r');
?>
===DONE===

 
