#!/usr/bin/env python3
"""SimPoint-weighted IPC speedups for the revision dump.

IPC per simpoint = Periodic_Instructions / Periodic_Cycles (the 10M measured
instructions), weighted per app with the same SimPoint weights as the paper
plots. Configs at the default interleave are compared with the committed
baseline (hpca2027-revision-main); "-8B" configs with baseline-8B.
"""

import csv
import io
import math
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, "/users/deepmish/scarab-infra/hpca2027-main-graphs")
import plot_ipc  # noqa: E402

HERE = Path(__file__).resolve().parent
SCARAB = Path("/users/deepmish/scarab")
APPS = plot_ipc.SIMPOINT_WORKLOADS
W = plot_ipc.load_simpoint_trace_weights(Path("/dev/shm/ifuse"), APPS)

COMMITTED = {
    "baseline": ("hpca2027-revision-main",
                 "src/hpca2027-revision-main-results/datacenter/baseline"),
    "ifuse (committed, free LD2 bank)": (
        "hpca2027-revision-main",
        "src/hpca2027-revision-main-results/datacenter/ifuse"),
    "1-cycle-delayed-ifuse (committed, free LD2 bank)": (
        "hpca2027-revision-ifuse", "src/simulations/1-cycle-delayed-ifuse"),
    "helios": ("hpca2027-revision-main",
               "src/hpca2027-revision-main-results/datacenter/helios"),
    "rfp": ("hpca2027-revision-main",
            "src/hpca2027-revision-main-results/datacenter/rfp"),
}


def ipc_from_csv(text):
    d = {}
    for r in csv.reader(io.StringIO(text)):
        if len(r) >= 3 and r[0].strip() in ("Periodic_Instructions", "Periodic_Cycles"):
            d[r[0].strip()] = float(r[2])
    return d["Periodic_Instructions"] / d["Periodic_Cycles"]


def load(cfg):
    """app -> {simpoint: ipc} from the dump dir or a committed run dir."""
    out = {}
    for app in APPS:
        for (a, sp), w in W.items():
            if a != app or w <= 0:
                continue
            if cfg in COMMITTED:
                b, d = COMMITTED[cfg]
                r = subprocess.run(["git", "-C", str(SCARAB), "show",
                                    f"{b}:{d}/{app}/{sp}/core.stat.0.csv"],
                                   capture_output=True, text=True)
                text = r.stdout if r.returncode == 0 else None
            else:
                p = HERE / cfg / app / sp / "core.stat.0.csv"
                text = p.read_text() if p.exists() else None
            if text:
                out.setdefault(app, {})[sp] = ipc_from_csv(text)
    return out


def weighted(ipcs, app):
    v = ipcs.get(app, {})
    if not v:
        return None
    t = sum(W[(app, s)] for s in v)
    return sum(x * W[(app, s)] for s, x in v.items()) / t


def table(title, base_cfg, cfgs):
    base = load(base_cfg)
    data = {c: load(c) for c in cfgs}
    print(f"\n## {title} (speedup over {base_cfg})\n")
    print("| App | " + " | ".join(cfgs) + " |")
    print("|---|" + "---|" * len(cfgs))
    gm = {c: [] for c in cfgs}
    for app in APPS:
        b = weighted(base, app)
        cells = []
        for c in cfgs:
            x = weighted(data[c], app)
            if b and x:
                gm[c].append(x / b)
                cells.append(f"{100 * (x / b - 1):+.1f}%")
            else:
                cells.append("n/a")
        print(f"| {app} | " + " | ".join(cells) + " |")
    cells = []
    for c in cfgs:
        xs = gm[c]
        cells.append(f"{100 * (math.exp(sum(map(math.log, xs)) / len(xs)) - 1):+.1f}%"
                     f" ({len(xs)} apps)" if xs else "n/a")
    print("| **Geomean** | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    table("Default interleave (4 B banks)", "baseline",
          ["ifuse (committed, free LD2 bank)", "ifuse-bankport",
           "1-cycle-delayed-ifuse (committed, free LD2 bank)",
           "1-cycle-delayed-ifuse-bankport"])
    if (HERE / "baseline-8B").exists():
        table("8 B banks (--dcache_interleave_factor 8)", "baseline-8B",
              ["ifuse-bankport-8B", "1-cycle-delayed-ifuse-bankport-8B",
               "helios-8B", "rfp-8B"])
