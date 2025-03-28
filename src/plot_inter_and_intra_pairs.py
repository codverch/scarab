#!/usr/bin/env python3
"""
Micro-Op Pair Distance Analyzer

This script analyzes:
1. Intra-pair distances: The distance between Micro-op 1 and Micro-op 2 within each pair
2. Inter-pair distances: The distance between consecutive pairs that access the same cache block

Usage:
    python microop_analyzer.py <workload_dir> [--output OUTPUT_DIR]
"""

import os
import re
import glob
import argparse
import numpy as np
import matplotlib.pyplot as plt
from collections import defaultdict, namedtuple

# Define a namedtuple to store micro-op pair information
MicroOpPair = namedtuple('MicroOpPair', ['pc1', 'pc2', 'cacheblock', 'op_num1', 'op_num2'])

def parse_args():
    parser = argparse.ArgumentParser(description='Analyze Micro-op pair distances')
    parser.add_argument('workload_dir', help='Directory containing workload TXT files')
    parser.add_argument('--output', '-o', default='microop_analysis', help='Output directory')
    parser.add_argument('--bins', default='0,1,11,21,31,41,51,61,71,81,91,101,1000,10000', 
                      help='Comma-separated bin edges for histograms')
    parser.add_argument('--max-y', type=float, default=None, help='Max y-axis value for plots')
    return parser.parse_args()

def parse_workload_file(txt_path):
    """Parse workload TXT file to extract micro-op pairs"""
    micro_op_pairs = []
    
    # Regular expression to match the micro-op pair lines
    pattern = re.compile(
        r'Micro-op\s+1:\s+([0-9a-fA-Fx]+)\s+'
        r'Micro-op\s+2:\s+([0-9a-fA-Fx]+)\s+'
        r'Cacheblock\s+Address:\s+([0-9a-fA-Fx]+)\s+'
        r'Micro-op\s+1\s+Number:\s+(\d+)\s+'
        r'Micro-op\s+2\s+Number:\s+(\d+)'
    )
    
    with open(txt_path, 'r') as f:
        for line in f:
            match = pattern.search(line)
            if match:
                pc1, pc2, cacheblock = match.group(1), match.group(2), match.group(3)
                op_num1, op_num2 = int(match.group(4)), int(match.group(5))
                
                # Add to our collection
                micro_op_pairs.append(MicroOpPair(pc1, pc2, cacheblock, op_num1, op_num2))
    
    return micro_op_pairs

def compute_intra_pair_distances(micro_op_pairs):
    """Compute distances between Micro-op 1 and Micro-op 2 within each pair"""
    distances = []
    
    for pair in micro_op_pairs:
        # Calculate distance: op_num2 - op_num1
        distance = pair.op_num2 - pair.op_num1
        distances.append(distance)
    
    return distances

def compute_inter_pair_distances(micro_op_pairs):
    """Compute distances between consecutive pairs accessing the same cache block"""
    # Group pairs by cache block
    cache_block_groups = defaultdict(list)
    for pair in micro_op_pairs:
        cache_block_groups[pair.cacheblock].append(pair)
    
    distances = []
    
    # Process each cache block group
    for cache_block, pairs in cache_block_groups.items():
        # Sort pairs by the first micro-op number to ensure proper order
        sorted_pairs = sorted(pairs, key=lambda x: x.op_num1)
        
        # Compare consecutive pairs
        for i in range(1, len(sorted_pairs)):
            prev_pair = sorted_pairs[i-1]
            curr_pair = sorted_pairs[i]
            
            # Distance = current pair's first micro-op - previous pair's second micro-op
            distance = curr_pair.op_num1 - prev_pair.op_num2
            
            # Only include non-negative distances (pairs in program order)
            if distance >= 0:
                distances.append(distance)
    
    return distances

