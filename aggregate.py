#!/usr/bin/env python3
import numpy as np

arr = []
while True:
    v = input()
    if v == "DONE":
        break
    
    [inst, iex, l2hit] = v.strip().split(' ')
    arr.append([float(inst), float(iex), float(l2hit)])

A = np.asarray(arr)
print("inst:  %.2f, std: %.2f"%(np.mean(A[:,0]), np.std(A[:,0])))
print("iex:   %.2f, std: %.2f"%(np.mean(A[:,1]), np.std(A[:,1])))
print("l2hit: %.2f, std: %.2f"%(np.mean(A[:,2]), np.std(A[:,2])))