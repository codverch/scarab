import os
import re
from collections import defaultdict
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import Ellipse
import matplotlib.patheffects as path_effects

# Directory containing the result files
dir_path = "/users/deepmish/scarab/src/unique_cacheblock_pairs_fusion_candidates"

# List of result files to process
files = ["clang.txt", "gcc.txt", "mongodb.txt", "mysql.txt", "postgres.txt"]

# Create the plot with a clean aesthetic
plt.figure(figsize=(10, 7))
plt.rcParams.update({
    'font.family': 'serif',
    'font.size': 12,
    'text.usetex': False,
    'axes.labelsize': 14,
    'axes.titlesize': 16,
    'xtick.labelsize': 12,
    'ytick.labelsize': 12,
    'legend.fontsize': 12,
    'figure.titlesize': 18
})

# Set the background color
plt.gcf().set_facecolor('white')
plt.gca().set_facecolor('white')

# Define spring-inspired colors for regions
hot_color = "#ff7e79"        # Cherry blossom pink
warm_color = "#ffd166"       # Daffodil yellow
cold_color = "#78c6f7"       # Bluebell blue

# Create region ellipses with more subtle appearance but spring colors
hot_ellipse = Ellipse(xy=(30, 60), width=60, height=80, alpha=0.35, color=hot_color, zorder=0)
warm_ellipse = Ellipse(xy=(70, 85), width=30, height=30, alpha=0.35, color=warm_color, zorder=0)
cold_ellipse = Ellipse(xy=(92, 95), width=16, height=12, alpha=0.35, color=cold_color, zorder=0)

# Add ellipses to the plot
plt.gca().add_patch(hot_ellipse)
plt.gca().add_patch(warm_ellipse)
plt.gca().add_patch(cold_ellipse)

# Add region labels with nice typography - spring-inspired text colors
hot_text = plt.text(30, 60, "hot", fontsize=22, ha='center', va='center', 
                   fontweight='bold', color='#d84c4c', zorder=10)
warm_text = plt.text(70, 85, "warm", fontsize=18, ha='center', va='center', 
                    fontweight='bold', color='#d4a017', zorder=10)
cold_text = plt.text(92, 95, "cold", fontsize=16, ha='center', va='center', 
                    fontweight='bold', color='#4682B4', zorder=10)

# Add path effects to make text more visible
for text in [hot_text, warm_text, cold_text]:
    text.set_path_effects([path_effects.withStroke(linewidth=3, foreground='white')])

# Define colors and line styles for each benchmark
colors = ['#4e79a7', '#f28e2b', '#76b7b2', '#e15759', '#59a14f']
line_styles = ['-', '-', '-', '-', '-']  # Use solid lines for cleaner look
markers = ['o', 's', '^', 'D', 'P']

