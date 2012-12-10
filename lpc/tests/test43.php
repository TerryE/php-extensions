*** Derived from PHP test script func/010 ***
function with many parameters
<?php
@unlink(dirname(__FILE__).'/010-file.php');
// the stack size + some random constant
$boundary = 64*1024;
$limit    = $boundary+42;
exit;             ///////////////////// Temp 'cos this is a dog to run

function test($a,$b)
{
	var_dump($a === $b);
	test2($a,$b);
}

function test2($a, $b)
{
	if ($a !== $b) {
		var_dump("something went wrong: $a !== $b");
	}
}


// generate the function
$str = "<?php\nfunction x(";

for($i=0; $i < $limit; ++$i) {
	$str .= '$v'.dechex($i).($i===($limit-1) ? '' : ',');
}

$str .= ') {
	test($v42, \'42\');
	test(\'4000\', $v4000);
	test2($v300, \'300\');
	test($v0, \'0\'); // first
	test($v'.dechex($limit-1).", '".dechex($limit-1).'\'); // last
	test($v'.dechex($boundary).", '".dechex($boundary).'\'); //boundary
	test($v'.dechex($boundary+1).", '".dechex($boundary+1).'\'); //boundary+1
	test($v'.dechex($boundary-1).", '".dechex($boundary-1).'\'); //boundary-1
}';

// generate the function call
$str .= "\n\nx(";

for($i=0; $i< $limit; ++$i) {
	$str .= "'".dechex($i)."'".($i===($limit-1) ? '' : ',');
}

$str .= ");\n";

$filename = dirname(__FILE__).'/010-file.php';
file_put_contents(dirname(__FILE__).'/010-file.php', $str);
unset($str);

include($filename);

echo "Done\n";

?>
===DONE===
