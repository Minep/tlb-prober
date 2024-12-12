#!/bin/bash

RESULT_DIR=results

if [ ! -d "$RESULT_DIR" ]; then
  mkdir $RESULT_DIR
fi

RUNS=100
CPU=11

PROG=${1:-evaluate}

for ((nr=0; nr < $RUNS; nr++)); do
    #echo "run" $nr
    sudo chrt --rr 99 taskset -c $CPU ./$PROG --cpu $CPU --samples 1 > $RESULT_DIR/result.$nr.txt
done


