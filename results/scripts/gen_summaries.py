#!/usr/bin/env python3
"""Distill a scarab-infra RFP sweep into compact, git-friendly summary CSVs.

Reads <sim_dir>/collected_stats.csv (IPC) and the per-simpoint rfp.stat.0.csv files,
plus simpoint weights from workloads_db.json, and writes:
  data/ipc_speedup_summary.csv   - per-workload simpoint-weighted baseline/rfp IPC + speedup%
  data/rfp_coverage_summary.csv  - per-workload injected/executed/useful (% of all loads)

Usage: gen_summaries.py [sim_dir] [workloads_db.json] [out_data_dir]
"""
import json, csv, re, sys, os

SIM = sys.argv[1] if len(sys.argv) > 1 else "/users/vedlaksh/simulations/rfp_sweep"
DB  = sys.argv[2] if len(sys.argv) > 2 else "/users/vedlaksh/scarab-infra/workloads/workloads_db.json"
OUT = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(__file__), "..", "data")
os.makedirs(OUT, exist_ok=True)
W = json.load(open(DB))["datacenter"]["datacenter"]

# canonical workload order (reference order; ML appended)
ORDER = ["bfs", "cc", "pagerank", "sssp_ego_fb", "tc", "bc", "dfs", "cd", "chemcrow",
         "haystack", "langchain", "memcached", "mongodb", "mysql", "postgres", "redis",
         "sweRepair", "toolformer", "pytorch", "tensorflow"]

def stat_total(path, name):
    try:
        for line in open(path):
            p = [x.strip() for x in line.split(",")]
            if p and p[0] == name + "_total_count":
                nums = [x for x in p[1:] if x.replace('.', '').replace('-', '').isdigit()]
                return float(nums[-1]) if nums else 0.0
    except FileNotFoundError:
        return 0.0
    return 0.0

# --- IPC speedup from collected_stats.csv (simpoint-weighted IPC already aggregated) ---
rows = list(csv.reader(open(os.path.join(SIM, "collected_stats.csv"))))
hdr = rows[0]; ipc = next(r for r in rows if r[0] == "IPC")
base, rfp = {}, {}
pat = re.compile(r"(baseline|rfp) datacenter/datacenter/(\S+)\s")
for col, h in enumerate(hdr):
    m = pat.match(h + " ")
    if m:
        (base if m.group(1) == "baseline" else rfp)[m.group(2)] = float(ipc[col])

with open(os.path.join(OUT, "ipc_speedup_summary.csv"), "w", newline="") as f:
    w = csv.writer(f); w.writerow(["workload", "baseline_ipc", "rfp_ipc", "speedup_pct"])
    for wl in ORDER:
        if wl in base and wl in rfp and base[wl]:
            w.writerow([wl, f"{base[wl]:.6f}", f"{rfp[wl]:.6f}", f"{(rfp[wl]/base[wl]-1)*100:.4f}"])

# --- RFP coverage counters from per-simpoint rfp.stat.0.csv (simpoint-weighted) ---
with open(os.path.join(OUT, "rfp_coverage_summary.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["workload", "all_loads_w", "injected_w", "executed_w", "useful_w",
                "injected_pct", "executed_pct", "useful_pct"])
    for wl in ORDER:
        AI = AE = AS = AL = 0.0
        for sp in W[wl]["simpoints"]:
            d = f"{SIM}/rfp/datacenter/datacenter/{wl}/{sp['cluster_id']}/rfp.stat.0.csv"
            L = stat_total(d, "RFP_ALL_LOADS")
            if L == 0:
                continue
            wt = sp["weight"]
            AI += wt * stat_total(d, "RFP_PREFETCH_INJECTED")
            AE += wt * stat_total(d, "RFP_PREFETCH_EXECUTED")
            AS += wt * stat_total(d, "RFP_PRF_SERVED")
            AL += wt * L
        if AL == 0:
            continue
        w.writerow([wl, f"{AL:.1f}", f"{AI:.1f}", f"{AE:.1f}", f"{AS:.1f}",
                    f"{100*AI/AL:.4f}", f"{100*AE/AL:.4f}", f"{100*AS/AL:.4f}"])

print("wrote", os.path.join(OUT, "ipc_speedup_summary.csv"))
print("wrote", os.path.join(OUT, "rfp_coverage_summary.csv"))
