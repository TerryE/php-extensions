*** Derived from PHP test script lang/bug28213 ***
Bug #28213 (crash in debug_print_backtrace in static methods)
<?php
class FooBar { static function error() { debug_print_backtrace(); } }
set_error_handler(array('FooBar', 'error'));
include('foobar.php');
?>
===DONE===