# Process each file and create curves
for i, file_name in enumerate(files):
    file_path = os.path.join(dir_path, file_name)
    benchmark_name = file_name.split('.')[0]
    
    if not os.path.exists(file_path):
        print(f"Skipping {file_name} - file not found")
        continue
    
    print(f"Processing {file_name}...")
    
    # Track PC pairs and their counts
    pc_pair_counter = defaultdict(int)
    cacheblock_pc_pairs = defaultdict(set)
    
    # Read the file and extract PC pairs and cacheline addresses
    with open(file_path, 'r') as file:
        for line in file:
            line = line.strip()
            if line.startswith('Micro-op 1:'):
                match = re.match(r'Micro-op 1:\s+(\w+)\s+Micro-op 2:\s+(\w+)\s+Cacheblock Address:\s+(\w+)', line)
                if match:
                    uop1 = match.group(1)
                    uop2 = match.group(2)
                    cacheblock = match.group(3)
                    pc_pair = (uop1, uop2)
                    pc_pair_counter[pc_pair] += 1
                    cacheblock_pc_pairs[cacheblock].add(pc_pair)
    
    # Find unique PC pairs (those that appear exactly once)
    unique_pc_pairs = {pc for pc, count in pc_pair_counter.items() if count == 1}
    total_unique = len(unique_pc_pairs)
    
    if total_unique == 0:
        print(f"No unique PC pairs found in {file_name}")
        continue
    
    # Count unique PC pairs per cacheline
    cacheblock_counts = {}
    for cb, pairs in cacheblock_pc_pairs.items():
        cnt = sum(1 for p in pairs if p in unique_pc_pairs)
        if cnt > 0:
            cacheblock_counts[cb] = cnt
    
    # Sort cachelines by count (descending)
    sorted_cachelines = sorted(cacheblock_counts.items(), key=lambda x: -x[1])
    
    # Calculate cumulative sums
    cumulative = []
    current = 0
    
    for _, cnt in sorted_cachelines:
        current += cnt
        cumulative.append(current)
    
    total_cachelines = len(sorted_cachelines)
    if total_cachelines == 0:
        print(f"No cachelines with unique PC pairs found in {file_name}")
        continue
    
    # Calculate percentages for x and y axes
    x = [(i+1) / total_cachelines * 100 for i in range(total_cachelines)]
    y = [c / total_unique * 100 for c in cumulative]
    
    # Get the super hot threshold (top 1% of cachelines)
    super_hot_index = max(1, int(total_cachelines * 0.01))
    super_hot_x = x[super_hot_index-1]
    super_hot_y = y[super_hot_index-1]
    
    # Get the hot threshold (top 15% of cachelines)
    hot_index = max(1, int(total_cachelines * 0.15))
    hot_x = x[hot_index-1]
    hot_y = y[hot_index-1]
    
    # Get the warm threshold (top 50% of cachelines)
    warm_index = max(1, int(total_cachelines * 0.50))
    warm_x = x[warm_index-1]
    warm_y = y[warm_index-1]
    
    # Plot the curve with cleaner styling
    plt.plot(x, y, label=benchmark_name, 
             color=colors[i % len(colors)], 
             linestyle=line_styles[i % len(line_styles)], 
             linewidth=2.5, 
             marker=markers[i % len(markers)], 
             markevery=max(1, len(x)//15),  # Fewer markers for cleaner look
             markersize=7)
    
    # Highlight the super hot point with a larger marker
    plt.scatter([super_hot_x], [super_hot_y], color=colors[i % len(colors)], 
                s=120, edgecolor='white', linewidth=1.5, zorder=10)
    
    # Print temperature category statistics
    print(f"\nTemperature Statistics for {benchmark_name}:")
    print(f"Total unique cachelines: {total_cachelines}")
    print(f"Total unique PC pairs: {total_unique}")
    print("-" * 50)
    print(f"Super Hot (top 1%):")
    print(f"  Cachelines: {super_hot_index} ({super_hot_index/total_cachelines*100:.1f}% of total)")
    print(f"  Unique PC pairs: {int(super_hot_y * total_unique / 100)} ({super_hot_y:.1f}% of total)")
    print("-" * 50)
    print(f"Hot (1-15%):")
    print(f"  Cachelines: {hot_index - super_hot_index} ({(hot_index - super_hot_index)/total_cachelines*100:.1f}% of total)")
    print(f"  Unique PC pairs: {int((hot_y - super_hot_y) * total_unique / 100)} ({(hot_y - super_hot_y):.1f}% of total)")
    print("-" * 50)
    print(f"Warm (15-50%):")
    print(f"  Cachelines: {warm_index - hot_index} ({(warm_index - hot_index)/total_cachelines*100:.1f}% of total)")
    print(f"  Unique PC pairs: {int((warm_y - hot_y) * total_unique / 100)} ({(warm_y - hot_y):.1f}% of total)")
    print("-" * 50)
    print(f"Cold (50-100%):")
    print(f"  Cachelines: {total_cachelines - warm_index} ({(total_cachelines - warm_index)/total_cachelines*100:.1f}% of total)")
    print(f"  Unique PC pairs: {int((100 - warm_y) * total_unique / 100)} ({(100 - warm_y):.1f}% of total)")
    print("=" * 80)

# Set axis labels and title
plt.xlabel('Unique cachelines (%)', fontweight='bold')
plt.ylabel('Non-repeating load micro-op PC pairs (%)', fontweight='bold')
plt.title('Distribution of Load Micro-op PC Pairs Across Cachelines', fontweight='bold', pad=20)

# Add annotation for super hot points - moved down closer to zero and right
plt.annotate("Super Hot points (1%)", xy=(8, 15), 
             xycoords='data', fontsize=10, fontweight='bold', color='#d84c4c',
             bbox=dict(boxstyle="round,pad=0.3", fc="white", ec="#d84c4c", alpha=0.8))

# Add legend with improved styling
plt.legend(loc='lower right', frameon=True, facecolor='white', 
           edgecolor='lightgray', framealpha=0.9, 
           title="Benchmarks", title_fontsize=13)

# Set axis limits
plt.xlim(0, 100)
plt.ylim(0, 100)

# Add grid but make it more subtle
plt.grid(True, linestyle='--', alpha=0.2, zorder=0)

# Save the figure
output_path = os.path.join(dir_path, "unique_pairs_analysis.png")
plt.savefig(output_path, dpi=300, bbox_inches='tight')
print(f"Figure saved to: {output_path}")

# Show the plot
plt.show()