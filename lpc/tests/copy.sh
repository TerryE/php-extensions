#! /bin/bash

doCopy() {
    dest="$1.php"
    echo "Creating $dest from $2.phpt"
    echo "*** Derived from PHP test script $2 ***" > $dest
    cat ~/work/php5/tests/$2.phpt >> $dest
}

doCopy test40 classes/constants_basic_001
doCopy test41 classes/constants_basic_006
doCopy test42 classes/tostring_004
doCopy test43 func/010
doCopy test44 lang/038
doCopy test45 lang/039
doCopy test46 lang/bug22592
doCopy test47 lang/bug28213
doCopy test48 lang/catchable_error_002
doCopy test49 lang/error_2_exception_001
doCopy test50 lang/include_variation2
doCopy test51 lang/include_variation3
doCopy test52 output/ob_implicit_flush_variation_001
doCopy test53 output/ob_start_basic_unerasable_002
