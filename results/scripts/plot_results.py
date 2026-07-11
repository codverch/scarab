#!/usr/bin/env python3
"""Reproduce all RFP result graphs from the committed summary CSVs (self-contained).

Reads ../data/ipc_speedup_summary.csv and ../data/rfp_coverage_summary.csv and writes
into ../graphs/:
  rfp_ipc_speedup.png            - per-workload IPC speedup vs baseline
  rfp_coverage_combined.png      - Injected/Executed/Useful, all 20 workloads + Mean
  rfp_coverage_graph.png         - per category (graph / databases / llm / ml), each + Mean
  rfp_coverage_databases.png
  rfp_coverage_llm.png
  rfp_coverage_ml.png

Usage: plot_results.py            (paths are relative to this script)
"""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "data")
GRAPHS = os.path.join(HERE, "..", "graphs")
os.makedirs(GRAPHS, exist_ok=True)

PURPLE, PINK, RED = "#8e44ad", "#ff5fa2", "#e23b3b"
DISP = {"sweRepair": "swe"}
GROUPS = {
    "Graph analytics":       ["bfs", "dfs", "cc", "cd", "tc", "pagerank", "sssp_ego_fb", "bc"],
    "Databases & KV stores": ["memcached", "mongodb", "mysql", "postgres", "redis"],
    "LLM / Agent":           ["chemcrow", "haystack", "langchain", "sweRepair", "toolformer"],
    "ML frameworks":         ["pytorch", "tensorflow"],
}
REF_ORDER = ["bfs", "cc", "pagerank", "sssp_ego_fb", "tc", "bc", "dfs", "cd", "chemcrow",
             "haystack", "langchain", "memcached", "mongodb", "mysql", "postgres", "redis",
             "sweRepair", "toolformer", "pytorch", "tensorflow"]

def load(fn):
    with open(os.path.join(DATA, fn)) as f:
        return {r["workload"]: r for r in csv.DictReader(f)}

# ---------------- IPC speedup ----------------
def plot_speedup():
    d = load("ipc_speedup_summary.csv")
    names = [DISP.get(w, w) for w in REF_ORDER if w in d]
    sp = [float(d[w]["speedup_pct"]) for w in REF_ORDER if w in d]
    fig, ax = plt.subplots(figsize=(14, 7))
    bars = ax.bar(range(len(names)), sp, color="royalblue", edgecolor="black", linewidth=0.6, width=0.6)
    ax.axhline(0, color="black", linewidth=1.0)
    yr = max(sp) - min(sp)
    for b, v in zip(bars, sp):
        ax.text(b.get_x() + b.get_width()/2, v + (yr*0.012 if v >= 0 else -yr*0.012),
                f"{v:.1f}", ha="center", va="bottom" if v >= 0 else "top", fontsize=9)
    ax.set_title("RFP IPC normalized to baseline", fontsize=15)
    ax.set_ylabel("IPC (% vs baseline)", fontsize=12); ax.set_xlabel("Workloads", fontsize=12)
    ax.set_xticks(range(len(names))); ax.set_xticklabels(names, rotation=45, ha="right")
    ax.set_ylim(min(min(sp)-yr*0.10, 0), max(sp)+yr*0.10)
    ax.set_axisbelow(True); ax.yaxis.grid(True, linestyle=":", color="lightgray")
    for s in ("top", "right"): ax.spines[s].set_visible(False)
    fig.tight_layout(); fig.savefig(os.path.join(GRAPHS, "rfp_ipc_speedup.png"), dpi=150); plt.close(fig)

# ---------------- coverage (3-bar) ----------------
def plot_cov(names, inj, exe, cov, title, out, figw):
    x = np.arange(len(names)); bw = 0.27
    fig, ax = plt.subplots(figsize=(figw, 6.5))
    b1 = ax.bar(x-bw, inj, bw, label="Prefetches Injected", color=PURPLE)
    b2 = ax.bar(x,    exe, bw, label="Prefetches Executed", color=PINK)
    b3 = ax.bar(x+bw, cov, bw, label="Prefetches Useful (Coverage)", color=RED)
    small = len(names) > 10
    for bars, vals in ((b1, inj), (b2, exe), (b3, cov)):
        for b, v in zip(bars, vals):
            ax.text(b.get_x()+b.get_width()/2, v+0.8, f"{v:.0f}", ha="center",
                    va="bottom", rotation=90 if small else 0, fontsize=6.5 if small else 8.5)
    ax.set_ylabel("Coverage (Fraction of all Loads)", fontsize=12)
    ax.set_xlabel("Workloads", fontsize=12)
    ax.set_title(title, fontsize=14, pad=34)
    ax.set_xticks(x); ax.set_xticklabels(names, rotation=45, ha="right")
    ax.set_ylim(0, 108)
    ax.yaxis.set_major_formatter(mticker.PercentFormatter(decimals=0))
    ax.set_axisbelow(True); ax.yaxis.grid(True, linestyle=":", color="lightgray")
    for s in ("top", "right"): ax.spines[s].set_visible(False)
    ax.axvline(len(names)-1.5, color="gray", linewidth=0.8, linestyle="--")
    ax.legend(loc="lower center", bbox_to_anchor=(0.5, 1.005), ncol=3, fontsize=10, frameon=False)
    fig.tight_layout(); fig.savefig(out, dpi=150); plt.close(fig)

def plot_coverage():
    d = load("rfp_coverage_summary.csv")
    def vals(wls):
        n, i, e, c = [], [], [], []
        for w in wls:
            if w in d:
                n.append(DISP.get(w, w)); i.append(float(d[w]["injected_pct"]))
                e.append(float(d[w]["executed_pct"])); c.append(float(d[w]["useful_pct"]))
        return n, i, e, c
    # per-group
    fnmap = {"Graph analytics": "graph", "Databases & KV stores": "databases",
             "LLM / Agent": "llm", "ML frameworks": "ml"}
    for g, wls in GROUPS.items():
        n, i, e, c = vals(wls)
        n2, i2, e2, c2 = n+["Mean"], i+[np.mean(i)], e+[np.mean(e)], c+[np.mean(c)]
        plot_cov(n2, i2, e2, c2, f"Timeliness and Accuracy of RFP — {g}",
                 os.path.join(GRAPHS, f"rfp_coverage_{fnmap[g]}.png"), max(7, 1.1*len(n2)+2))
    # combined
    n, i, e, c = vals(REF_ORDER)
    n2, i2, e2, c2 = n+["Mean"], i+[np.mean(i)], e+[np.mean(e)], c+[np.mean(c)]
    plot_cov(n2, i2, e2, c2, "Timeliness and Accuracy of RFP (datacenter workloads)",
             os.path.join(GRAPHS, "rfp_coverage_combined.png"), 22)

if __name__ == "__main__":
    plot_speedup(); plot_coverage()
    print("wrote graphs to", os.path.normpath(GRAPHS))
