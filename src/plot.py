import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import glob
import os
import matplotlib as mpl
from matplotlib.ticker import MultipleLocator

def analyze_fusion_strides_by_workload():
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
        'figure.figsize': (14, 5.5),
        'figure.dpi': 150,
        'savefig.dpi': 300,
        'savefig.bbox': 'tight',
    })
    
    # Define columns for our data
    workload_strides = {}
    
    # Process each file
    for file_path in glob.glob('/users/deepmish/results/*.txt'):
        workload = os.path.basename(file_path).split('.')[0]  # Extract workload name
        workload_strides[workload] = []
        
        # Extract fusion candidate lines
        with open(file_path, 'r') as f:
            for line in f:
                if line.startswith('Op1 PC:'):
                    parts = line.strip().split()
                    try:
                        op1_offset = int(parts[14])
                        op2_offset = int(parts[17])
                        stride = abs(op2_offset - op1_offset)
                        
                        workload_strides[workload].append(stride)
                    except (IndexError, ValueError):
                        continue
    
    # Sort workloads based on the percentage of 0 stride (most important metric)
    workload_zero_percent = {}
    for workload, strides in workload_strides.items():
        if strides:
            zero_count = sum(1 for s in strides if s == 0)
            workload_zero_percent[workload] = (zero_count / len(strides)) * 100
    
    # Sort workloads by their 0-stride percentage
    sorted_workloads = sorted(workload_zero_percent.keys(), 
                             key=lambda w: workload_zero_percent[w], reverse=True)
    
    # Create figure with subplots
    fig, axes = plt.subplots(1, len(sorted_workloads), figsize=(14, 5.5), sharey=True)
    
    # If there's only one workload, wrap axes in a list
    if len(sorted_workloads) == 1:
        axes = [axes]
    
    # Define color palette - professional blue
    aligned_color = '#4682B4'  # Steel blue for 8-byte aligned
    
    # Common strides to display (multiples of 8 up to 64)
    display_strides = list(range(0, 65, 8))
    
    # Create the plots
    for i, (ax, workload) in enumerate(zip(axes, sorted_workloads)):
        strides = workload_strides[workload]
        
        if not strides:  # Handle empty workloads
            ax.text(0.5, 0.5, 'No Data', ha='center', va='center', transform=ax.transAxes)
            continue
        
        # Count occurrences of each stride value
        stride_counts = {}
        for stride in strides:
            stride_counts[stride] = stride_counts.get(stride, 0) + 1
        
        # Calculate total strides for percentage
        total_strides = len(strides)
        
        # Prepare data for plotting - focus on multiples of 8
        x_values = display_strides
        y_values = [(stride_counts.get(x, 0) / total_strides) * 100 for x in x_values]
        
        # Create bar positions
        bar_positions = np.arange(len(x_values))
        
        # Plot the bars
        bars = ax.bar(bar_positions, y_values, width=0.75, color=aligned_color, 
                     edgecolor='black', linewidth=0.7, zorder=3)
        
        # Add percentage labels on top of the bars
        for j, (bar, y_val) in enumerate(zip(bars, y_values)):
            if y_val >= 0.5:  # Show percentage for any meaningful value
                ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.5, 
                       f"{y_val:.0f}%", ha='center', va='bottom', 
                       color='black', fontsize=9, fontweight='normal')
        
        # Format axes and grid
        ax.set_xticks(bar_positions)
        ax.set_xticklabels([str(x) for x in x_values], fontsize=10)
        ax.set_ylim(0, 45)  # Set max to 45% for better visibility
        
        # Y-axis grid lines - every 10% but more subtle
        ax.yaxis.set_major_locator(MultipleLocator(10))
        ax.grid(axis='y', linestyle=':', color='gray', alpha=0.4, zorder=0)
        
        # Set workload name as title
        ax.set_title(workload.capitalize(), fontsize=14, pad=10, fontweight='bold')
        
        # Only show y label on first subplot
        if i == 0:
            ax.set_ylabel('Load Micro-op Pairs Accessing\nSame Cacheline (%)', 
                          fontsize=12, labelpad=10)
        
        # Enhance the appearance of the axes
        for spine in ax.spines.values():
            spine.set_linewidth(1.0)
            spine.set_color('black')
            
        # Add subtle background shading for alternate grid rows
        for y in range(0, 101, 20):
            ax.axhspan(y, y+10, color='#f5f5f5', alpha=0.3, zorder=0)
        
    # Add common x-label
    fig.text(0.5, 0.02, 'Stride Between Micro-ops (Bytes Offset Within Cacheline)', 
             ha='center', fontsize=13, fontweight='bold')
    
    # Add title with more space
    fig.suptitle('Distribution of Memory Access Strides Across Workloads', 
                fontsize=16, fontweight='bold', y=0.98)
    

    
    # Create a legend with styled elements
    from matplotlib.patches import Patch
    legend_elements = [
        Patch(facecolor=aligned_color, edgecolor='black', label='8-Byte Aligned Strides')
    ]
    
    # Place legend at bottom right with a border
    leg = fig.legend(handles=legend_elements, loc='lower right', bbox_to_anchor=(0.99, 0.01),
                   frameon=True, framealpha=0.9, facecolor='white', edgecolor='#CCCCCC')
    leg.get_frame().set_linewidth(0.8)
    
    # Add a thin border around the entire figure
    fig.patch.set_linewidth(1)
    fig.patch.set_edgecolor('black')
    
    # Adjust layout
    plt.tight_layout(rect=[0, 0.07, 1, 0.93])
    plt.subplots_adjust(wspace=0.05)  # Reduce spacing between subplots
    
    # Save with high quality
    plt.savefig('stride_distribution_enhanced.png', dpi=300, bbox_inches='tight')
    plt.show()

analyze_fusion_strides_by_workload()