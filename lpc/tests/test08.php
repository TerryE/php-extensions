TEST @include to check error suppression
<?php
if (1 != @include("dd_missing.php")) {
    echo "include error detected \n";
}
echo "hello word\n";
?>
===DONE===
