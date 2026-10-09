"""Backend-stalled cycles as % of Baseline cycles, by full structure and ROB-head state.
Per simpoint: stat_CYCLES / Baseline NODE_CYCLE (core.stat.0.csv); per app: workloads_db-weighted mean.
Usage: stalls.py BASE_DIR NAME=DIR ..."""
import csv, json, sys
from pathlib import Path
DB=Path("/users/deepmish/scarab-infra/workloads/workloads_db.json")
APPS=["bfs","dfs","pagerank","corebench","appworld","terminal_bench","cachebench","clickhouse","duckdb","leveldb","memcached"]
K=["FULL_PRF","FULL_ROB","FULL_LQ","FULL_SQ","HEAD_LLC_MISS","HEAD_L1D_MISS","HEAD_L1D_HIT","HEAD_DCACHE_PORT","HEAD_FUSED_LD2"]
def stats(d):
    f=d/"core.stat.0.csv"; v={}
    for r in csv.reader(open(f)):
        if len(r)>=3: v[r[0].strip()]=float(r[2])
    return v
db=json.load(open(DB))["datacenter"]["datacenter"]
base=Path(sys.argv[1]); cfgs=[("Baseline",base)]+[(a.split("=")[0],Path(a.split("=")[1])) for a in sys.argv[2:]]
print("app,config,"+",".join(K))
avg={n:[0.0]*len(K) for n,_ in cfgs}
for app in APPS:
    for n,d in cfgs:
        acc=[0.0]*len(K); tw=0
        for sp in db[app]["simpoints"]:
            w=float(sp["weight"]); c=str(sp["cluster_id"])
            if w<=0: continue
            b=stats(base/app/c); x=stats(d/app/c); cyc=b["NODE_CYCLE_count"]
            for i,k in enumerate(K): acc[i]+=w*100*x.get(f"TOPDOWN_BE_{k}_CYCLES_count",float('nan'))/cyc
            tw+=w
        row=[a/tw for a in acc]
        for i in range(len(K)): avg[n][i]+=row[i]/len(APPS)
        print(f"{app},{n},"+",".join(f"{v:.2f}" for v in row))
for n,_ in cfgs: print(f"Average,{n},"+",".join(f"{v:.2f}" for v in avg[n]))
