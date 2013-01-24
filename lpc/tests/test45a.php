TEST Catch Interfaces
<?php

function Error2Exception($errno, $errstr, $errfile, $errline)
{
	throw new Exception($errstr);
}

set_error_handler('Error2Exception');

try
{
	$con = fopen('/tmp/a_file_that_does_not_exist','r');
}
catch (Exception $e)
{
	echo "Exception: ", $e->getMessage(),"\n";
}

?>
===DONE=== 
