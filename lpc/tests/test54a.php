*** Derived from PHP test script lang/include_variation2 ***
Including a file in the current script directory from an included function
<?php
$fred=100;
echo "Include path = ", get_include_path(), "\n";
echo "Included counter =", include('include_files/counter.inc'), "\n";
$newPath=get_include_path() . PATH_SEPARATOR . dirname(__FILE__) . '/include_files';
set_include_path($newPath);
echo "New included path = ", get_include_path(), "\n";
echo "Included counter from includ_path =", include('include_files/counter.inc'), "\n";

/*
$fred = 100;
echo "\n100=", include 'include_files/counter.inc';
echo "\n101=", include './include_files/counter.inc';
echo "\n102=", include './' . 'include_files/counter.inc';
echo "\n103=", include dirname(__FILE__) . '/include_files/counter.inc';
$newPath=get_include_path() . PATH_SEPARATOR . dirname(__FILE__) . '/include_files';
echo "\nNew Include Path = $newPath";
set_include_path($newPath);
echo "\n104=", include 'counter.inc';
echo "\n105=", include array_pop(explode(PATH_SEPARATOR, get_include_path())).'/counter.inc';

$fred = 200;
echo "\n1=", include_once 'include_files/counter.inc';
echo "\n1=", include_once './include_files/counter.inc';
echo "\n1=", include_once './' . 'include_files/counter.inc';
echo "\n1=", include_once dirname(__FILE__) . '/include_files/counter.inc';
echo "\n1=", include_once 'counter.inc';
echo "\n1=", include_once array_pop(explode(PATH_SEPARATOR, get_include_path())).'/counter.inc';

$fred = 300;
echo "\n1=", include_once 'include_files/counter.inc';
echo "\n1=", include_once './include_files/counter.inc';
echo "\n1=", include_once './' . 'include_files/counter.inc';
echo "\n1=", include_once dirname(__FILE__) . '/include_files/counter.inc';
echo "\n1=", include_once 'counter.inc';
echo "\n1=", include_once array_pop(explode(PATH_SEPARATOR, get_include_path())).'/counter.inc';

$fred = 400;
echo "\n400=", require 'include_files/counter.inc';
echo "\n401=", require './include_files/counter.inc';
echo "\n402=", require './' . 'include_files/counter.inc';
echo "\n403=", require dirname(__FILE__) . '/include_files/counter.inc';
echo "\n404=", require 'counter.inc';
echo "\n405=", require array_pop(explode(PATH_SEPARATOR, get_include_path())).'/counter.inc';
echo "\n";

*/
?>
===DONE===
