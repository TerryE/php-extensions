#! /bin/bash
clean () {
    pushd /home/terry/work/ext/lpc/tests
	rm -f .test*.cache {test,dd}*.php~
	rm -f test*.{out,err,sum}[12]
    test -d php && rm -R php
    popd
}

dotest() { 
    file=$1
    baseDir=$2
    baseOP=$3
    t=${file%%.*}

    test -f $baseDir/.$t.cache && rm $baseDir/.$t.cache
	SKIP_SLOW_TESTS=1 /opt/bin/php $baseDir/$file 1> $baseOP/$t.out1 2> $baseOP/$t.err1
	SKIP_SLOW_TESTS=1 /opt/bin/php $baseDir/$file 1> $baseOP/$t.out2 2> $baseOP/$t.err2

    cat $baseOP/$t.{out,err}1 | grep -vP \
     '^PHP Notice:|^Error:\s\d| :  Freeing |  Script:  | : Actual location |memory leaks detected|Last leak repeated ' >> $baseOP/$t.sum1
    cat $baseOP/$t.{out,err}2 | grep -vP \
     '^PHP Notice:|^Error:\s\d| :  Freeing |  Script:  | : Actual location |memory leaks detected|Last leak repeated ' >> $baseOP/$t.sum2

	grep "Missed Totals" $baseOP/$t.err[12]

    sed -i -e "s!/home/terry/work/ext/lpc/tests/!!" \
           -e 's/0x[0-9a-f]\{2,\}/0xaaaaaaaa/' $baseOP/$t.sum1	
    sed -i -e "s!/home/terry/work/ext/lpc/tests/!!" \
           -e 's/0x[0-9a-f]\{2,\}/0xaaaaaaaa/'      \
           -e 's! ./test! test!g'                   $baseOP/$t.sum2

    grep -P "===DONE===|^--TEST--|^PHP" $baseOP/$t.sum1 >/dev/null && diff $baseOP/$t.sum[12] > $baseOP/$t.diff

	if [[ $? -eq 0 ]]; then
		echo $t Passed
        rm $baseOP/$t.diff
	else
		echo $t Failed
	fi
}

run() {
	rm -f .test*.cache {test,dd}*.php~
	rm -f test*.{out,err}[12]
 	for t in t*.php; do
        dotest $t . .
	done
}
php() {
    oldDir=$(pwd)
    cd php
    testdirs=$(cd ~/work/php5/tests; find * -type d)

    for d in $testdirs; do
        test -d $d || mkdir $d
        rm $d/* $d/.*.cache
        tests=$(cd ~/work/php5/tests/$d; ls *.phpt)
	    for t in $tests; do
            dotest $t ~/work/php5/tests/$d ./$d
	    done
    done
    popd
}
case "$1" in
php)
    php
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
    echo "Usage: $0 {clean|run}"
    exit 1
esac
