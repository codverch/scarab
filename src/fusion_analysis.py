#!/usr/bin/env python3
"""
Research-Quality Cacheline Temperature Distribution Plot Generator
with optimal label placement and improved visibility
"""

import sys
import re
import os
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib as mpl
from collections import Counter, defaultdict
from matplotlib.patches import Patch, Rectangle
from matplotlib.ticker import StrMethodFormatter, MaxNLocator
import matplotlib.patheffects as PathEffects

# Make sure plots can be saved even on a headless server
import matplotlib
matplotlib.use('Agg')

# Create output directory
results_dir = "fusion_results"
os.makedirs(results_dir, exist_ok=True)
print(f"Results will be saved to: {os.path.abspath(results_dir)}")

# Exact color palette from the example image
plot_colors = ["#1f77b4", "#2ca02c", "#ff7f0e", "#d62728"]  # Blue, Green, Orange, Red

def parse_fusion_data(lines):
    """
    Parse and analyze fusion data from log lines.
    Expected format: "First op: addr1 Second op: addr2 Cacheblock addr: addr3"
    """
    # Create a DataFrame from the file data
    data = []
    fusion_pattern = re.compile(r'First op: ([0-9a-f]+)\s+Second op: ([0-9a-f]+)\s+Cacheblock addr: ([0-9a-f]+)')
    
    for line in lines:
        match = fusion_pattern.search(line)
        if match:
            first_op = match.group(1)
            second_op = match.group(2)
            cacheblock = match.group(3)
            data.append((first_op, second_op, cacheblock))
    
    # Create DataFrame from parsed data
    df = pd.DataFrame(data, columns=['First_op', 'Second_op', 'Cacheblock'])
    
    # Count unique elements
    unique_cachelines = df['Cacheblock'].nunique()
    unique_op_pairs = df.groupby(['First_op', 'Second_op']).ngroups
    total_accesses = len(df)
    
    # Count op pairs per cacheline (how many unique micro-op pairs access each cacheline)
    cacheline_counts = df.groupby('Cacheblock').apply(
        lambda x: x.groupby(['First_op', 'Second_op']).ngroups
    ).reset_index(name='op_pair_count')
    
    return df, cacheline_counts, unique_cachelines, unique_op_pairs, total_accesses

def categorize_cachelines(cacheline_data):
    """Categorize cachelines by temperature (number of unique micro-op PC pairs)."""
    
    # Create a copy of the data to avoid warnings
    data = cacheline_data.copy()
    
    # Manually categorize rather than using pd.cut to avoid categorical issues
    data['temperature'] = 'Cold (1-2 pairs)'  # Default category
    
    # Assign categories based on op_pair_count
    data.loc[data['op_pair_count'] > 2, 'temperature'] = 'Warm (3-10 pairs)'
    data.loc[data['op_pair_count'] > 10, 'temperature'] = 'Hot (11+ pairs)'
    
    # Identify super hot cachelines (top 1%)
    if len(data) > 0:
        threshold = data['op_pair_count'].quantile(0.99)
        data.loc[data['op_pair_count'] > threshold, 'temperature'] = 'Super Hot (top 1%)'
    
    return data

