#!/bin/bash

RUNS=1
CPU=3

for ((nr=0, nr <= $RUNS, nr++)); do
    sudo chrt --rr 99 taskset -c $CPU ./evaluate --cpu $CPU --samples 1 > result.$nr.txt
    sleep 2
done


