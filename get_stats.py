#!/usr/bin/env python3
import glob
import numpy as np

files = glob.glob("results/result.*.txt")

stats = []
for file in files:
    with open(file, 'r') as f:
        s = f.read().strip().splitlines()
        last = s[-1]
        _, inst, iex, l2hit = last.split(',')
        stats.append([float(inst), float(iex), float(l2hit)])
    

A = np.asarray(stats)
print("inst",  np.median(A[:,0]))
print("iex/lsu",   np.median(A[:,1]))
print("l2hit", np.median(A[:,2]))


