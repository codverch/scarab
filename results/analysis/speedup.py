"""Per-app speedup over Baseline, weighted as plot_ipc.py does.

speedup(app) = weighted IPC(config) / weighted IPC(baseline) - 1, where each
app's IPC is the workloads_db-weighted mean over the simpoints that both runs
have. Usage: speedup.py BASELINE_DIR NAME=DIR [NAME=DIR ...]
"""
import csv, json, sys
from pathlib import Path

DB = Path("/users/deepmish/scarab-infra/workloads/workloads_db.json")
APPS = ["bfs", "dfs", "pagerank", "corebench", "appworld", "terminal_bench",
        "cachebench", "clickhouse", "duckdb", "leveldb", "memcached"]

def weights():
    db = json.load(open(DB))["datacenter"]["datacenter"]
    return {(a, str(sp["cluster_id"])): float(sp["weight"])
            for a in APPS for sp in db[a]["simpoints"] if float(sp["weight"]) > 0}

def ipc(d):
    f = d / "core.stat.0.csv"
    if not f.is_file():
        return None
    v = {}
    for row in csv.reader(open(f)):
        if len(row) >= 3 and row[0].strip() in ("Periodic_Instructions", "Periodic_Cycles"):
            v[row[0].strip()] = float(row[2])
    return v["Periodic_Instructions"] / v["Periodic_Cycles"] if len(v) == 2 else None

def find(root, app, sp):
    for p in (root / app / sp, root / "datacenter" / "datacenter" / app / sp):
        if (p / "core.stat.0.csv").is_file():
            return p
    return None

def main():
    base = Path(sys.argv[1])
    cfgs = [a.split("=", 1) for a in sys.argv[2:]]
    w = weights()
    print("app," + ",".join(n for n, _ in cfgs) + ",simpoints")
    sums = {n: [] for n, _ in cfgs}
    for app in APPS:
        sps = [sp for (a, sp) in w if a == app]
        row = []
        used = None
        for n, d in cfgs:
            num = den = tw = 0.0
            cnt = 0
            for sp in sps:
                b, c = find(base, app, sp), find(Path(d), app, sp)
                if not b or not c:
                    continue
                ib, ic = ipc(b), ipc(c)
                if ib is None or ic is None:
                    continue
                num += w[(app, sp)] * ic; den += w[(app, sp)] * ib; cnt += 1
            s = 100 * (num / den - 1) if den else float("nan")
            sums[n].append(s); row.append(f"{s:.2f}"); used = cnt
        print(f"{app}," + ",".join(row) + f",{used}")
    print("Average," + ",".join(f"{sum(v)/len(v):.2f}" for v in sums.values()) + ",")

main()