def plot_histogram(data, title, xlabel, output_path, bin_edges, show_percentage=True, max_y=None):
    """Generate and save histogram with percentage option"""
    plt.figure(figsize=(10, 6))
    
    # Convert bin edges to numeric values
    bin_edges = [float(x) for x in bin_edges.split(',')] + [float('inf')]
    
    # Calculate histogram counts
    hist, _ = np.histogram(data, bins=bin_edges)
    
    # Create bin labels
    bin_labels = []
    for i in range(len(bin_edges)-1):
        if i == 0 and bin_edges[i] == 0 and bin_edges[i+1] == 1:
            bin_labels.append('0')
        elif bin_edges[i+1] == float('inf'):
            bin_labels.append(f'>{int(bin_edges[i])}')
        elif bin_edges[i] == 1 and bin_edges[i+1] == 11:
            bin_labels.append('1-10')
        elif bin_edges[i] == 11 and bin_edges[i+1] == 21:
            bin_labels.append('11-20')
        elif bin_edges[i] == 21 and bin_edges[i+1] == 31:
            bin_labels.append('21-30')
        elif bin_edges[i] == 31 and bin_edges[i+1] == 41:
            bin_labels.append('31-40')
        elif bin_edges[i] == 41 and bin_edges[i+1] == 51:
            bin_labels.append('41-50')
        elif bin_edges[i] == 51 and bin_edges[i+1] == 61:
            bin_labels.append('51-60')
        elif bin_edges[i] == 61 and bin_edges[i+1] == 71:
            bin_labels.append('61-70')
        elif bin_edges[i] == 71 and bin_edges[i+1] == 81:
            bin_labels.append('71-80')
        elif bin_edges[i] == 81 and bin_edges[i+1] == 91:
            bin_labels.append('81-90')
        elif bin_edges[i] == 91 and bin_edges[i+1] == 101:
            bin_labels.append('91-100')
        else:
            bin_labels.append(f'{int(bin_edges[i])}-{int(bin_edges[i+1]-1)}')
    
    # Calculate percentages
    total = sum(hist)
    percentages = np.zeros_like(hist, dtype=float)
    if total > 0:
        percentages = (hist / total) * 100
    
    # Determine what to plot
    if show_percentage:
        values = percentages
        ylabel = 'Percentage (%)'
        filename_suffix = '_percent'
    else:
        values = hist
        ylabel = 'Count'
        filename_suffix = '_count'
    
    # Plot
    bars = plt.bar(range(len(values)), values, color='#2F8E9E', width=0.7)
    plt.title(title, fontsize=14)
    plt.ylabel(ylabel, fontsize=12)
    plt.xlabel(xlabel, fontsize=12)
    plt.xticks(range(len(bin_labels)), bin_labels, rotation=45, ha='right')
    
    # Add value labels
    max_height = max(values) if len(values) > 0 else 0
    label_height = max_height * 0.03 if max_height > 0 else 1
    
    for bar in bars:
        height = bar.get_height()
        if height > 0:
            if show_percentage:
                plt.text(bar.get_x() + bar.get_width()/2, height + label_height,
                       f'{height:.1f}%', ha='center', va='bottom')
            else:
                plt.text(bar.get_x() + bar.get_width()/2, height + label_height,
                       f'{int(height):,}', ha='center', va='bottom')
    
    # Axis limits
    if max_y:
        plt.ylim(0, max_y)
    plt.grid(axis='y', linestyle='--', alpha=0.7)
    plt.tight_layout()
    
    # Save the plot
    plt.savefig(f'{output_path}{filename_suffix}.png', dpi=300, bbox_inches='tight')
    plt.savefig(f'{output_path}{filename_suffix}.pdf', format='pdf', bbox_inches='tight')
    plt.close()
    
    # Save raw data to CSV
    with open(f'{output_path}{filename_suffix}.csv', 'w') as f:
        f.write('bin,count,percentage\n')
        for i, (count, pct) in enumerate(zip(hist, percentages)):
            f.write(f'{bin_labels[i]},{count},{pct:.2f}\n')
    
    return hist, percentages

