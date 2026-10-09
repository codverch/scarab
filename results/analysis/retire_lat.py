"""Fetch-to-retire load latency reduction vs Baseline, weighted as plot_load_latency.py.

Per simpoint: avg = LD_RETIRE_MINUS_FETCH_LATENCY / IFUSE_ALL_LOADS (the I-Fuse
run's on-path load count; every config retires the same instruction stream).
Per app: weighted mean of avg over simpoints; reduction = 1 - cfg/base.
Usage: retire_lat.py BASE_DIR LOADS_DIR NAME=DIR ...
"""
import csv, json, sys
from pathlib import Path
DB = Path("/users/deepmish/scarab-infra/workloads/workloads_db.json")
APPS = ["bfs","dfs","pagerank","corebench","appworld","terminal_bench","cachebench","clickhouse","duckdb","leveldb","memcached"]
def stat(f, name):
    if not f.is_file(): return None
    for r in csv.reader(open(f)):
        if r and r[0].strip() == name: return float(r[2])
def lat(d, app, sp): return stat(Path(d)/app/sp/"core.stat.0.csv", "LD_RETIRE_MINUS_FETCH_LATENCY_count")
def loads(d, app, sp): return stat(Path(d)/app/sp/"ifuse.stat.0.csv", "IFUSE_ALL_LOADS_count")
db = json.load(open(DB))["datacenter"]["datacenter"]
base, ld = sys.argv[1], sys.argv[2]; cfgs = [a.split("=",1) for a in sys.argv[3:]]
print("app," + ",".join(n for n,_ in cfgs)); sums = {n: [] for n,_ in cfgs}
for app in APPS:
    row = []
    for n, d in cfgs:
        wb = wc = tw = 0.0
        for sp in db[app]["simpoints"]:
            w = float(sp["weight"]); c = str(sp["cluster_id"])
            b, x, L = lat(base, app, c), lat(d, app, c), loads(ld, app, c)
            if w <= 0 or None in (b, x, L) or L == 0: continue
            wb += w*b/L; wc += w*x/L; tw += w
        r = 100*(1 - wc/wb) if wb else float("nan"); sums[n].append(r); row.append(f"{r:.2f}")
    print(app + "," + ",".join(row))
print("Average," + ",".join(f"{sum(v)/len(v):.2f}" for v in sums.values()))
