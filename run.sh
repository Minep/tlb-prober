#!/bin/bash

items=("32" "64" "128" "256" "512")
CPU=3

for nr in "${items[@]}"; do
    sudo chrt --rr 99 taskset -c $CPU ./evaluate --cpu $CPU --samples 1024 --scatter $nr > result.$nr.txt
done


