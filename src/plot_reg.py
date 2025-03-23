import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import glob
import os
import matplotlib as mpl
from matplotlib.colors import LinearSegmentedColormap

def analyze_same_base_register_pairs():
    # Set up professional styling
    plt.rcParams.update({
        'font.family': 'serif',
        'font.serif': ['Times New Roman', 'DejaVu Serif'],
        'font.size': 11,
        'axes.labelsize': 12,
        'axes.titlesize': 14,
        'axes.spines.top': True,
        'axes.spines.right': True,
        'axes.spines.left': True,
        'axes.spines.bottom': True,
        'axes.linewidth': 1.0,
        'figure.figsize': (10, 6),
        'figure.dpi': 150,
        'savefig.dpi': 300,
        'savefig.bbox': 'tight',
    })
    
    # Define teal color scheme
    primary_color = '#008080'  # Teal
    secondary_color = '#D0E7E7'  # Light teal/mint
    
    # Data structures to store our analysis
    workload_data = {}
    
    # Process each file
    for file_path in glob.glob('/users/deepmish/results/*.txt'):
        workload = os.path.basename(file_path).split('.')[0]  # Extract workload name
        workload_data[workload] = {
            'total_pairs': 0,
            'same_base_reg': 0,
            'diff_base_reg': 0,
            'same_base_by_stride': {}  # Track same-base percentage by stride value
        }
        
        # Extract fusion candidate lines
        with open(file_path, 'r') as f:
            for line in f:
                if line.startswith('Op1 PC:'):
                    parts = line.strip().split()
                    try:
                        # Extract all the data
                        op1_offset = int(parts[14])
                        op2_offset = int(parts[17])
                        op1_base_reg = int(parts[21])
                        op2_base_reg = int(parts[25])
                        
                        # Calculate stride
                        stride = abs(op2_offset - op1_offset)
                        
                        # Update totals
                        workload_data[workload]['total_pairs'] += 1
                        
                        # Check if base registers are the same
                        if op1_base_reg == op2_base_reg:
                            workload_data[workload]['same_base_reg'] += 1
                            
                            # Track by stride
                            if stride not in workload_data[workload]['same_base_by_stride']:
                                workload_data[workload]['same_base_by_stride'][stride] = {
                                    'total': 0,
                                    'same_base': 0
                                }
                            workload_data[workload]['same_base_by_stride'][stride]['total'] += 1
                            workload_data[workload]['same_base_by_stride'][stride]['same_base'] += 1
                        else:
                            workload_data[workload]['diff_base_reg'] += 1
                            
                            # Track by stride
                            if stride not in workload_data[workload]['same_base_by_stride']:
                                workload_data[workload]['same_base_by_stride'][stride] = {
                                    'total': 0,
                                    'same_base': 0
                                }
                            workload_data[workload]['same_base_by_stride'][stride]['total'] += 1
                            
                    except (IndexError, ValueError) as e:
                        continue
    
    # Calculate percentages
    for workload in workload_data:
        if workload_data[workload]['total_pairs'] > 0:
            # Overall same base register percentage
            workload_data[workload]['same_base_pct'] = (
                workload_data[workload]['same_base_reg'] / 
                workload_data[workload]['total_pairs'] * 100
            )
            
            # Calculate percentage by stride
            for stride in workload_data[workload]['same_base_by_stride']:
                stride_data = workload_data[workload]['same_base_by_stride'][stride]
                if stride_data['total'] > 0:
                    stride_data['same_base_pct'] = (
                        stride_data['same_base'] / stride_data['total'] * 100
                    )
    
    # Set up the figure for the main visualization
    fig, ax = plt.subplots(figsize=(10, 6), facecolor='white')
    ax.set_facecolor('#F9F9F9')  # Very light gray background
    
    # Create data for plotting
    workloads = []
    same_base_pcts = []
    diff_base_pcts = []
    
    for workload in sorted(workload_data.keys()):
        data = workload_data[workload]
        if data['total_pairs'] > 0:
            workloads.append(workload.capitalize())
            same_base_pcts.append(data['same_base_pct'])
            diff_base_pcts.append(100 - data['same_base_pct'])
    
    # Plot stacked bars
    bar_width = 0.6
    x = np.arange(len(workloads))
    
    # Create the stacked bar chart with selected colors
    ax.bar(x, same_base_pcts, bar_width, label='Same Base Register', 
           color=primary_color, edgecolor='black', linewidth=0.8)
    ax.bar(x, diff_base_pcts, bar_width, bottom=same_base_pcts, 
           label='Different Base Registers', color=secondary_color, 
           edgecolor='black', linewidth=0.8)
    
    # Add percentage labels for same base register
    for i, v in enumerate(same_base_pcts):
        ax.text(i, v/2, f"{v:.1f}%", ha='center', va='center', 
               color='white', fontweight='bold', fontsize=11)
    
    # Add percentage labels for different base register
    for i, v in enumerate(diff_base_pcts):
        if v > 10:  # Only add label if there's enough space
            ax.text(i, same_base_pcts[i] + v/2, f"{v:.1f}%", ha='center', 
                   va='center', color='black', fontsize=11)
    
    # Customize the plot
    ax.set_xticks(x)
    ax.set_xticklabels(workloads, fontsize=12, fontweight='bold')
    ax.set_ylabel('Percentage of Micro-op Pairs (%)', fontsize=13)
    ax.set_title('Load Micro-op Pairs Sharing Same Base Register Across Workloads', 
                fontsize=16, fontweight='bold', pad=20)
    
    # Add grid for readability
    ax.grid(axis='y', linestyle=':', alpha=0.3)
    ax.set_axisbelow(True)
    
    # Set y-axis limits
    ax.set_ylim(0, 100)

    
    # Add legend with positioning to avoid overlap
    # Move legend outside the plot area
    ax.legend(loc='upper center', bbox_to_anchor=(0.5, -0.12), ncol=2, frameon=True)
    
    # Adjust layout - make extra room at the bottom for the legend
    plt.tight_layout(rect=[0, 0.1, 1, 0.95])
    
    # Save the figure
    plt.savefig('base_register_analysis.png', dpi=300, bbox_inches='tight')
    plt.show()
    
    # Now create a secondary figure showing same base percentage by stride
    fig2, ax2 = plt.subplots(1, len(workloads), figsize=(15, 5), sharey=True, facecolor='white')
    
    # If only one workload, wrap in list
    if len(workloads) == 1:
        ax2 = [ax2]
    
    # Common strides to always display
    key_strides = [0, 8, 16, 24, 32, 40, 48, 56, 64]
    
    # Create color gradient for stride bars
    # Teal gradient
    cmap = LinearSegmentedColormap.from_list("teal_gradient", 
                                           ['#66B2B2', '#004C4C'])
    
    # Plot base register sharing by stride for each workload
    for i, (workload_name, ax_i) in enumerate(zip(workloads, ax2)):
        workload = workload_name.lower()
        ax_i.set_facecolor('#F9F9F9')
        
        # Collect data for this workload
        strides = []
        same_base_pcts_by_stride = []
        
        # Get all strides with enough data for meaningful analysis
        stride_data = workload_data[workload]['same_base_by_stride']
        
        # Process data for each key stride
        for stride in key_strides:
            if stride in stride_data and stride_data[stride]['total'] >= 5:
                strides.append(stride)
                same_base_pcts_by_stride.append(stride_data[stride]['same_base_pct'])
            elif stride in key_strides:
                strides.append(stride)
                same_base_pcts_by_stride.append(0)  # No data
        
        # Create bar positions
        x_pos = np.arange(len(strides))
        
        # Create the bars with color gradient based on value
        bars = ax_i.bar(x_pos, same_base_pcts_by_stride, width=0.7,
                       edgecolor='black', linewidth=0.8)
        
        # Color each bar with gradient based on value
        for j, bar in enumerate(bars):
            bar_value = same_base_pcts_by_stride[j]
            # Normalize value to 0-1 range for color mapping
            color_val = min(bar_value / 100.0, 1.0)
            bar.set_color(cmap(color_val))
        
        # Add percentage labels for meaningful bars
        for j, v in enumerate(same_base_pcts_by_stride):
            if v > 5:  # Only label if percentage is significant
                ax_i.text(j, v + 2, f"{v:.0f}%", ha='center', fontsize=9, 
                        color='black')
        
        # Customize the subplot
        ax_i.set_xticks(x_pos)
        ax_i.set_xticklabels([str(s) for s in strides], fontsize=10)
        ax_i.set_title(workload_name, fontsize=13, fontweight='bold')
        
        if i == 0:
            ax_i.set_ylabel('Same Base Register (%)', fontsize=12)
        
        ax_i.set_ylim(0, 100)
        ax_i.grid(axis='y', linestyle=':', alpha=0.3)
        ax_i.set_axisbelow(True)
    
    # Add common x label
    fig2.text(0.5, 0.01, 'Memory Access Stride (Bytes)', ha='center', fontsize=13)
    
    # Add overall title
    fig2.suptitle('Percentage of Micro-op Pairs Sharing Base Register by Stride', 
                 fontsize=16, fontweight='bold', y=0.98)
    
    # Create a legend for the second plot - place at the bottom
    handles = [plt.Rectangle((0,0),1,1, color=cmap(0.8), ec='black'),
               plt.Rectangle((0,0),1,1, color=cmap(0.3), ec='black')]
    labels = ['High % Same Register', 'Low % Same Register']
    fig2.legend(handles, labels, loc='lower center', 
               bbox_to_anchor=(0.5, -0.03), ncol=2, frameon=True)
    
    # Adjust layout - make extra room at the bottom for the legend
    plt.tight_layout(rect=[0, 0.08, 1, 0.95])
    
    # Save the figure
    plt.savefig('base_register_by_stride.png', dpi=300, bbox_inches='tight')
    plt.show()
    
    # Print summary statistics
    print("\n===== Base Register Analysis =====")
    for workload_name in workloads:
        workload = workload_name.lower()
        data = workload_data[workload]
        
        print(f"\n{workload_name}:")
        print(f"  Total fusion candidate pairs: {data['total_pairs']:,}")
        print(f"  Pairs with same base register: {data['same_base_reg']:,} ({data['same_base_pct']:.1f}%)")
        print(f"  Pairs with different base registers: {data['diff_base_reg']:,} ({100-data['same_base_pct']:.1f}%)")
        
        print("  By stride (top 5):")
        stride_data = [(s, data['same_base_by_stride'][s]) 
                      for s in data['same_base_by_stride']]
        stride_data.sort(key=lambda x: x[1]['total'], reverse=True)
        
        for stride, sdata in stride_data[:5]:
            if sdata['total'] > 0:
                print(f"    Stride {stride}B: {sdata['same_base_pct']:.1f}% same base " +
                      f"({sdata['same_base']:,}/{sdata['total']:,})")

analyze_same_base_register_pairs()