def plot_comparison_side_by_side(workload_data, bin_edges, title, xlabel, output_path, max_y=None):
    """Create side-by-side bar chart comparing all workloads"""
    plt.figure(figsize=(14, 8))
    
    # Process bins
    bin_edges = [float(x) for x in bin_edges.split(',')] + [float('inf')]
    
    # Create bin labels
    bin_labels = []
    for i in range(len(bin_edges)-1):
        if i == 0 and bin_edges[i] == 0 and bin_edges[i+1] == 1:
            bin_labels.append('0')
        elif bin_edges[i+1] == float('inf'):
            bin_labels.append('>10000')
        elif bin_edges[i] == 1 and bin_edges[i+1] == 11:
            bin_labels.append('1-10')
        elif bin_edges[i] == 11 and bin_edges[i+1] == 21:
            bin_labels.append('11-20')
        elif bin_edges[i] == 21 and bin_edges[i+1] == 31:
            bin_labels.append('21-30')
        elif bin_edges[i] == 31 and bin_edges[i+1] == 41:
            bin_labels.append('31-40')
        elif bin_edges[i] == 41 and bin_edges[i+1] == 51:
            bin_labels.append('41-50')
        elif bin_edges[i] == 51 and bin_edges[i+1] == 61:
            bin_labels.append('51-60')
        elif bin_edges[i] == 61 and bin_edges[i+1] == 71:
            bin_labels.append('61-70')
        elif bin_edges[i] == 71 and bin_edges[i+1] == 81:
            bin_labels.append('71-80')
        elif bin_edges[i] == 81 and bin_edges[i+1] == 91:
            bin_labels.append('81-90')
        elif bin_edges[i] == 91 and bin_edges[i+1] == 101:
            bin_labels.append('91-100')
        elif bin_edges[i] == 101 and bin_edges[i+1] == 1000:
            bin_labels.append('101-999')
        elif bin_edges[i] == 1000 and bin_edges[i+1] == 10000:
            bin_labels.append('1000-9999')
        else:
            bin_labels.append(f'{int(bin_edges[i])}-{int(bin_edges[i+1]-1)}')
    
    # Calculate histograms and percentages for each workload
    workload_percentages = {}
    x = np.arange(len(bin_labels))
    width = 0.8 / len(workload_data) if workload_data else 0.8
    
    # Use a colorful palette for different workloads
    colors = plt.cm.tab10(np.linspace(0, 1, len(workload_data)))
    
    # Calculate data
    for name, distances in workload_data.items():
        hist, _ = np.histogram(distances, bins=bin_edges)
        total = sum(hist)
        if total > 0:
            percentages = (hist / total) * 100
        else:
            percentages = np.zeros_like(hist, dtype=float)
        workload_percentages[name] = percentages
    
    # Plot bars side by side
    for i, (name, percentages) in enumerate(workload_percentages.items()):
        plt.bar(x + (i - len(workload_data)/2 + 0.5) * width, 
               percentages, width, label=name, color=colors[i])
    
    plt.xlabel(xlabel, fontsize=12)
    plt.ylabel('Percentage (%)', fontsize=12)
    plt.title(title, fontsize=14)
    plt.xticks(x, bin_labels, rotation=45, ha='right')
    plt.legend(loc='upper right', fontsize=10)
    plt.grid(axis='y', linestyle='--', alpha=0.7)
    
    # Axis limits
    if max_y:
        plt.ylim(0, max_y)
    else:
        # Find max percentage across all workloads plus some margin
        max_pct = max([max(pct) if len(pct) > 0 else 0 for pct in workload_percentages.values()])
        plt.ylim(0, max_pct * 1.1)
    
    plt.tight_layout()
    
    # Save outputs
    plt.savefig(f'{output_path}.png', dpi=300, bbox_inches='tight')
    plt.savefig(f'{output_path}.pdf', format='pdf', bbox_inches='tight')
    plt.close()
    
    # Save raw data
    with open(f'{output_path}.csv', 'w') as f:
        header = ['bin'] + list(workload_percentages.keys())
        f.write(','.join(header) + '\n')
        
        for i in range(len(bin_labels)):
            row = [bin_labels[i]]
            for name in workload_percentages.keys():
                row.append(f'{workload_percentages[name][i]:.2f}')
            f.write(','.join(row) + '\n')

