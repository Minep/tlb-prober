#!/bin/bash

sudo taskset -c 3 chrt --rr 99 ./evaluate --cpu 3 --samples 32 --scatter 64 > result.txt
