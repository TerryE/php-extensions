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

		diff $t.out[12] > /dev/null

		if [[ $? -eq 0 ]]; then
			echo $t Passed
		else
			echo $t Failed
			exit 1 
		fi
	done
}
case "$1" in
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
