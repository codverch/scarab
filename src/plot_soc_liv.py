import os
import re
import matplotlib.pyplot as plt
import numpy as np

# Directories containing IPC files
baseline_dir = '/users/deepmish/scarab/src/baseline'
ideal_fusion_dir = '/users/deepmish/scarab/src/ideal_fusion'

# Extract IPC value from a given file
def extract_ipc(file_path):
    if not os.path.exists(file_path):
        print(f"File not found: {file_path}")
        return None
    try:
        with open(file_path, 'r') as file:
            content = file.read()
            match = re.search(r'(\d+\.\d+) IPC \((\d+\.\d+) IPC\)', content)
            if match:
                return float(match.group(2))  # Extract the IPC in parentheses
    except Exception as e:
        print(f"Error reading {file_path}: {e}")
    print(f"Could not find IPC pattern in {file_path}")
    return None

# Filter IPC files and applications
baseline_files = [f for f in os.listdir(baseline_dir) if f.endswith('_ipc.txt')]
ideal_fusion_files = [f for f in os.listdir(ideal_fusion_dir) if f.endswith('_ipc.txt')]

baseline_apps = [f.replace('baseline_', '').replace('_ipc.txt', '') for f in baseline_files]
ideal_fusion_apps = [f.replace('ideal_fusion_', '').replace('_ipc.txt', '') for f in ideal_fusion_files]

common_apps = sorted(set(baseline_apps).intersection(ideal_fusion_apps))
display_names = {app: f"{app.upper()}-SOC-LJ" for app in common_apps}

# Collect IPC values
valid_apps, baseline_ipcs, ideal_fusion_ipcs = [], [], []

for app in common_apps:
    base_path = os.path.join(baseline_dir, f'baseline_{app}_ipc.txt')
    ideal_path = os.path.join(ideal_fusion_dir, f'ideal_fusion_{app}_ipc.txt')

    base_ipc = extract_ipc(base_path)
    ideal_ipc = extract_ipc(ideal_path)

    if base_ipc is not None and ideal_ipc is not None:
        valid_apps.append(app)
        baseline_ipcs.append(base_ipc)
        ideal_fusion_ipcs.append(ideal_ipc)

# Normalize IPC values
common_apps = valid_apps
normalized_baseline = [1.0] * len(baseline_ipcs)
normalized_ideal = [ideal_fusion_ipcs[i] / baseline_ipcs[i] for i in range(len(baseline_ipcs))]

# Plot settings
plt.rcParams['font.family'] = 'serif'
fig, ax = plt.subplots(figsize=(12, 6.5))
fig.patch.set_facecolor('white')
ax.set_facecolor('#f9f9f9')

# Bar placement
index = np.arange(len(common_apps))
bar_width = 0.35

baseline_bars = ax.bar(index - bar_width / 2, normalized_baseline, bar_width,
                       label='Baseline', color='#2f8e89', alpha=0.9)
ideal_bars = ax.bar(index + bar_width / 2, normalized_ideal, bar_width,
                    label='Ideal Fusion', color='#59c959', alpha=0.9)

# Axis labels and ticks
ax.set_xlabel('Datacenter Applications', fontsize=12)
ax.set_ylabel('Normalized IPC', fontsize=12)
ax.set_title('Normalized IPC Comparison: Baseline vs Ideal Fusion (Distance = 352 + Store dependence checks)', fontsize=14)
ax.set_xticks(index)
ax.set_xticklabels([display_names[app] for app in common_apps], rotation=45, ha='right')

# Add numerical labels on bars
def add_labels(bars):
    for bar in bars:
        height = bar.get_height()
        ax.text(bar.get_x() + bar.get_width()/2, height + 0.03,
                f'{height:.2f}', ha='center', va='bottom', fontsize=9, fontweight='bold')

add_labels(baseline_bars)
add_labels(ideal_bars)

# Annotate % improvement above Ideal Fusion bars
for i in range(len(baseline_ipcs)):
    improvement = ((ideal_fusion_ipcs[i] - baseline_ipcs[i]) / baseline_ipcs[i]) * 100
    y = normalized_ideal[i] + 0.17
    ax.text(index[i] + bar_width / 2, y, f'{improvement:+.1f}%',
            ha='center', va='bottom', fontsize=9,
            color='green' if improvement >= 0 else 'red', fontweight='bold')

# Add reference line at baseline
ax.axhline(y=1.0, color='#666666', linestyle='--', linewidth=0.8, alpha=0.5)

# Tweak axes and grid
ax.set_ylim(0, max(normalized_ideal) * 1.3)
ax.grid(True, axis='y', linestyle='--', alpha=0.3)
ax.spines['top'].set_visible(False)
ax.spines['right'].set_visible(False)
ax.spines['bottom'].set_linewidth(0.5)
ax.spines['left'].set_linewidth(0.5)

# Legend
ax.legend(loc='upper right', frameon=True, framealpha=0.95,
          edgecolor='#dddddd', facecolor='white', fontsize=11)

# Save and show
plt.tight_layout()
plt.savefig('ipc_comparison.png', dpi=300, bbox_inches='tight', facecolor='white')
plt.show()

# Terminal summary
print("\nIPC Comparison Summary:")
print("=" * 85)
print(f"{'Benchmark-Workload':<25} {'Baseline IPC':<15} {'Ideal Fusion IPC':<20} {'Normalized':<15} {'Improvement %':<15}")
print("-" * 85)
for i, app in enumerate(common_apps):
    norm = normalized_ideal[i]
    impr = ((ideal_fusion_ipcs[i] - baseline_ipcs[i]) / baseline_ipcs[i]) * 100
    print(f"{display_names[app]:<25} {baseline_ipcs[i]:<15.2f} {ideal_fusion_ipcs[i]:<20.2f} {norm:<15.2f} {impr:+.2f}%")
print("=" * 85)
print(f"Comparison includes {len(common_apps)} applications: {', '.join(common_apps)}")
print("Plot saved as 'ipc_comparison.png'")
