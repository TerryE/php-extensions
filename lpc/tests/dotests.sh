#! /bin/bash
clean () {
    pushd /home/terry/work/ext/lpc/tests
	rm -f test*.cache {test,dd}*.{php,inc}~ test*.{out,err,sum}[12]
    test -d php5 && rm -Rf php5/*
    cd ~/work/php5; for d in $(find tests -type d); do rm -f $d/*.cache; done
    popd
}

dotest() { 
    file=$1
    baseDir=$2
    baseOP=$3
    t=${file%.ph*}
    title=$(head -2 $baseDir/$file| tail -1)
    echo -n "$baseDir - $t: $title: " 
    test -f $baseOP/$t.cache && rm $baseOP/$t.cache
	SKIP_SLOW_TESTS=1 /opt/bin/php $OPT -d lpc.cache_pattern=".*?/$file" -d lpc.cache_replacement="$baseOP/$t.cache" \
                                   $baseDir/$file 1> $baseOP/$t.out1 2> $baseOP/$t.err1    
	SKIP_SLOW_TESTS=1 /opt/bin/php $OPT -d lpc.cache_pattern=".*?/$file" -d lpc.cache_replacement="$baseOP/$t.cache" \
                                   $baseDir/$file 1> $baseOP/$t.out2 2> $baseOP/$t.err2
    cat $baseOP/$t.{out,err}1 | grep -vP \
     '^PHP Notice:|^Error:\s\d| :  Freeing |  Script:  | : Actual location |memory leaks detected|Last leak repeated ' >> $baseOP/$t.sum1
    cat $baseOP/$t.{out,err}2 | grep -vP \
     '^PHP Notice:|^Error:\s\d| :  Freeing |  Script:  | : Actual location |memory leaks detected|Last leak repeated ' >> $baseOP/$t.sum2

	grep "Missed Totals" $baseOP/$t.err[12]

    sed -i -e "s!/home/terry/work/ext/lpc/tests/!!" \
           -e 's/0x[0-9a-f]\{2,\}/0xaaaaaaaa/'      $baseOP/$t.sum1	
    sed -i -e "s!/home/terry/work/ext/lpc/tests/!!" \
           -e 's/0x[0-9a-f]\{2,\}/0xaaaaaaaa/'      \
           -e 's! ./test! test!g'                   $baseOP/$t.sum2

    grep -P "===DONE===|^--TEST--|^PHP" $baseOP/$t.sum1 >/dev/null && diff $baseOP/$t.sum[12] > $baseOP/$t.diff

	if [[ $? -eq 0 ]]; then
		echo Passed
        rm $baseOP/$t.diff
	else
		echo Failed
	fi

}

run() {
	rm -f .test*.cache {test,dd}*.php~
	rm -f test*.{out,err}[12] test*.diff
 	for t in test*.php; do
        dotest $t . .
	done
}
php() {
    inDir="$1"
    outDir="$2"
    oldDir=$(pwd)
    testdirs=$(cd inDir; find tests -type d; )
    echo $testDirs
    test -d $outDir || mkdir -p $outDir
    cd $outDir
    for d in $testdirs; do
        test -d $d || mkdir -p $d
        rm -f $d/*
        tests=$(cd $inDir/$d; ls *.{php,phpt})

        for t in $tests; do
            dotest $t $inDir/$d $outDir/$d
        done
    done
    cd $oldDir
}


OPT=""
test -n "$2" && OPT=" -d lpc.debug_flags=$2 "
case "$1" in
php)
    php ~/work/php5 ~/work/ext/lpc/tests/php5
	exit
    ;;
zend)
    php ~/work/php5/Zend ~/work/ext/lpc/tests/php5/Zend
	exit
    ;;
run)
    run
	exit
    ;;
clean)
    clean
	exit
    ;;
*)
    echo "Usage: $0 {clean|run|php|zend}"
    exit 1
esac
