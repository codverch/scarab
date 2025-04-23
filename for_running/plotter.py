import os
import re
import argparse
import matplotlib.pyplot as plt
from collections import defaultdict
from statistics import geometric_mean
import sys

from termcolor import colored

# --- argument parsing ---
parser = argparse.ArgumentParser()
parser.add_argument("--record-dir", default="records", help="Directory containing result .txt files")
args = parser.parse_args()
record_dir = args.record_dir

# --- prepare regex and containers ---
ipc_pattern = re.compile(r"--\s*([\d.]+)\s*IPC")
vanilla_ipcs = defaultdict(list)
fused_ipcs = defaultdict(list)

# --- gather IPC data ---
if not os.path.isdir(record_dir):
    print(f"Error: directory '{record_dir}' not found.", file=sys.stderr)
    sys.exit(1)

files_found = False
for fname in os.listdir(record_dir):
    if not fname.endswith('.txt'):
        continue
    fullpath = os.path.join(record_dir, fname)
    files_found = True
    if '_vanilla.txt' in fname:
        app = fname.split('_vanilla.txt')[0]
        target = vanilla_ipcs
    elif '_fused.txt' in fname:
        app = fname.split('_fused.txt')[0]
        target = fused_ipcs
    else:
        print(f"Warning: Unrecognized file pattern '{fname}'", file=sys.stderr)
        continue
    with open(fullpath, 'r') as f:
        matches = ipc_pattern.findall(f.read())
        if not matches:
            print(f"Warning: No IPC found in '{fname}'", file=sys.stderr)
        for val in matches:
            target[app].append(float(val))

if not files_found:
    print("Error: No .txt files found.", file=sys.stderr)
    sys.exit(1)

apps = sorted(set(vanilla_ipcs) & set(fused_ipcs))
if not apps:
    print("Error: No apps with both vanilla and fused data.", file=sys.stderr)
    sys.exit(1)

vanilla_avg = [sum(vanilla_ipcs[a]) / len(vanilla_ipcs[a]) for a in apps]
fused_avg = [sum(fused_ipcs[a]) / len(fused_ipcs[a]) for a in apps]

# --- add geometric means ---
vanilla_gm = geometric_mean(vanilla_avg)
fused_gm = geometric_mean(fused_avg)

apps.append("GEOMEAN")
vanilla_avg.append(vanilla_gm)
fused_avg.append(fused_gm)

# --- print summary ---
print("\nSummary of Average IPCs:\n")
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
plt.ylabel('Average IPC')
plt.title('Vanilla vs Fused IPC per App')
plt.legend()
plt.tight_layout()

outpath = os.path.join(record_dir, 'ipc_comparison_histogram.png')
plt.savefig(outpath)
plt.show()
