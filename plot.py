#!/usr/bin/env python3
import matplotlib.pyplot as plt
import matplotlib as mlt
import numpy as np
import sys, os

plt.rcParams["figure.figsize"] = (15,4)

def draw(inp):
    basename = os.path.basename(inp)
    basename = '.'.join(basename.split('.')[:-1])
    data = []
    with open(inp) as f:
        lines = f.read().strip().splitlines()

        for d in lines:
            if d == "DONE":
                break
            try:
                data.append(float(d))
            except:
                data.append(np.nan)

    fig, ax = plt.subplots()
    data_rint = np.asarray(data)
    data = np.rint(data_rint)

    ax.set_title(basename)
    ax.plot(data_rint)
    ax.plot(data, 'r--')
    
    ax.set_yscale("log", base=2)
    ax.set_ylabel("cycles")
    ax.set_xlabel("$2^n$ pages")
    #ax.set_xticks(range(len(data)))

    fig.savefig(basename + ".png")

for inp in sys.argv[1:]:
    draw(inp)