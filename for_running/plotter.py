import os
import re
import argparse
import matplotlib.pyplot as plt
from collections import defaultdict
from statistics import geometric_mean
import sys

from termcolor import colored

# unfortunately, this is hardcoded to work with the simpoint traces

# --- argument parsing ---
parser = argparse.ArgumentParser()
parser.add_argument("--record-dir",    default="records", help="Directory containing result .txt files")
parser.add_argument("--sim-root",      default="/users/DTRDNT/main/simpoint_traces",
                                         help="Root of simpoint_traces/<app>/simpoints")
args = parser.parse_args()
record_dir = args.record_dir
sim_root   = args.sim_root

# --- prepare regex and containers ---
ipc_pattern   = re.compile(r"--\s*([\d.]+)\s*IPC")
# Now store lists of (ipc, weight)
vanilla_data  = defaultdict(list)   # app -> [ (ipc, w), ... ]
fused_data    = defaultdict(list)

# cache per-app trace→weight maps
trace_weight = {}  # app -> { traceIndex(str) : weight(float), ... }

def load_weights_for(app):
    """Load opt.p and opt.w for this app, build trace→weight map."""
    d = {}
    pfile = os.path.join(sim_root, app, "simpoints", "opt.p")
    wfile = os.path.join(sim_root, app, "simpoints", "opt.w")
    if not os.path.exists(pfile):
        print(f"Error: missing mapping file {pfile}", file=sys.stderr)
        sys.exit(1)
    if not os.path.exists(wfile):
        print(f"Error: missing weight file  {wfile}", file=sys.stderr)
        sys.exit(1)

    # traceIndex → simIndex
    trace2sim = {}
    with open(pfile) as f:
        for line in f:
            parts = line.split()
            if len(parts) != 2: continue
            trace2sim[parts[0]] = parts[1]

    # simIndex → weight
    sim2w = {}
    with open(wfile) as f:
        for line in f:
            parts = line.split()
            if len(parts) != 2: continue
            sim2w[parts[1]] = float(parts[0])

    # build traceIndex → weight
    for t, s in trace2sim.items():
        if s not in sim2w:
            print(f"Warning: no weight for simIndex {s} in {wfile}", file=sys.stderr)
            continue
        d[t] = sim2w[s]

    trace_weight[app] = d

# --- gather IPC data ---
if not os.path.isdir(record_dir):
    print(f"Error: directory '{record_dir}' not found.", file=sys.stderr)
    sys.exit(1)

files_found = False
for fname in os.listdir(record_dir):
    if not fname.endswith('.txt'):
        continue
    files_found = True
    full = os.path.join(record_dir, fname)

    # determine app and traceIndex
    if fname.endswith('_vanilla.txt'):
        base = fname[:-len('_vanilla.txt')]   # e.g. "clang_62"
        target = vanilla_data
    elif fname.endswith('_fused.txt'):
        base = fname[:-len('_fused.txt')]
        target = fused_data
    else:
        print(f"Warning: Unrecognized file pattern '{fname}'", file=sys.stderr)
        continue

    parts = base.split('_', 1)
    if len(parts) != 2:
        print(f"Warning: can't extract traceIndex from '{base}'", file=sys.stderr)
        continue
    app, trace = parts[0], parts[1]

    # load weights for this app on first sight
    if app not in trace_weight:
        load_weights_for(app)

    wmap = trace_weight[app]
    if trace not in wmap:
        print(f"Warning: no weight for trace {trace} of app {app}", file=sys.stderr)
        continue
    w = wmap[trace]

    # extract IPC
    text = open(full).read()
    m = ipc_pattern.search(text)
    if not m:
        print(f"Warning: No IPC found in '{fname}'", file=sys.stderr)
        continue
    ipc = float(m.group(1))

    # store (ipc, weight)
    target[app].append((ipc, w))

if not files_found:
    print("Error: No .txt files found.", file=sys.stderr)
    sys.exit(1)

apps = sorted(set(vanilla_data) & set(fused_data))
if not apps:
    print("Error: No apps with both vanilla and fused data.", file=sys.stderr)
    sys.exit(1)

# --- compute weighted averages ---
def weighted_avg(pairs):
    total_w = sum(w for (_, w) in pairs)
    if total_w == 0:
        return 0.0
    return sum(ipc * w for (ipc, w) in pairs) / total_w

vanilla_avg = [weighted_avg(vanilla_data[a]) for a in apps]
fused_avg   = [weighted_avg(fused_data[a])   for a in apps]

# --- add geometric means of the weighted averages ---
vanilla_gm = geometric_mean(vanilla_avg)
fused_gm   = geometric_mean(fused_avg)
apps.append("GEOMEAN")
vanilla_avg.append(vanilla_gm)
fused_avg.append(fused_gm)

# --- print summary ---
print("\nSummary of Weighted Average IPCs:\n")
print(f"{'App':<20} {'Vanilla':>10} {'Fused':>10} {'Change':>10}")
print("-" * 45)
for app, v, f in zip(apps, vanilla_avg, fused_avg):
    pct = 100 * (f - v) / v if v != 0 else 0.0
    color = "green" if pct > 0 else "red" if pct < 0 else "yellow"
    change = colored(f"{pct:+.2f}%", color)
    print(f"{app:<20} {v:>10.3f} {f:>10.3f} {change:>10}")

# --- plot ---
x = list(range(len(apps)))
width = 0.35

plt.figure(figsize=(12, 6))
plt.bar([i - width/2 for i in x], vanilla_avg, width, label='Vanilla')
bars = plt.bar([i + width/2 for i in x], fused_avg, width, label='Fused')

# Add % change labels on fused bars
for i, (v, f) in enumerate(zip(vanilla_avg, fused_avg)):
    pct = 100 * (f - v) / v if v != 0 else 0.0
    label = f"{pct:+.2f}%"
    color = 'green' if pct > 0 else 'red' if pct < 0 else 'orange'
    plt.text(i + width/2, f + 0.01, label, ha='center', va='bottom', color=color, fontsize=9)

plt.xticks(x, apps, rotation=45, ha='right')
plt.ylabel('Weighted Average IPC')
plt.title('Vanilla vs Fused Weighted IPC per App for Datacenter applications')
plt.legend()
plt.tight_layout()

outpath = os.path.join(record_dir, 'ipc_comparison_weighted_histogram.png')
plt.savefig(outpath)
plt.show()
