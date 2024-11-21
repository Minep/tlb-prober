import numpy as np
import glob
import matplotlib.pyplot as plt 

def process(nr, name):
    runs = []
    vas = []

    with open(name, 'r') as f:
        obj = { "nr": nr }
        for line in f:
            line = line.strip()
            [_type, cycle, l1, l2] = line.split(',')

            if (_type == "loc"):
                vas.append(int(cycle))
                continue

            body = (int(cycle), int(l1), int(l2))
            obj[_type] = body

            if _type == "eval":
                runs.append(obj)
                obj = { "nr": nr }

    cycles  = []
    l1_lats = []
    l2_lats = []
    l1l2_lats = []

    for run in runs:
        (base_cycle, _,       _) = run["base"]
        (nm_cycle, nr_min_scat,  _) = run["no_miss"]
        (l1_cycle,   l1_l1,   _) = run["l1miss"]
        (eval_cycle_base, eval_l1, eval_l2) = run["eval"]
        nr = run["nr"]

        if not l1_l1 or not eval_l2:
            continue

        mem_acc  = (nm_cycle - base_cycle) / nr_min_scat
        l1_cycle = l1_cycle - mem_acc * nr_min_scat
        eval_cycle = eval_cycle_base - mem_acc * nr

        l1_lat = l1_cycle / l1_l1
        
        shoot_down = min(eval_l1, eval_l2)
        only_l1    = max(eval_l1 - shoot_down, 0)
        only_l2    = max(eval_l2 - shoot_down, 0)

        eval_cycle = eval_cycle - l1_lat * only_l1
        full_lat   = eval_cycle / shoot_down
        l2_lat = (full_lat - l1_lat)

        if l2_lat < 0 or l1_lat < 0:
            continue

        cycles.append(eval_cycle_base / nr)
        l1_lats.append(l1_lat)
        l2_lats.append(l2_lat)
        l1l2_lats.append(full_lat)


    return (cycles, l1_lats, l2_lats, l1l2_lats, vas)
    

def plot(ax, data, label, line=False, color = 'tab:blue', scale_fn = None):
    labels = sorted([x for x in data.keys()])
    y = []
    e = []
    for l in labels:
        v = np.mean(data[l])
        v2 = np.std(data[l])
        if scale_fn:
            v = scale_fn(v)
            v2 = scale_fn(v2)
        y.append(v)
        e.append(v2)

    if not line:
        ax.bar(range(len(labels)), y, color=color, label=label)
        ax.errorbar(range(len(labels)), y, yerr=e, ecolor='black', capsize=3, fmt="none", alpha=0.5)
    else:
        ax.plot(range(len(labels)), y, color=color, label=label, marker='.')
        ax.errorbar(range(len(labels)), y, yerr=e, ecolor=color, capsize=3, fmt="none", alpha=0.5)
        xlocs, xlabs = plt.xticks()
        for i, v in enumerate(y):
            ax.text(xlocs[i] - 0.15, v + 0.05, "%.1f"%(v))


def plot_va_dist(nr, va):
    fig, ax = plt.subplots()
    va = np.asarray(sorted(va))
    va = (va - va[0]) / 4096
    ax.hist(va, bins=512)

    fig.savefig(f"va_dist.{nr}.png")


dataset_cycles = {}
dataset_l1lat  = {}
dataset_l2lat  = {}
dataset_l1l2lat  = {}

for f in glob.glob("result.*.txt"):
    print(f)
    
    _, nr, _ = f.split('.')
    nr = int(nr)

    cycles, l1_lats, l2_lats, l1l2_lats, vas = process(nr, f)

    dataset_cycles[nr] = cycles
    dataset_l1lat[nr] = l1_lats
    dataset_l2lat[nr] = l2_lats
    dataset_l1l2lat[nr] = l1l2_lats

    print("cycles          %8.2f +- %3.2f"%(np.mean(cycles) , np.std(cycles) ))
    print("l1_refill_lat   %8.2f +- %3.2f"%(np.mean(l1_lats), np.std(l1_lats)))
    print("l2_refill_lat   %8.2f +- %3.2f"%(np.mean(l2_lats), np.std(l2_lats)))
    print("l2l1_refill_lat %8.2f +- %3.2f"%(np.mean(l1l2_lats), np.std(l1l2_lats)))
    print()

    plot_va_dist(nr, vas)

labels = sorted([x for x in dataset_l1lat.keys()])
labels_str = [str(x) for x in labels]

fig, ax1 = plt.subplots()
ax2 = ax1.twinx()

ax1.set_ylabel("cycles per access")
ax2.set_ylabel("latency (cycles)")

plt.xticks(range(len(labels)), labels_str)

ln1 = plot(ax1, dataset_cycles, "avg cycles")
ln2 = plot(ax2, dataset_l1lat , "l1 ", line=True, color="tab:orange")
ln3 = plot(ax2, dataset_l2lat , "l2 ", line=True, color="tab:red")
ln3 = plot(ax2, dataset_l1l2lat , "l1, l2", line=True, color="tab:green")

lines, labels = ax1.get_legend_handles_labels()
lines2, labels2 = ax2.get_legend_handles_labels()
ax1.legend(lines + lines2, labels + labels2, loc=0)

ax1.set_xlabel("Number of random write locations")

fig.tight_layout()
fig.savefig("figure.png")