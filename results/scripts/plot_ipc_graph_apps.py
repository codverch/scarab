#!/usr/bin/env python3
"""IPC benefit graph for the graph-analytics workloads only.

Companion to rfp_coverage_graph.png: same 8 graph workloads, same order, but shows
per-workload IPC speedup vs baseline (with a Geomean bar). Self-contained.

Reads  ../data/ipc_speedup_summary.csv
Writes ../graphs/rfp_ipc_speedup_graph.png

Usage: plot_ipc_graph_apps.py     (paths are relative to this script)
"""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "data")
GRAPHS = os.path.join(HERE, "..", "graphs")
os.makedirs(GRAPHS, exist_ok=True)

# Same set + order as GROUPS["Graph analytics"] in plot_results.py
GRAPH_APPS = ["bfs", "dfs", "cc", "cd", "tc", "pagerank", "sssp_ego_fb", "bc"]
DISP = {}  # no display renames needed for graph apps

def load(fn):
    with open(os.path.join(DATA, fn)) as f:
        return {r["workload"]: r for r in csv.DictReader(f)}

def geomean_pct(pcts):
    ratios = [1.0 + p/100.0 for p in pcts]
    g = np.prod(ratios) ** (1.0 / len(ratios))
    return (g - 1.0) * 100.0

def main():
    d = load("ipc_speedup_summary.csv")
    names = [DISP.get(w, w) for w in GRAPH_APPS if w in d]
    sp = [float(d[w]["speedup_pct"]) for w in GRAPH_APPS if w in d]

    gm = geomean_pct(sp)
    names_all = names + ["Geomean"]
    sp_all = sp + [gm]
    colors = ["royalblue"] * len(sp) + ["#2e7d32"]  # geomean in green

    fig, ax = plt.subplots(figsize=(11, 6.5))
    bars = ax.bar(range(len(names_all)), sp_all, color=colors,
                  edgecolor="black", linewidth=0.6, width=0.6)
    ax.axhline(0, color="black", linewidth=1.0)

    yr = max(sp_all) - min(sp_all)
    for b, v in zip(bars, sp_all):
        ax.text(b.get_x() + b.get_width()/2, v + (yr*0.012 if v >= 0 else -yr*0.012),
                f"{v:.1f}", ha="center", va="bottom" if v >= 0 else "top", fontsize=10)

    ax.set_title("RFP IPC benefit — Graph analytics (8-way L1 DCache)", fontsize=15)
    ax.set_ylabel("IPC (% vs baseline)", fontsize=12)
    ax.set_xlabel("Workloads", fontsize=12)
    ax.set_xticks(range(len(names_all)))
    ax.set_xticklabels(names_all, rotation=45, ha="right")
    ax.set_ylim(min(min(sp_all)-yr*0.10, 0), max(sp_all)+yr*0.12)
    ax.set_axisbelow(True); ax.yaxis.grid(True, linestyle=":", color="lightgray")
    for s in ("top", "right"): ax.spines[s].set_visible(False)
    # dashed divider before the Geomean bar (mirrors the coverage graph)
    ax.axvline(len(names)-0.5, color="gray", linewidth=0.8, linestyle="--")

    out = os.path.join(GRAPHS, "rfp_ipc_speedup_graph.png")
    fig.tight_layout(); fig.savefig(out, dpi=150); plt.close(fig)
    print("wrote", os.path.normpath(out))
    print(f"graph-apps geomean IPC speedup = {gm:.2f}%")

if __name__ == "__main__":
    main()
