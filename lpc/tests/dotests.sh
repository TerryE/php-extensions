#! /bin/bash
clean () {
	rm -f .test*.cache {test,dd}*.php~
	rm -f test*.{out,err}[12]
}

run() {
	clean
 	for t in t*.php; do
		/opt/bin/php $t 1> $t.out1 2> $t.err1
		/opt/bin/php $t 1> $t.out2 2> $t.err2

		grep "Missed Totals" $t.err[12]
	
        tail -3 $t.out1 | grep "===DONE===" > /dev/null && diff $t.out[12] > /dev/null

		if [[ $? -eq 0 ]]; then
			echo $t Passed
		else
			echo $t Failed
		fi
	done
}
php() {
	clean
    tests=$(cd ~/work/php5/tests; find * -name \*.phpt)
    declare -i abort
    cd php
	for t in $tests; do
		/opt/bin/php ~/work/php5/tests/$t 1> $t.out1 2> $t.err1
		/opt/bin/php ~/work/php5/tests/$t 1> $t.out2 2> $t.err2
		grep "Missed Totals" $t.err[12]
	    
        let abort=1
        egrep "^PHP Parse error:" $t.err1 && let abort=0 
        head -3 $t.out1 | egrep "^--TEST--" > /dev/null && let abort=0

        if [[ $abort -eq 1 ]]; then
			echo $t Failed
            cd -
            exit
        else
            diff $t.out[12] > /dev/null
		    if [[ $? -eq 0 ]]; then
			    echo $t Passed
		    else
			    echo $t Failed
                cd -
                exit
		    fi
        fi
	done
    cd -
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
