#!/usr/bin/env python3
import glob
import numpy as np

files = glob.glob("results/result.*.txt")

stats = []
for file in files:
    with open(file, 'r') as f:
        s = f.read().strip().splitlines()
        last = s[-1]
        _, inst, iex, l2hit, may_invalid = last.split(',')
        if int(may_invalid) == 0:
            stats.append([float(inst), float(iex), float(l2hit)])
    

A = np.asarray(stats).reshape((-1, 3))
print(np.median(A[:,0]), np.median(A[:,1]), np.median(A[:,2]))

