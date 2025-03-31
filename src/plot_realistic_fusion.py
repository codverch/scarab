#!/usr/bin/env python3
import os
import re
import matplotlib.pyplot as plt
import numpy as np
import matplotlib.patches as patches

# Base directory
base_dir = "/users/deepmish/scarab/src/realistic_fusion_distance"

# Directories to search with clean labels for plotting
directory_map = {
    "no_fusion": "No Fusion",
    "distance_1_to_20": "1-20",
    "distance_21_to_50": "21-50",
    "distance_51_to_100": "51-100",
    "distance_greater_than_100": ">100",
    "distance_no_restriction": "No Restriction"
}

# Categorize the ranges (excluding No Fusion)
range_categories = {
    "1-20": "Very Short Distance",
    "21-50": "Short Distance",
    "51-100": "Medium Distance",
    ">100": "Long Distance",
    "No Restriction": "Long Distance"
}

# Regular expression to extract IPC from the specific format
ipc_pattern = re.compile(r'--\s*([\d.]+)\s*IPC\s*\([\d.]+\s*IPC\)')

# Dictionary to store results
results = {}

# Extract IPC values
for directory, label in directory_map.items():
    full_path = os.path.join(base_dir, directory)
    
    # Check if directory exists
    if not os.path.isdir(full_path):
        print(f"{directory}: Not a valid directory")
        continue
    
    # List files
    files = os.listdir(full_path)
    if not files:
        print(f"{directory}: No files found")
        continue
    
    # Process each file
    for filename in files:
        file_path = os.path.join(full_path, filename)
        if os.path.isfile(file_path):
            try:
                # Search for IPC in the file
                with open(file_path, 'r') as f:
                    content = f.read()
                    match = ipc_pattern.search(content)
                    if match:
                        ipc_value = float(match.group(1))
                        results[label] = ipc_value
                        print(f"{label}: IPC = {ipc_value}")
                    else:
                        print(f"{label}: No IPC value found in expected format")
            except Exception as e:
                print(f"Error reading {filename} in {label}: {e}")

# Sort the results for visualization
# Make sure No Fusion is first, then ordered by distance ranges
preferred_order = ["No Fusion", "1-20", "21-50", "51-100", ">100", "No Restriction"]
sorted_labels = [label for label in preferred_order if label in results]
sorted_values = [results[label] for label in sorted_labels]

# Normalize values to baseline if No Fusion exists
baseline_value = 1.0
if "No Fusion" in results:
    baseline = results["No Fusion"]
    normalized_values = [value/baseline for value in sorted_values]
else:
    normalized_values = sorted_values

# Setup the figure with enhanced aesthetics
plt.figure(figsize=(14, 8))
ax = plt.subplot(111)

# Create background zones - only apply to the distance ranges (excluding No Fusion)
# First determine the position of No Fusion
no_fusion_idx = sorted_labels.index("No Fusion") if "No Fusion" in sorted_labels else -1

# Group the labels into zones
zone_categories = {
    "Very Short Distance": ["1-20"],
    "Short Distance": ["21-50"],
    "Medium Distance": ["51-100"],
    "Long Distance": [">100", "No Restriction"]
}

# Colors for the zones
zone_colors = {
    "Very Short Distance": "#FFF9C4",  # Light yellow
    "Short Distance": "#DCEDC8",       # Light green
    "Medium Distance": "#E1E1E1",      # Light gray
    "Long Distance": "#FFCCBC"         # Light coral
}

# Determine zone ranges in the plot
zones = []
for zone_name, labels in zone_categories.items():
    zone_indices = [sorted_labels.index(label) for label in labels if label in sorted_labels]
    if zone_indices:
        zones.append({
            "name": zone_name,
            "color": zone_colors[zone_name],
            "range": (min(zone_indices), max(zone_indices) + 1)
        })

# Colors for the bars that match the example
bar_colors = [
    "#555555",  # Dark gray for No Fusion
    "#3366CC",  # Blue for 1-20
    "#DC3912",  # Red for 21-50
    "#FF9900",  # Orange for 51-100
    "#109618",  # Green for >100
    "#990099",  # Purple for No Restriction
]

# Add zone backgrounds
for zone in zones:
    rect = patches.Rectangle(
        (zone["range"][0] - 0.4, 0), 
        zone["range"][1] - zone["range"][0], 
        max(normalized_values) * 1.3,
        linewidth=1, 
        edgecolor='none', 
        facecolor=zone["color"],
        alpha=0.5,
        zorder=0
    )
    ax.add_patch(rect)

# Plot bars
bars = plt.bar(
    np.arange(len(sorted_labels)), 
    normalized_values, 
    color=bar_colors[:len(sorted_labels)],
    width=0.6,
    zorder=3
)

# Add normalized values on top of bars
for i, v in enumerate(normalized_values):
    # Format value display
    display_value = f"{v:.2f} (IPC: {sorted_values[i]:.2f})"
    
    plt.text(
        i, 
        v + 0.02, 
        display_value, 
        ha='center', 
        fontsize=11, 
        fontweight='bold',
        zorder=5
    )

# Add zone labels in the middle, properly positioned
for zone in zones:
    mid_x = (zone["range"][0] + zone["range"][1] - 1) / 2
    mid_x += 0.1  # Move the text slightly to the left (adjust the value as needed)
    
    # Adjust the label for "Very Short Distance" to appear on two lines
    label = zone["name"]
    if label == "Very Short Distance":
        label = "Very Short\nDistance"
    
    plt.text(
        mid_x, 
        max(normalized_values) * 1.1,  # Adjust the y-coordinate to move the text up
        label, 
        ha='center', 
        va='center',
        fontsize=14, 
        fontweight='bold', 
        color='#333333',
        zorder=5
    )

# Set the x-axis labels
plt.xticks(np.arange(len(sorted_labels)), sorted_labels, fontsize=12, fontweight='bold')

# Add descriptive labels and title
plt.xlabel('Distance Between Load Micro-Op PC Pairs Accessing Same (Cacheblock, Memory Size, Base Register)', fontsize=14, fontweight='bold', labelpad=15)
plt.ylabel('Normalized Performance (Relative to No Fusion)', fontsize=14, fontweight='bold', labelpad=15)
plt.title('Impact of Fusion Distance on Performance - MongoDB', fontsize=16, fontweight='bold', pad=20)

# Add grid for readability
plt.grid(axis='y', linestyle='--', alpha=0.3, zorder=1)

# Find and mark the baseline (No Fusion)
if "No Fusion" in results:
    baseline_norm = 1.0  # Normalized baseline is always 1.0
    plt.axhline(y=baseline_norm, color='#CC0000', linestyle='--', alpha=0.8, linewidth=2, zorder=2)
    plt.text(
        len(sorted_labels) - 0.5, 
        baseline_norm + 0.02, 
        f'Baseline', 
        color='#CC0000', 
        fontweight='bold', 
        ha='center',
        zorder=5
    )

# Remove top and right spines
ax.spines['top'].set_visible(False)
ax.spines['right'].set_visible(False)

# Set y limit to have more appropriate space
plt.ylim(0, max(normalized_values) * 1.3)

# Add a subtle background color to the entire plot
ax.set_facecolor('#F8F8F8')



# Improve layout
plt.tight_layout()

# Save the plot
plt.savefig("fusion_distance_performance_comparison.png", dpi=300, bbox_inches='tight')
print("Plot saved as fusion_distance_performance_comparison.png")

# Show plot
plt.show()