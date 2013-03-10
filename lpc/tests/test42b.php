TEST Error handling with classes and __tostring
<?php
function test_error_handler($err_no, $err_msg, $filename, $linenum, $vars) {
        echo "Error: $err_no - $err_msg\n";
}
set_error_handler('test_error_handler');
error_reporting(8191);

class badToString { function __toString() { return 0; }	}
$obj =  new badToString;
printf ($obj);
?>
===DONE===
