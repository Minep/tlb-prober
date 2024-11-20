#!/bin/bash

sudo chrt -r 99 taskset -c 0 ./evaluate --cpu 0 --samples 30 --scatter 64 > result.txt
