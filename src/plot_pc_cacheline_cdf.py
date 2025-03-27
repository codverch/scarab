import os
import re
from collections import defaultdict
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import Rectangle
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

# Set plot margins to avoid clipping
plt.subplots_adjust(left=0.1, right=0.95, top=0.9, bottom=0.1)

# Set the background color
plt.gcf().set_facecolor('white')
plt.gca().set_facecolor('white')

# Define colors for regions with exactly specified hex codes
super_hot_color = "#C41230"  # Exact dark red for super hot (0-1%)
hot_color = "#e57373"        # Pink for hot (1-15%)
warm_color = "#f9d62e"       # Exact specified yellow for warm (15-50%)
cold_color = "#89c3f3"       # Exact specified blue for cold (50-100%)

# Define the regions as percentages
super_hot_range = (0, 1)
hot_range = (1, 15)
warm_range = (15, 50)
cold_range = (50, 100)

# Create rectangular patches for each temperature region
# Each rectangle spans from the start to end percentage on x-axis, and full height on y-axis
super_hot_rect = Rectangle(xy=(super_hot_range[0], 0), width=super_hot_range[1]-super_hot_range[0], 
                          height=100, alpha=0.35, color=super_hot_color, zorder=0)
hot_rect = Rectangle(xy=(hot_range[0], 0), width=hot_range[1]-hot_range[0], 
                     height=100, alpha=0.2, color=hot_color, zorder=0)
warm_rect = Rectangle(xy=(warm_range[0], 0), width=warm_range[1]-warm_range[0], 
                     height=100, alpha=0.2, color=warm_color, zorder=0)
cold_rect = Rectangle(xy=(cold_range[0], 0), width=cold_range[1]-cold_range[0], 
                     height=100, alpha=0.2, color=cold_color, zorder=0)

# Add rectangles to the plot
plt.gca().add_patch(super_hot_rect)
plt.gca().add_patch(hot_rect)
plt.gca().add_patch(warm_rect)
plt.gca().add_patch(cold_rect)

# Create annotation for super hot with a slightly diagonal arrow
plt.annotate('super hot', 
             xy=(0.8, 85),  # Arrow tip position (slightly below text for diagonal effect)
             xytext=(3, 90),  # Text position at top
             fontsize=14,
             fontweight='bold',
             color='#C41230',
             arrowprops=dict(facecolor='#C41230', 
                            shrink=0.02,  # Small shrink for a tighter arrow
                            width=1.5,    # Thin arrow
                            headwidth=6,  # Small arrowhead
                            headlength=4, # Short arrowhead
                            alpha=0.8,
                            connectionstyle="arc3,rad=-0.05"),  # Slight curve for aesthetic
             horizontalalignment='left',
             verticalalignment='center',
             zorder=15)

hot_text = plt.text(8, 30,  # Position moved much lower in the hot region
                   "hot", fontsize=16, ha='center', va='center', 
                   fontweight='bold', color='#c62828', zorder=10)
# Update text colors to match the regions
warm_text = plt.text(32, 65,  # Position in the middle of warm region
                    "warm", fontsize=16, ha='center', va='center', 
                    fontweight='bold', color='#b8860b', zorder=10)  # Darker gold for better visibility
cold_text = plt.text(75, 85,  # Position in the middle of cold region
                    "cold", fontsize=16, ha='center', va='center', 
                    fontweight='bold', color='#0d47a1', zorder=10)  # Keep dark blue for good contrast

# Add path effects to make text more visible
for text in [hot_text, warm_text, cold_text]:
    text.set_path_effects([path_effects.withStroke(linewidth=4, foreground='white')])
    text.set_fontweight('bold')

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
    cumulative = [0]
    current = 0
    
    for _, cnt in sorted_cachelines:
        current += cnt
        cumulative.append(current)
    
    total_cachelines = len(sorted_cachelines)
    if total_cachelines == 0:
        print(f"No cachelines with unique PC pairs found in {file_name}")
        continue
    
    # Calculate percentages for x and y axes
    x = [0] + [(i+1) / total_cachelines * 100 for i in range(total_cachelines)]
    y = [c / total_unique * 100 for c in cumulative]
    
    # Get the super hot threshold (top 1% of cachelines)
    super_hot_index = max(1, int(total_cachelines * 0.01))
    super_hot_x = x[super_hot_index]
    super_hot_y = y[super_hot_index]
    
    # Get the hot threshold (top 15% of cachelines)
    hot_index = max(1, int(total_cachelines * 0.15))
    hot_x = x[hot_index]
    hot_y = y[hot_index]
    
    # Get the warm threshold (top 50% of cachelines)
    warm_index = max(1, int(total_cachelines * 0.50))
    warm_x = x[warm_index]
    warm_y = y[warm_index]
    
    # Plot the curve with cleaner styling
    plt.plot(x, y, label=benchmark_name, 
             color=colors[i % len(colors)], 
             linestyle=line_styles[i % len(line_styles)], 
             linewidth=2.5, 
             marker=markers[i % len(markers)], 
             markevery=max(1, len(x)//15),  # Fewer markers for cleaner look
             markersize=7)
    
    # Highlight the key points with larger markers and darker outline
    plt.scatter([super_hot_x], [super_hot_y], color=colors[i % len(colors)], 
                s=120, edgecolor='black', linewidth=1.5, zorder=10)
    plt.scatter([hot_x], [hot_y], color=colors[i % len(colors)], 
                s=100, edgecolor='black', linewidth=1.5, zorder=10)
    plt.scatter([warm_x], [warm_y], color=colors[i % len(colors)], 
                s=100, edgecolor='black', linewidth=1.5, zorder=10)
    
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

# Add vertical lines to clearly mark the boundaries between regions
plt.axvline(x=super_hot_range[1], color='black', linestyle='--', linewidth=1.2, alpha=0.6)
plt.axvline(x=hot_range[1], color='black', linestyle='--', linewidth=1.2, alpha=0.6)
plt.axvline(x=warm_range[1], color='black', linestyle='--', linewidth=1.2, alpha=0.6)

# Set axis labels and title
plt.xlabel('Unique cachelines (%)', fontweight='bold')
plt.ylabel('Non-repeating load micro-op PC pairs (%)', fontweight='bold')
plt.title('Distribution of Load Micro-op PC Pairs Across Cachelines', fontweight='bold', pad=20)

# Add a simple legend without fancy styling
legend = plt.legend(loc='lower right', frameon=True, facecolor='white', 
           edgecolor='lightgray', framealpha=0.7,
           title="Benchmarks", title_fontsize=13)
# Bold the legend title
legend.get_title().set_fontweight('bold')

# Set axis limits
plt.xlim(0, 100)
plt.ylim(0, 100)

# Add grid but make it more subtle
plt.grid(True, linestyle='--', alpha=0.2, zorder=0)

# Save the figure
output_path = os.path.join(dir_path, "unique_pairs_analysis_rectangular.png")
plt.savefig(output_path, dpi=300, bbox_inches='tight')
print(f"Figure saved to: {output_path}")

# Show the plot
plt.show()