def create_temperature_plot(cacheline_data, unique_cachelines, unique_op_pairs, total_accesses, output_prefix="plot"):
    """Create a cacheline temperature plot using actual data from the file."""
    if len(cacheline_data) == 0:
        print("No data to plot")
        return
    
    # --- STYLE SETUP ---
    # Match the example styling exactly
    plt.rcParams.update({
        'font.family': 'sans-serif',
        'font.sans-serif': ['Arial', 'Helvetica', 'DejaVu Sans'],
        'font.size': 10,
        'axes.labelsize': 11,
        'axes.titlesize': 14,
        'xtick.labelsize': 10,
        'ytick.labelsize': 10,
        'figure.figsize': (10, 12),  # Increased height for larger info boxes
        'figure.dpi': 300,
        'figure.facecolor': 'white',
        'axes.facecolor': 'white',
        'axes.grid': True,
        'grid.alpha': 0.2,
        'axes.spines.top': False,
        'axes.spines.right': False,
        'axes.linewidth': 0.8,
    })
    
    # Create figure with better proportions for larger info boxes
    fig = plt.figure(figsize=(10, 12))
    
    # Create grid layout with better spacing for bottom boxes
    gs = plt.GridSpec(12, 1, figure=fig, height_ratios=[6, 6, 6, 6, 6, 6, 1, 2, 2, 1, 1, 1])
    
    # Main plot (first 6 grid cells)
    ax = fig.add_subplot(gs[:6, 0])
    
    # --- DATA PREPARATION ---
    # Get temperature distribution from actual data
    temp_order = ['Cold (1-2 pairs)', 'Warm (3-10 pairs)', 
                 'Hot (11+ pairs)', 'Super Hot (top 1%)']
    temp_counts = cacheline_data['temperature'].value_counts().reindex(temp_order).fillna(0)
    
    # Calculate percentages for cachelines
    total = temp_counts.sum()
    temp_percent = [round((count / total * 100), 1) for count in temp_counts.values]
    
    # Calculate total micro-op pairs per category
    # Sum the op_pair_count for each temperature category
    temp_pairs = cacheline_data.groupby('temperature')['op_pair_count'].sum().reindex(temp_order).fillna(0)
    total_pairs = temp_pairs.sum()
    temp_pairs_percent = [round((count / total_pairs * 100), 1) for count in temp_pairs.values]
    
    # --- BAR CHART CREATION ---
    # Create bars with exact styling as example
    bars = ax.bar(range(len(temp_counts)), temp_counts.values, 
                 color=plot_colors,
                 edgecolor='white', linewidth=0.5, width=0.7)
    
    # X-axis styling to match example
    ax.set_xticks(range(len(temp_counts)))
    simple_labels = ['Cold', 'Warm', 'Hot', 'Super']
    x_labels = [f"{s}\n({t.split('(')[1][:-1]})" for s, t in zip(simple_labels, temp_order)]
    ax.set_xticklabels(x_labels, fontweight='bold')
    
    # Y-axis styling to match example
    ax.set_ylabel('Number of Unique Cachelines', fontsize=11, fontweight='bold')
    y_max = max(temp_counts.values) * 1.3 if max(temp_counts.values) > 0 else 10  # Increased headroom for labels
    ax.set_ylim(0, y_max)
    ax.yaxis.set_major_formatter(StrMethodFormatter('{x:,.0f}'))
    
    # Grid styling to match example
    ax.grid(axis='y', alpha=0.15, linestyle='-', color='#cccccc')
    ax.set_axisbelow(True)
    
    # --- VALUE ANNOTATIONS ---
    # Top values (count + percentage)
    for i, bar in enumerate(bars):
        height = bar.get_height()
        # Match exact positioning of values in example
        ax.text(bar.get_x() + bar.get_width()/2, height + (y_max * 0.01),
                f"{int(height):,}\n({temp_percent[i]}%)", 
                ha='center', va='bottom', fontsize=10, fontweight='bold',
                path_effects=[PathEffects.withStroke(linewidth=3, foreground='white')])
        
        # Special handling for Super Hot category
        if i == 3:  # Always show Super Hot outside the bar for consistency
            # Always show value to the right of the bar in a box
            ax.annotate(
                f"{int(temp_pairs.values[i]):,}\n({temp_pairs_percent[i]}%)",
                xy=(bar.get_x() + bar.get_width(), height/2),
                xytext=(bar.get_x() + bar.get_width() + 0.3, height/2),
                ha='left', va='center', fontsize=10, fontweight='bold',
                bbox=dict(boxstyle="round,pad=0.3", facecolor="white", 
                         edgecolor=plot_colors[i], alpha=1.0),
                arrowprops=dict(arrowstyle="-", color=plot_colors[i],
                               connectionstyle="angle,angleA=0,angleB=90,rad=10")
            )
        else:
            # Standard in-bar placement for other categories
            if height > 0:
                text_color = 'white'  # White text for all bars to match example
                ax.text(bar.get_x() + bar.get_width()/2, height/2,
                        f"{int(temp_pairs.values[i]):,}\n({temp_pairs_percent[i]}%)", 
                        ha='center', va='center', fontsize=10, fontweight='bold',
                        color=text_color)
    
    # --- IMPROVED KEY INSIGHT CALLOUT ---
    # Find the category with highest micro-op pairs
    if len(temp_pairs) > 0 and max(temp_pairs) > 0:
        max_idx = np.argmax(temp_pairs.values)
        max_category_name = simple_labels[max_idx]
        max_percent = temp_percent[max_idx]
        max_pairs_percent = temp_pairs_percent[max_idx]
        
        # Create insight text with actual values
        insight_text = f"A small number of cachelines ({max_percent}%)\ncontribute {max_pairs_percent}% of total micro-op activity"
        
        # Get the bar to highlight
        highlight_bar = bars[max_idx]
        highlight_height = highlight_bar.get_height()
        
        # Position the insight box diagonally above the highlighted bar
        # Calculate position relative to the specific bar
        bar_center_x = highlight_bar.get_x() + highlight_bar.get_width()/2
        insight_x = bar_center_x + 0.5  # Move the insight box a bit to the right
        
        # Adjust vertical position based on bar height
        if highlight_height > (y_max * 0.6):
            # For very tall bars, put it to the side
            insight_x = bar_center_x + 1.5  # Move further to the right for tall bars
            insight_y = highlight_height * 0.7
            connection_style = "arc3,rad=0.2"
        else:
            # For shorter bars, put it above
            insight_y = highlight_height + (y_max * 0.25)
            connection_style = "arc3,rad=-0.5"
        
        # Add the insight annotation
        ax.annotate(
            insight_text,
            xy=(bar_center_x, highlight_height),
            xytext=(insight_x, insight_y),
            ha='center', va='center', fontsize=11, fontweight='bold',
            bbox=dict(boxstyle="round,pad=0.4", facecolor="#fff8e1", 
                     edgecolor="#ff9900", alpha=1.0, linewidth=1),
            arrowprops=dict(arrowstyle="->", color="#ff9900", lw=1.5,
                           connectionstyle=connection_style)
        )
    
    # --- LEGEND STYLING ---
    # Match legend style from example
    legend_labels = ['Cold (1-2 pairs)', 'Warm (3-10 pairs)', 
                    'Hot (11+ pairs)', 'Super Hot (top 1%)']
    handles = [Patch(facecolor=c, label=l) 
              for c, l in zip(plot_colors, legend_labels)]
    
    legend = ax.legend(handles=handles, 
                      loc='upper right',
                      title='Temperature Categories',
                      frameon=True,
                      framealpha=1.0,
                      edgecolor='#cccccc')
    
    # --- TITLE STYLING ---
    # Match title styling from example
    ax.set_title('Distribution of Cachelines by Temperature Category (GCC 253)', 
                fontsize=14, fontweight='bold', pad=10)
    
    # --- LARGER INFO BOXES BELOW GRAPH ---
    # Chart interpretation box (first info box below graph)
    interp_ax = fig.add_subplot(gs[7:9, 0])
    interp_ax.axis('off')  # Hide axes
    
    # Add interpretation text with box
    interp_text = (
        "Chart Interpretation:\n"
        "Top Value: Cacheline count (% of total cachelines)\n"
        "Center Value: Micro-op pairs (% of total micro-op pairs)"
    )
    
    # Create the box with text
    interp_ax.text(0.5, 0.5, interp_text, 
                  ha='center', va='center', 
                  fontsize=12,
                  bbox=dict(boxstyle='round', facecolor='#f9f9f9', 
                           edgecolor='#dddddd', pad=0.8),
                  transform=interp_ax.transAxes)
    
    # Summary info box (second info box below graph)
    summary_ax = fig.add_subplot(gs[9:11, 0])
    summary_ax.axis('off')  # Hide axes
    
    # Add summary text with box using actual values
    summary_text = (
        f"Total Analysis\n"
        f"• Unique Cachelines: {unique_cachelines:,}\n"
        f"• Unique Micro-op Pairs: {unique_op_pairs:,}\n"
        f"• Total Activity: {total_accesses:,}"
    )
    
    # Create the box with text
    summary_ax.text(0.5, 0.5, summary_text, 
                   ha='center', va='center', 
                   fontsize=12,
                   bbox=dict(boxstyle='round', facecolor='#f9f9f9', 
                            edgecolor='#dddddd', pad=0.8),
                   transform=summary_ax.transAxes)
    
    # --- FINALIZE ---
    plt.tight_layout(rect=[0, 0, 1, 0.97])  # Adjust to account for larger bottom boxes
    output_path = f"{output_prefix}_temperature.png"
    plt.savefig(
        output_path, 
        dpi=300, 
        bbox_inches='tight', 
        facecolor='white'
    )
    print(f"Saved enhanced temperature distribution plot to {output_path}")
    plt.close()

