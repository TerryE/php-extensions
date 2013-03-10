Test of _toString
<?php
class Title {
	public function __toString() {
	    return "Hello World\n";
	}
}

$page = new Title;
echo (string) $page;
?>
===DONE===

