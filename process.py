import numpy as np

runs = []

with open("result.txt", 'r') as f:
    obj = {}
    for line in f:
        line = line.strip()
        [_type, cycle, l1, l2] = line.split(',')

        body = (int(cycle), int(l1), int(l2))
        obj[_type] = body

        if _type == "eval":
            runs.append(obj)
            obj = {}

cycles  = []
l1_lats = []
l2_lats = []

for run in runs:
    (base_cycle, base_l1, base_l2) = run["base"]
    (l1_cycle,   l1_l1,   _) = run["l1miss"]
    (eval_cycle, eval_l1, eval_l2) = run["eval"]

    l1_cycle -= base_cycle
    eval_cycle -= base_cycle

    l1_lat = l1_cycle / l1_l1
    l2_lat = (eval_cycle - l1_lat * eval_l1) / eval_l2

    cycles.append(eval_cycle)
    l1_lats.append(l1_lat)
    l2_lats.append(l2_lat)


print("cycles        %8.2f +- %8.2f"%(np.mean(cycles) , np.std(cycles) ))
print("l1_refill_lat %8.2f +- %8.2f"%(np.mean(l1_lats), np.std(l1_lats)))
print("l2_refill_lat %8.2f +- %8.2f"%(np.mean(l2_lats), np.std(l2_lats)))