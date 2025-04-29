#!/usr/bin/env python3
import os
import re
import matplotlib.pyplot as plt
import numpy as np

# Directories containing core stat files
baseline_dir = '/users/deepmish/scarab/src/baseline'
data_dir = '/users/deepmish/scarab/src/records_with_data'

# Get all applications (subdirectories) in both directories
baseline_apps = [d for d in os.listdir(baseline_dir) if os.path.isdir(os.path.join(baseline_dir, d))]
data_apps = [d for d in os.listdir(data_dir) if os.path.isdir(os.path.join(data_dir, d))]

# Function to extract FULL_WINDOW_STALL_pct from core.stat.0.csv
def extract_stall_percentage(file_path):
    if not os.path.exists(file_path):
        print(f"File not found: {file_path}")
        return None
    
    try:
        # Read the file content
        with open(file_path, 'r') as f:
            content = f.read()
        
        # Extract FULL_WINDOW_STALL_pct
        stall_match = re.search(r'FULL_WINDOW_STALL_pct,\s+(\d+\.\d+)', content)
        
        if stall_match:
            stall_pct = float(stall_match.group(1))
            print(f"File: {file_path}, FULL_WINDOW_STALL_pct: {stall_pct:.2f}%")
            return stall_pct
        else:
            print(f"Could not find FULL_WINDOW_STALL_pct in {file_path}")
            return None
        
    except Exception as e:
        print(f"Error processing {file_path}: {e}")
        return None

# Collect stall percentages for each application
# IMPORTANT: Handle different file naming conventions
data_stalls = {}
baseline_stalls = {}

# First collect all data from records_with_data directory
for app in data_apps:
    path = os.path.join(data_dir, app, 'core.stat.0.csv')
    stall = extract_stall_percentage(path)
    
    if stall is not None:
        data_stalls[app] = stall
        print(f"\nApp: {app}")
        print(f"Records_with_data FULL_WINDOW_STALL_pct: {stall:.2f}%")

# Then collect all available baseline data with different naming pattern
for app in baseline_apps:
    # In baseline directory, files are named as app_core.stat.0.csv
    path = os.path.join(baseline_dir, app, f'{app}_core.stat.0.csv')
    stall = extract_stall_percentage(path)
    
    if stall is not None:
        baseline_stalls[app] = stall
        print(f"\nApp: {app}")
        print(f"Baseline FULL_WINDOW_STALL_pct: {stall:.2f}%")

# Get the union of all apps that have data in at least one directory
all_apps = sorted(set(list(data_stalls.keys()) + list(baseline_stalls.keys())))