def main():
    args = parse_args()
    
    # Create output directory
    os.makedirs(args.output, exist_ok=True)
    
    # Store data for combined analysis
    intra_pair_data = {}  # Within-pair distances by workload
    inter_pair_data = {}  # Between-pair distances by workload
    
    # Process each workload file
    for txt_file in glob.glob(os.path.join(args.workload_dir, '*.txt')):
        workload_name = os.path.basename(txt_file).split('.')[0]
        print(f"Processing {workload_name}...")
        
        # Parse workload file
        micro_op_pairs = parse_workload_file(txt_file)
        print(f"  Found {len(micro_op_pairs)} micro-op pairs")
        
        if len(micro_op_pairs) < 1:
            print("  No micro-op pairs found. Skipping.")
            continue
        
        # Calculate intra-pair distances (within each pair)
        intra_distances = compute_intra_pair_distances(micro_op_pairs)
        print(f"  Calculated {len(intra_distances)} intra-pair distances")
        
        # Store for combined analysis
        intra_pair_data[workload_name] = intra_distances
        
        # Generate individual intra-pair histogram
        intra_title = f'Distance Between Micro-op 1 and Micro-op 2 Within Pairs: {workload_name}'
        intra_xlabel = 'Micro-op Distance Within Pair'
        intra_output = os.path.join(args.output, f'{workload_name}_intra_pair')
        plot_histogram(intra_distances, intra_title, intra_xlabel, intra_output, args.bins, True, args.max_y)
        plot_histogram(intra_distances, intra_title, intra_xlabel, intra_output, args.bins, False, args.max_y)
        
        # Calculate inter-pair distances (between consecutive pairs)
        inter_distances = compute_inter_pair_distances(micro_op_pairs)
        print(f"  Calculated {len(inter_distances)} inter-pair distances")
        
        # Store for combined analysis
        inter_pair_data[workload_name] = inter_distances
        
        # Generate individual inter-pair histogram
        inter_title = f'Distance Between Consecutive Pairs Accessing Same Cache Block: {workload_name}'
        inter_xlabel = 'Micro-op Distance Between Pairs'
        inter_output = os.path.join(args.output, f'{workload_name}_inter_pair')
        plot_histogram(inter_distances, inter_title, inter_xlabel, inter_output, args.bins, True, args.max_y)
        plot_histogram(inter_distances, inter_title, inter_xlabel, inter_output, args.bins, False, args.max_y)
    
    # Create side-by-side comparison visualizations if we have multiple workloads
    if len(intra_pair_data) > 1:
        print("\nGenerating comparison visualizations...")
        
        # Intra-pair comparison
        intra_title = 'Comparison of Distances Within Micro-op Pairs Across Applications'
        intra_xlabel = 'Micro-op Distance Within Pair'
        intra_output = os.path.join(args.output, 'comparison_intra_pair')
        plot_comparison_side_by_side(intra_pair_data, args.bins, intra_title, intra_xlabel, intra_output, args.max_y)
        
        # Inter-pair comparison
        inter_title = 'Comparison of Distances Between Consecutive Pairs Across Applications'
        inter_xlabel = 'Micro-op Distance Between Pairs'
        inter_output = os.path.join(args.output, 'comparison_inter_pair')
        plot_comparison_side_by_side(inter_pair_data, args.bins, inter_title, inter_xlabel, inter_output, args.max_y)
    
    print(f"\nAnalysis complete. Results saved to: {args.output}")

if __name__ == '__main__':
    main()