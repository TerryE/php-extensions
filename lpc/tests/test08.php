TEST @include to check error suppression
<.inc
if (1 != @include("dd_missing.inc")) {
    echo "include error detected \n";
}
echo "hello word\n";
?>
===DONE===