if all_apps:
    display_names = {app: f"{app.upper()}-SOC-LJ" for app in all_apps}
    
    # Create lists for plotting
    plot_apps = []
    plot_data_stalls = []
    plot_baseline_stalls = []
    
    for app in all_apps:
        # Only include apps where we have at least one value
        if app in data_stalls or app in baseline_stalls:
            plot_apps.append(app)
            # Use the value if available, otherwise use 0
            plot_data_stalls.append(data_stalls.get(app, 0))
            plot_baseline_stalls.append(baseline_stalls.get(app, 0))
    
    # Calculate reduction percentages where both values exist
    reductions = []
    for i, app in enumerate(plot_apps):
        if app in baseline_stalls and app in data_stalls and baseline_stalls[app] > 0:
            reduction = ((baseline_stalls[app] - data_stalls[app]) / baseline_stalls[app]) * 100
            reductions.append(reduction)
        else:
            reductions.append(0)
    
    # Sort applications by reduction for better visualization
    # Note: If you prefer to sort by stall values instead, you can modify this logic
    if plot_apps:
        # Create sorted indices based on stall values in data_dir
        sorted_indices = sorted(range(len(plot_data_stalls)), key=lambda i: plot_data_stalls[i], reverse=True)
        
        # Sort all data using these indices
        sorted_apps = [plot_apps[i] for i in sorted_indices]
        sorted_data_stalls = [plot_data_stalls[i] for i in sorted_indices]
        sorted_baseline_stalls = [plot_baseline_stalls[i] for i in sorted_indices]
        sorted_reductions = [reductions[i] for i in sorted_indices]
    
    # Create the plot
    plt.rcParams['font.family'] = 'serif'
    fig, ax = plt.subplots(figsize=(14, 8))
    fig.patch.set_facecolor('white')
    ax.set_facecolor('#f9f9f9')
    
    # Bar placement
    index = np.arange(len(sorted_apps))
    bar_width = 0.35
    
    # Create the bars
    baseline_bars = ax.bar(index - bar_width/2, sorted_baseline_stalls, bar_width,
                          label='Baseline', color='#2f8e89', alpha=0.9)
    data_bars = ax.bar(index + bar_width/2, sorted_data_stalls, bar_width,
                      label='Ideal fusion', color='#59c959', alpha=0.9)
    
    # Add horizontal connecting lines between baseline and ideal fusion bars to emphasize the difference
    for i in range(len(sorted_apps)):
        base_height = sorted_baseline_stalls[i]
        data_height = sorted_data_stalls[i]
        
        # Only draw connecting lines if both bars have data
        if base_height > 0 and data_height > 0:
            base_x = index[i] - bar_width/2
            data_x = index[i] + bar_width/2
            
            # Draw a dashed line connecting the tops of the bars
            if base_height > data_height:
                color = 'green'  # Green for improvement
            else:
                color = 'red'    # Red for regression
                
            ax.plot([base_x, data_x], [base_height, data_height], 
                    color=color, linestyle='--', linewidth=1.5, alpha=0.7)
    
    # Add x and y axis labels
    ax.set_xlabel('Datacenter Applications', fontsize=12)
    ax.set_ylabel('Full Window Stall Percentage (%)', fontsize=12)
    
    # Set title
    ax.set_title('Reorder Buffer Stalls', fontsize=14, fontweight='bold')
    
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
    add_labels(data_bars)
    
    # Annotate % reduction where applicable
    for i in range(len(sorted_apps)):
        base_height = sorted_baseline_stalls[i]
        data_height = sorted_data_stalls[i]
        
        # Only calculate reduction if both values exist and baseline > 0
        if base_height > 0 and data_height > 0:
            reduction = ((base_height - data_height) / base_height) * 100
            y = max(base_height, data_height) + 5.0
            
            # Only show reduction label if it's not 0
            if abs(reduction) > 0.1:  # Small threshold to avoid rounding issues
                ax.text(index[i], y, 
                      f'-{abs(reduction):.1f}%' if reduction >= 0 else f'+{abs(reduction):.1f}%',
                      ha='center', va='bottom', fontsize=9,
                      color='green' if reduction >= 0 else 'red', fontweight='bold')
    
    # Set y-axis limits
    max_val = max(max(sorted_data_stalls or [0]), max(sorted_baseline_stalls or [0]))
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
    
    # No descriptive box at the bottom
    
    # Save and show the plot
    plt.tight_layout()
    plt.savefig('rob_stalls_comparison.png', dpi=300, bbox_inches='tight', facecolor='white')
    plt.show()
    
    # Print summary
    print("\nFull Window Stall Percentage Comparison Summary:")
    print("=" * 80)
    print(f"{'Application':<20} {'Baseline Stall (%)':<18} {'Ideal Fusion (%)':<22} {'Change (%)':<14}")
    print("-" * 80)
    
    for i, app in enumerate(sorted_apps):
        base = sorted_baseline_stalls[i]
        data = sorted_data_stalls[i]
        change = 0
        
        if base > 0 and data > 0:
            change = ((base - data) / base) * 100
            
        # Format the output to handle missing data
        base_str = f"{base:.2f}" if base > 0 else "N/A"
        data_str = f"{data:.2f}" if data > 0 else "N/A"
        change_str = f"{'-' if change >= 0 else '+'}{abs(change):.2f}" if base > 0 and data > 0 else "N/A"
            
        print(f"{display_names[app]:<20} {base_str:<18} {data_str:<22} {change_str}")
    
    print("=" * 80)
    print(f"Plot includes {len(sorted_apps)} applications")
    print("Plot saved as 'rob_stalls_comparison.png'")
else:
    print("No valid applications found with stall statistics. Check file paths and CSV format.")