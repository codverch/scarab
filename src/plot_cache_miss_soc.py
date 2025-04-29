#!/usr/bin/env python3
import os
import re
import matplotlib.pyplot as plt
import numpy as np

# Directories containing stat files
baseline_dir = '/users/deepmish/scarab/src/baseline'
data_dir = '/users/deepmish/scarab/src/records_with_data'

# Get all applications (subdirectories) in both directories
baseline_apps = [d for d in os.listdir(baseline_dir) if os.path.isdir(os.path.join(baseline_dir, d))]
data_apps = [d for d in os.listdir(data_dir) if os.path.isdir(os.path.join(data_dir, d))]

# Function to extract a metric from a stat file
def extract_metric(file_path, metric_name):
    if not os.path.exists(file_path):
        print(f"File not found: {file_path}")
        return None
    
    try:
        # Read the file content
        with open(file_path, 'r') as f:
            content = f.read()
        
        # Extract the metric
        metric_match = re.search(rf'{metric_name},\s+(\d+\.\d+)', content)
        
        if metric_match:
            metric_value = float(metric_match.group(1))
            print(f"File: {file_path}, {metric_name}: {metric_value:.2f}%")
            return metric_value
        else:
            print(f"Could not find {metric_name} in {file_path}")
            return None
        
    except Exception as e:
        print(f"Error processing {file_path}: {e}")
        return None

# Collect metrics for each application
baseline_rob_stalls = {}
data_rob_stalls = {}
baseline_dcache_miss = {}
data_dcache_miss = {}

# Collect data from records_with_data directory
for app in data_apps:
    # Get ROB stall percentage
    core_path = os.path.join(data_dir, app, 'core.stat.0.csv')
    stall = extract_metric(core_path, 'FULL_WINDOW_STALL_pct')
    
    if stall is not None:
        data_rob_stalls[app] = stall
        print(f"\nApp: {app}")
        print(f"Records_with_data FULL_WINDOW_STALL_pct: {stall:.2f}%")
    
    # Get DCACHE miss percentage
    memory_path = os.path.join(data_dir, app, 'memory.stat.0.csv')
    dcache = extract_metric(memory_path, 'DCACHE_MISS_pct')
    
    if dcache is not None:
        data_dcache_miss[app] = dcache
        print(f"Records_with_data DCACHE_MISS_pct: {dcache:.2f}%")

# Collect data from baseline directory
for app in baseline_apps:
    # Get ROB stall percentage - note the app prefix in filename
    core_path = os.path.join(baseline_dir, app, f'{app}_core.stat.0.csv')
    stall = extract_metric(core_path, 'FULL_WINDOW_STALL_pct')
    
    if stall is not None:
        baseline_rob_stalls[app] = stall
        print(f"\nApp: {app}")
        print(f"Baseline FULL_WINDOW_STALL_pct: {stall:.2f}%")
    
    # Get DCACHE miss percentage - note the app prefix in filename
    memory_path = os.path.join(baseline_dir, app, f'{app}_memory.stat.0.csv')
    dcache = extract_metric(memory_path, 'DCACHE_MISS_pct')
    
    if dcache is not None:
        baseline_dcache_miss[app] = dcache
        print(f"Baseline DCACHE_MISS_pct: {dcache:.2f}%")

# Get all apps with at least one metric in either directory
all_apps = sorted(set(
    list(data_rob_stalls.keys()) + list(baseline_rob_stalls.keys()) + 
    list(data_dcache_miss.keys()) + list(baseline_dcache_miss.keys())
))

if not all_apps:
    print("No valid applications found with statistics. Check file paths and CSV format.")
    exit(1)

# Create formatting for display
display_names = {app: f"{app.upper()}-SOC-LJ" for app in all_apps}

