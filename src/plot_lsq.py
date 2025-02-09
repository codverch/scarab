import matplotlib.pyplot as plt
import numpy as np
import re

# Read and process data
input_file = "agg_results.txt"
data = {}
try:
    with open(input_file, "r") as f:
        lines = f.readlines()
except FileNotFoundError:
    print(f"Error: The file {input_file} does not exist.")
    exit(1)

# Process the file to extract data
workload_configs = {}
all_ipcs = []  # To calculate overall average
for line in lines[1:]:  # Skip header
    parts = line.strip().split()
    if len(parts) != 5:
        print(f"Skipping invalid line: {line.strip()}")
        continue
    workload, lq_sq, total_insts, total_cycles, ipc = parts
    ipc = float(ipc)
    all_ipcs.append(ipc)  # Add to list for overall average
    match = re.search(r"LQ(\d+)_SQ(\d+)", lq_sq)
    if not match:
        print(f"Skipping invalid LQ_SQ format: {lq_sq}")
        continue
    lq, sq = map(int, match.groups())
    config_key = f'LQ{lq}_SQ{sq}'
    if workload not in workload_configs:
        workload_configs[workload] = {}
    workload_configs[workload][config_key] = ipc

# Get unique configurations across all workloads and sort them
all_configs = sorted(
    list(set(sum([list(w.keys()) for w in workload_configs.values()], []))),
    key=lambda x: tuple(map(int, re.findall(r'\d+', x)))
)
workloads = sorted(workload_configs.keys())

# Create figure for grouped bar chart
plt.figure(figsize=(15, 8))
ax = plt.gca()

# Define bar width and positions
bar_width = 0.15
indices = np.arange(len(workloads))

# Define pastel colors for the bars (using lighter shades)
colors = ['#B5E7A0',  # Light Green
         '#FAD02E',   # Pastel Yellow
         '#F28D35',   # Pastel Orange
         '#D8B4A6',   # Light Pastel Pink
         '#A7C7E7',   # Pastel Blue
         '#B1E1DC']   # Light Turquoise

# Store maximum IPC value to set y-axis limit
max_ipc = 0

# Plot data for each configuration
for i, config in enumerate(all_configs):
    ipcs = [workload_configs[workload].get(config, 0) for workload in workloads]
    bar_positions = indices + i * bar_width
    ax.bar(bar_positions, ipcs, bar_width, label=config, color=colors[i % len(colors)])  # Recycle colors if more configs than colors
    
    # Add IPC value above each bar
    for j, ipc in enumerate(ipcs):
        ax.text(bar_positions[j], ipc + 0.02, f'{ipc:.2f}', 
                ha='center', va='bottom', fontsize=9)
        max_ipc = max(max_ipc, ipc)
    
    # Add IPC increase factor between consecutive bars within each workload
    if i > 0:
        prev_ipcs = [workload_configs[workload].get(all_configs[i - 1], 0) for workload in workloads]
        for j, (prev_ipc, ipc) in enumerate(zip(prev_ipcs, ipcs)):
            if prev_ipc > 0:
                ipc_ratio = ipc / prev_ipc
                # Alternate vertical position for each configuration within a workload
                vertical_offset = 0.1 if i % 2 == 1 else -0.1
                y_pos = ipc + vertical_offset
                
                ax.text(bar_positions[j], y_pos, f'x{ipc_ratio:.2f}', 
                        ha='center', 
                        va='bottom' if i % 2 == 1 else 'top',
                        fontsize=9, 
                        color='#FF4500',  # Bright orange-red for contrast
                        bbox=dict(facecolor='white', 
                                edgecolor='none', 
                                alpha=0.7,
                                pad=1))

# Set labels and title
ax.set_xlabel('Datacenter Applications', fontsize=14)
ax.set_ylabel('Instructions Per Cycle (IPC)', fontsize=14)
ax.set_title('IPC Comparison by Scaling up LQ/SQ Configurations for Datacenter Applications', fontsize=16)

# Set y-axis limit with some padding
ax.set_ylim(0, max_ipc * 1.2)

# Set x-axis ticks and labels
ax.set_xticks(indices + bar_width * (len(all_configs) - 1) / 2)
ax.set_xticklabels(workloads, rotation=45, ha='right', fontsize=12)

# Add legend
ax.legend(title="Configurations (LQ/SQ)", fontsize=12)

# Add grid for better readability
ax.grid(True, which='both', linestyle='--', linewidth=0.5, alpha=0.3)  # Made grid lighter

# Set background color to very light gray
ax.set_facecolor('#f8f8f8')

# Adjust layout to prevent label cutoff
plt.tight_layout()

# Save and show the plot
plt.savefig('ipc_comparison_lsq.png', dpi=300, bbox_inches='tight')
plt.show()

print("IPC comparison plot saved as ipc_comparison_lsq.png")

# Calculate and print overall average IPC
overall_avg_ipc = sum(all_ipcs) / len(all_ipcs)
print(f"Overall average IPC: {overall_avg_ipc:.2f}")
