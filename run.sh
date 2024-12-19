#!/bin/bash

RESULT_DIR=results

if [ ! -d "$RESULT_DIR" ]; then
  mkdir $RESULT_DIR
fi

RUNS=17
CPU=3

PROG=${1:-evaluate}

# for ((i=1;i<=RUNS;i++)); do
#   sudo chrt -r 99 taskset -c $CPU ./$PROG --cpu $CPU --samples $i >> result.seq.txt
# done

declare -a arr=(1 3 4 5)

## now loop through the above array
for i in "${arr[@]}"
do
  for m in 8 32; do
    if [ $m -eq 0 ]; then
      RUNS=15
    else
      RUNS=128
    fi
    printf "" > result.s$i.m$m.txt
    sudo chrt -r 99 taskset -c $CPU ./$PROG \
        --cpu $CPU --samples $RUNS --skip $i --mode $m >> result.s$i.m$m.txt
    echo DONE: skip$i.mode$m
  done
done