# Function to create comparison plot for a metric
def create_comparison_plot(baseline_data, enhanced_data, metric_name, title, ylabel, filename):
    # Create lists for plotting
    plot_apps = []
    plot_baseline = []
    plot_enhanced = []
    
    for app in all_apps:
        # Only include apps where we have at least one value
        if app in baseline_data or app in enhanced_data:
            plot_apps.append(app)
            # Use the value if available, otherwise use 0
            plot_baseline.append(baseline_data.get(app, 0))
            plot_enhanced.append(enhanced_data.get(app, 0))
    
    # Sort by enhanced data value (highest to lowest)
    if plot_apps:
        sorted_indices = sorted(range(len(plot_enhanced)), key=lambda i: plot_enhanced[i], reverse=True)
        
        # Sort all data using these indices
        sorted_apps = [plot_apps[i] for i in sorted_indices]
        sorted_baseline = [plot_baseline[i] for i in sorted_indices]
        sorted_enhanced = [plot_enhanced[i] for i in sorted_indices]
    
    # Create the plot
    plt.rcParams['font.family'] = 'serif'
    fig, ax = plt.subplots(figsize=(14, 8))
    fig.patch.set_facecolor('white')
    ax.set_facecolor('#f9f9f9')
    
    # Bar placement
    index = np.arange(len(sorted_apps))
    bar_width = 0.35
    
    # Create the bars
    baseline_bars = ax.bar(index - bar_width/2, sorted_baseline, bar_width,
                          label='Baseline', color='#2f8e89', alpha=0.9)
    enhanced_bars = ax.bar(index + bar_width/2, sorted_enhanced, bar_width,
                      label='Ideal fusion', color='#59c959', alpha=0.9)
    
    # Add connecting lines between baseline and enhanced bars
    for i in range(len(sorted_apps)):
        base_height = sorted_baseline[i]
        enhanced_height = sorted_enhanced[i]
        
        # Only draw connecting lines if both bars have data
        if base_height > 0 and enhanced_height > 0:
            base_x = index[i] - bar_width/2
            enhanced_x = index[i] + bar_width/2
            
            # Draw a dashed line connecting the tops of the bars
            # Lower is better for both metrics (stalls and cache misses)
            if base_height > enhanced_height:
                color = 'green'  # Green for improvement
            else:
                color = 'red'    # Red for regression
                
            ax.plot([base_x, enhanced_x], [base_height, enhanced_height], 
                    color=color, linestyle='--', linewidth=1.5, alpha=0.7)
    
    # Add axis labels and title
    ax.set_xlabel('Datacenter Applications', fontsize=12)
    ax.set_ylabel(ylabel, fontsize=12)
    ax.set_title(title, fontsize=14, fontweight='bold')
    
    # Set x-ticks and labels
    ax.set_xticks(index)
    ax.set_xticklabels([display_names[app] for app in sorted_apps], rotation=45, ha='right')
    
    # Add value labels on bars
    def add_labels(bars):
        for bar in bars:
            height = bar.get_height()
            if height > 0:  # Only add labels to bars with non-zero height
                ax.text(bar.get_x() + bar.get_width()/2, height + 1.0,
                        f'{height:.1f}%', ha='center', va='bottom', fontsize=9, fontweight='bold')
    
    add_labels(baseline_bars)
    add_labels(enhanced_bars)
    
    # Annotate % improvement where applicable
    for i in range(len(sorted_apps)):
        base_height = sorted_baseline[i]
        enhanced_height = sorted_enhanced[i]
        
        # Only calculate improvement if both values exist and baseline > 0
        if base_height > 0 and enhanced_height > 0:
            improvement = ((base_height - enhanced_height) / base_height) * 100
            y = max(base_height, enhanced_height) + 3.0
            
            # Only show improvement label if it's not too small
            if abs(improvement) > 0.1:  # Small threshold to avoid rounding issues
                ax.text(index[i], y, 
                      f'-{abs(improvement):.1f}%' if improvement >= 0 else f'+{abs(improvement):.1f}%',
                      ha='center', va='bottom', fontsize=9,
                      color='green' if improvement >= 0 else 'red', fontweight='bold')
    
    # Set y-axis limits
    max_val = max(max(sorted_baseline or [0]), max(sorted_enhanced or [0]))
    if max_val > 0:
        ax.set_ylim(0, max_val * 1.2)
    else:
        ax.set_ylim(0, 100.0)  # Default range if no data
    
    # Customize grid and spines
    ax.grid(True, axis='y', linestyle='--', alpha=0.3)
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    ax.spines['bottom'].set_linewidth(0.5)
    ax.spines['left'].set_linewidth(0.5)
    
    # Add legend
    ax.legend(loc='upper right', frameon=True, framealpha=0.95,
             edgecolor='#dddddd', facecolor='white', fontsize=11)
    
    # Save and show the plot
    plt.tight_layout()
    plt.savefig(filename, dpi=300, bbox_inches='tight', facecolor='white')
    plt.show()
    
    # Print summary table
    print(f"\n{metric_name} Comparison Summary:")
    print("=" * 80)
    print(f"{'Application':<20} {'Baseline (%)':<18} {'Records Data (%)':<22} {'Change (%)':<14}")
    print("-" * 80)
    
    for i, app in enumerate(sorted_apps):
        base = sorted_baseline[i]
        enhanced = sorted_enhanced[i]
        change = 0
        
        if base > 0 and enhanced > 0:
            change = ((base - enhanced) / base) * 100
            
        # Format the output to handle missing data
        base_str = f"{base:.2f}" if base > 0 else "N/A"
        enhanced_str = f"{enhanced:.2f}" if enhanced > 0 else "N/A"
        change_str = f"{'-' if change >= 0 else '+'}{abs(change):.2f}" if base > 0 and enhanced > 0 else "N/A"
            
        print(f"{display_names[app]:<20} {base_str:<18} {enhanced_str:<22} {change_str}")
    
    print("=" * 80)
    print(f"Plot includes {len(sorted_apps)} applications")
    print(f"Plot saved as '{filename}'")

# Generate plots for both metrics
if baseline_rob_stalls or data_rob_stalls:
    create_comparison_plot(
        baseline_rob_stalls, data_rob_stalls,
        "FULL_WINDOW_STALL_pct", 
        "Reorder Buffer Stalls", 
        "Full Window Stall Percentage (%)",
        "rob_stalls_comparison.png"
    )

if baseline_dcache_miss or data_dcache_miss:
    create_comparison_plot(
        baseline_dcache_miss, data_dcache_miss,
        "DCACHE_MISS_pct", 
        "Data Cache Miss Percentage", 
        "DCACHE Miss Percentage (%)",
        "dcache_miss_comparison.png"
    )