def main():
    # Check if input file was provided
    if len(sys.argv) > 1:
        input_file = sys.argv[1]
        output_prefix = os.path.join(results_dir, os.path.basename(input_file).split('.')[0] + "_analysis")
        
        try:
            with open(input_file, 'r') as f:
                lines = f.readlines()
        except FileNotFoundError:
            print(f"Error: File '{input_file}' not found.")
            sys.exit(1)
    else:
        # Read from stdin
        print("Reading fusion data from stdin...")
        lines = sys.stdin.readlines()
        output_prefix = os.path.join(results_dir, "fusion_analysis")
    
    # Filter out non-fusion data lines
    fusion_lines = [line for line in lines if "First op:" in line]
    
    print(f"Found {len(fusion_lines)} fusion data lines to analyze.")
    
    # Parse and analyze the fusion data
    df, cacheline_data, unique_cachelines, unique_op_pairs, total_accesses = parse_fusion_data(fusion_lines)
    
    if unique_cachelines == 0:
        print("No cachelines found in the data.")
        return
    
    # Categorize cachelines
    categorized_data = categorize_cachelines(cacheline_data)
    
    # Save DataFrame to CSV for inspection
    categorized_data.to_csv(f"{output_prefix}_data.csv", index=False)
    print(f"Saved analysis data to {output_prefix}_data.csv")
    
    # Create the temperature plot
    create_temperature_plot(categorized_data, unique_cachelines, unique_op_pairs, total_accesses, output_prefix)
    
    print("\nAnalysis and visualization complete!")

if __name__ == "__main__":
    main()