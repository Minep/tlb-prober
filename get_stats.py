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

def reject_outliers(data, i, m = 2.):
    d = np.abs(data[:,i] - np.median(data[:,i]))
    mdev = np.median(d)
    s = d/mdev if mdev else np.zeros(len(d))
    return data[s<m,:]

A = np.asarray(stats).reshape((-1, 3))
print("valid:", A.shape[0])
print("inst:  %.2f, std: %.2f"%(np.mean(A[:,0]), np.std(A[:,0])))
print("iex:   %.2f, std: %.2f"%(np.mean(A[:,1]), np.std(A[:,1])))
print("l2hit: %.2f, std: %.2f"%(np.mean(A[:,2]), np.std(A[:,2])))