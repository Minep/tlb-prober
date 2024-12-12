#!/bin/bash

RUNS=10
CPU=3

for ((nr=0; nr < $RUNS; nr++)); do
    echo "run" $nr
    sudo chrt --rr 99 taskset -c $CPU ./evaluate --cpu $CPU --samples 1 > result.$nr.txt
done


