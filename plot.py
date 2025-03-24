#!/usr/bin/env python3
import matplotlib.pyplot as plt
import matplotlib as mlt
import numpy as np
import sys, os

plt.rcParams["figure.figsize"] = (15,4)

def get_plot_policy(basename):
    [name,skip,mode,o] = basename.split('.')
    nr_skip = int(skip[1:])
    nr_mode = int(mode[1:])
    nr_off  = int(o[1:])

    def plot_policy_log2(data):
        fig, ax = plt.subplots()
        data_rint = np.rint(data)
        
        ax.set_title(basename)
        ax.plot(data)
        ax.plot(data_rint, 'r--')

        ax.set_title(f"{name} ({nr_skip * 4}K aligned, POT)")
        ax.set_yscale("log", base=2)
        ax.set_ylabel("cycles")
        ax.set_xlabel("$2^n$ pages")
        ax.set_xticks(range(len(data)))
        ax.set_yticks([1 << x for x in [1, 2, 3, 4, 5, 6, 7, 8, 9]])

        return (fig, ax)
    
    def plot_policy(data):
        fig, ax = plt.subplots()
        data_rint = np.rint(data)
        
        ax.set_title(basename)
        ax.plot(range(1, len(data)+1), data)
        ax.plot(range(1, len(data)+1), data_rint, 'r--')

        ax.set_title(f"{name} ({nr_skip * 4}K aligned, x{nr_mode}, start_from={nr_off * nr_mode})")
        ax.set_ylabel("cycles")
        ax.set_xlabel(f"bundles of {nr_mode} pages")

        return (fig, ax)

    if nr_mode == 0:
        return plot_policy_log2
    return plot_policy

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

    plotter = get_plot_policy(basename)

    fig, ax = plotter(np.asarray(data))

    fig.savefig(basename + ".png")
    plt.close(fig)

for inp in sys.argv[1:]:
    draw(inp)