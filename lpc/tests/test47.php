TEST debug_print_backtrace in static methods
<?php
class FooBar { static function error() { debug_print_backtrace(); } }
set_error_handler(array('FooBar', 'error'));
include('foobar.inc');
?>
===DONE===
