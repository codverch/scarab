import os
import re
import matplotlib.pyplot as plt
import numpy as np

# Directories containing core stat files
baseline_dir = '/users/deepmish/scarab/src/baseline'
ideal_fusion_dir = '/users/deepmish/scarab/src/records_with_data'

# Get all applications (subdirectories) in both directories
baseline_apps = [d for d in os.listdir(baseline_dir) if os.path.isdir(os.path.join(baseline_dir, d))]
ideal_fusion_apps = [d for d in os.listdir(ideal_fusion_dir) if os.path.isdir(os.path.join(ideal_fusion_dir, d))]

common_apps = sorted(set(baseline_apps).intersection(ideal_fusion_apps))
display_names = {app: f"{app.upper()}-LJ" for app in common_apps}

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
valid_apps = []
baseline_stall_pcts = []
ideal_fusion_stall_pcts = []

for app in common_apps:
    base_path = os.path.join(baseline_dir, app, 'core.stat.0.csv')
    ideal_path = os.path.join(ideal_fusion_dir, app, 'core.stat.0.csv')
    
    base_stall = extract_stall_percentage(base_path)
    ideal_stall = extract_stall_percentage(ideal_path)
    
    if base_stall is not None and ideal_stall is not None:
        valid_apps.append(app)
        baseline_stall_pcts.append(base_stall)
        ideal_fusion_stall_pcts.append(ideal_stall)
        
        print(f"\nApp: {app}")
        print(f"Baseline FULL_WINDOW_STALL_pct: {base_stall:.2f}%")
        print(f"Ideal Fusion FULL_WINDOW_STALL_pct: {ideal_stall:.2f}%")
        if base_stall > 0:
            reduction = ((base_stall - ideal_stall) / base_stall) * 100
            print(f"Stall Reduction: {reduction:.2f}%")

# Create plot if we have valid data
if valid_apps:
    plt.rcParams['font.family'] = 'serif'
    
    # Stall Percentage Plot
    fig, ax = plt.subplots(figsize=(12, 6.5))
    fig.patch.set_facecolor('white')
    ax.set_facecolor('#f9f9f9')
    
    # Sort applications by improvement for more interesting visualization
    if valid_apps:
        # Calculate reduction percentages
        reductions = []
        for i in range(len(valid_apps)):
            if baseline_stall_pcts[i] > 0:
                reduction = ((baseline_stall_pcts[i] - ideal_fusion_stall_pcts[i]) / baseline_stall_pcts[i]) * 100
            else:
                reduction = 0
            reductions.append(reduction)
        
        # Create sorted indices based on reduction
        sorted_indices = sorted(range(len(reductions)), key=lambda i: reductions[i], reverse=True)
        
        # Sort all data using these indices
        sorted_apps = [valid_apps[i] for i in sorted_indices]
        sorted_baseline = [baseline_stall_pcts[i] for i in sorted_indices]
        sorted_ideal = [ideal_fusion_stall_pcts[i] for i in sorted_indices]
        sorted_reductions = [reductions[i] for i in sorted_indices]
        
        # Update the valid_apps and other lists with sorted data
        valid_apps = sorted_apps
        baseline_stall_pcts = sorted_baseline
        ideal_fusion_stall_pcts = sorted_ideal
    
    # Bar placement
    index = np.arange(len(valid_apps))
    bar_width = 0.35
    
    baseline_bars = ax.bar(index - bar_width / 2, baseline_stall_pcts, bar_width,
                           label='Baseline', color='#2f8e89', alpha=0.9)
    ideal_bars = ax.bar(index + bar_width / 2, ideal_fusion_stall_pcts, bar_width,
                        label='Ideal Fusion', color='#59c959', alpha=0.9)
    
    # Add horizontal connecting lines between baseline and ideal fusion bars to emphasize the difference
    for i in range(len(valid_apps)):
        base_height = baseline_stall_pcts[i]
        ideal_height = ideal_fusion_stall_pcts[i]
        base_x = index[i] - bar_width / 2
        ideal_x = index[i] + bar_width / 2
        
        # Draw a dashed line connecting the tops of the bars
        if base_height > ideal_height:
            color = 'green'  # Green for improvement
        else:
            color = 'red'    # Red for regression
            
        ax.plot([base_x, ideal_x], [base_height, ideal_height], 
                color=color, linestyle='--', linewidth=1.5, alpha=0.7)
            
    # Add x and y axis labels
    ax.set_xlabel('Datacenter Applications', fontsize=12)
    ax.set_ylabel('Full Window Stall Percentage (%)', fontsize=12)
    
    # Change the title to be more descriptive
    ax.set_title('Reorder Buffer Stalls', fontsize=14, fontweight='bold')
    ax.set_xticks(index)
    ax.set_xticklabels([display_names[app] for app in valid_apps], rotation=45, ha='right')
    
    # Add numerical labels on bars
    def add_labels(bars):
        for bar in bars:
            height = bar.get_height()
            ax.text(bar.get_x() + bar.get_width()/2, height + 1.0,
                    f'{height:.1f}%', ha='center', va='bottom', fontsize=9, fontweight='bold')
    
    add_labels(baseline_bars)
    add_labels(ideal_bars)
    
    # Annotate % reduction above Ideal Fusion bars
    for i in range(len(valid_apps)):
        if baseline_stall_pcts[i] > 0:
            # For stall percentages, lower is better, so we calculate reduction
            reduction = ((baseline_stall_pcts[i] - ideal_fusion_stall_pcts[i]) / baseline_stall_pcts[i]) * 100
            y = ideal_fusion_stall_pcts[i] + 3.0
            # Make all decrease percentages red, regardless of whether it's an improvement or regression
            ax.text(index[i] + bar_width / 2, y, f'-{abs(reduction):.1f}%' if reduction >= 0 else f'+{abs(reduction):.1f}%',
                    ha='center', va='bottom', fontsize=9,
                    color='red' if reduction >= 0 else 'red', fontweight='bold')
    
    # Tweak axes and grid
    if max(baseline_stall_pcts + ideal_fusion_stall_pcts) > 0:
        ax.set_ylim(0, max(max(baseline_stall_pcts), max(ideal_fusion_stall_pcts)) * 1.2)
    else:
        ax.set_ylim(0, 100.0)  # Default range if no data
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
    plt.savefig('full_window_stall_comparison.png', dpi=300, bbox_inches='tight', facecolor='white')
    plt.show()
    
    # Print summary
    print("\nFull Window Stall Percentage Comparison Summary:")
    print("=" * 80)
    print(f"{'Benchmark-Workload':<20} {'Baseline Stall (%)':<18} {'Ideal Fusion Stall (%)':<22} {'Decrease (%)':<14}")
    print("-" * 80)
    for i, app in enumerate(valid_apps):
        reduction = 0
        if baseline_stall_pcts[i] > 0:
            reduction = ((baseline_stall_pcts[i] - ideal_fusion_stall_pcts[i]) / baseline_stall_pcts[i]) * 100
            
        print(f"{display_names[app]:<20} {baseline_stall_pcts[i]:<18.2f} {ideal_fusion_stall_pcts[i]:<22.2f} {'-' if reduction >= 0 else '+'}{abs(reduction):.2f}")
    print("=" * 80)
    print(f"Comparison includes {len(valid_apps)} applications: {', '.join(valid_apps)}")
    print("Plot saved as 'full_window_stall_comparison_other.png'")
else:
    print("No valid applications found with stall statistics. Check file paths and CSV format.")