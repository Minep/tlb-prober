#!/bin/bash

RESULT_DIR=results

if [ ! -d "$RESULT_DIR" ]; then
  mkdir $RESULT_DIR
fi

RUNS=100
CPU=11

PROG=${1:-evaluate}
NR_REPEAT=${2:-5}

for ((i=0;i<$NR_REPEAT;i++)); do
  for ((nr=0; nr < $RUNS; nr++)); do
      sudo chrt --rr 99 taskset -c $CPU ./$PROG --cpu $CPU --samples 1 > $RESULT_DIR/result.$nr.txt
  done
  ./get_stats.py
done

echo DONE
