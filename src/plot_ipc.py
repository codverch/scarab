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

for line in lines:
    parts = line.strip().split()
    if len(parts) != 5:
        continue
    
    workload, rob, total_insts, total_cycles, ipc = parts
    ipc = float(ipc)
    all_ipcs.append(ipc)  # Add to list for overall average
    
    match = re.search(r"ROB(\d+)", rob)
    if not match:
        print(f"Skipping invalid ROB format: {rob}")
        continue
    
    rob_value = int(match.group(1))  # Extract and convert ROB value
    
    config_key = f'ROB{rob_value}'
    
    if workload not in workload_configs:
        workload_configs[workload] = {}
    
    workload_configs[workload][config_key] = ipc

# Get unique configurations across all workloads and sort them
all_configs = sorted(list(set(sum([list(w.keys()) for w in workload_configs.values()], []))), 
                     key=lambda x: tuple(map(int, re.findall(r'\d+', x))))
workloads = sorted(list(workload_configs.keys()))

# Create figure for grouped bar chart
fig, ax = plt.subplots(figsize=(15, 8))

# Define bar width and positions
bar_width = 0.15
indices = np.arange(len(workloads))

# Plot data for each configuration
for i, config in enumerate(all_configs):
    ipcs = [workload_configs[workload].get(config, 0) for workload in workloads]
    bar_positions = indices + i * bar_width
    ax.bar(bar_positions, ipcs, bar_width, label=config)
    
    # Add IPC value above the bar
    for j, ipc in enumerate(ipcs):
        ax.text(bar_positions[j], ipc + 0.01, f'{ipc:.2f}', ha='center', va='bottom', fontsize=9)

# Set labels and title
ax.set_xlabel('Datacenter Applications', fontsize=14)
ax.set_ylabel('Instructions Per Cycle (IPC)', fontsize=14)
ax.set_title('IPC Comparison by Scaling up ROB Configurations for Datacenter Applications', fontsize=16)
ax.set_xticks(indices + bar_width * (len(all_configs) - 1) / 2)
ax.set_xticklabels(workloads, rotation=45, ha='right', fontsize=12)
ax.legend(title="Configurations (ROB)", fontsize=12)

# Add grid for better readability
ax.grid(True, which='both', linestyle='--', linewidth=0.5)

# Adjust layout to prevent label cutoff
plt.tight_layout()

# Save and show the plot
plt.savefig('ipc_comparison_rob.png', dpi=300, bbox_inches='tight')
plt.show()

print("IPC comparison plot saved as ipc_comparison_rob.png")
