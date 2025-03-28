#!/usr/bin/env python3
"""
Enhanced Super Hot Fusion Analyzer

Analyzes distances between micro-op pairs accessing "Super Hot" cachelines.
Features:
- Computes percentages of distance distributions
- Creates both individual and combined histograms across all applications
- Saves raw data and percentage data

Uses:
- Workload files (*.txt) containing micro-op pairs from fusion_with_micro_op_num/
- Analysis files (*_analysis_data.csv) from fusion_results/ for Super Hot classification

Usage:
    python fusion_analyzer.py <workload_dir> <analysis_dir> [--output OUTPUT_DIR]
"""

import os
import re
import csv
import glob
import argparse
import numpy as np
import matplotlib.pyplot as plt
from collections import defaultdict, namedtuple

#   python fusion_analyzer.py fusion_with_micro_op_num fusion_results --output superhot_analysis --stacked

MicroOpPair = namedtuple('MicroOpPair', ['pc1', 'pc2', 'cacheline', 'op_num1', 'op_num2'])

def parse_args():
    parser = argparse.ArgumentParser(description='Analyze Super Hot cacheline accesses')
    parser.add_argument('workload_dir', help='Directory containing workload TXT files')
    parser.add_argument('analysis_dir', help='Directory containing analysis CSV files')
    parser.add_argument('--output', '-o', default='superhot_analysis', help='Output directory')
    parser.add_argument('--bins', default='0,1,10,20,30,40,50,60,70,80,90,100', 
                      help='Comma-separated bin edges')
    parser.add_argument('--max-y', type=float, default=None, help='Max y-axis value')
    parser.add_argument('--stacked', action='store_true', help='Create stacked bar chart for comparison')
    return parser.parse_args()

def get_super_hot_cachelines(analysis_dir, workload_name):
    """Get Super Hot cachelines from analysis CSV for a specific workload"""
    pattern = os.path.join(analysis_dir, f"{workload_name}_*_analysis_data.csv")
    analysis_files = glob.glob(pattern)
    
    if not analysis_files:
        return set()
    
    super_hot = set()
    with open(analysis_files[0], 'r') as f:
        reader = csv.reader(f)
        for row in reader:
            if len(row) >= 3 and "Super Hot" in row[2]:
                # Normalize cacheline format
                cl = row[0].lower().lstrip('0x')
                super_hot.add(cl)
    return super_hot

def parse_workload_file(txt_path, super_hot_cachelines):
    """Parse workload TXT file and filter Super Hot pairs"""
    micro_op_pairs = []
    cacheline_pattern = re.compile(
        r'Micro-op\s+1:\s+([0-9a-fA-Fx]+)\s+'
        r'Micro-op\s+2:\s+([0-9a-fA-Fx]+)\s+'
        r'Cacheblock\s+Address:\s+([0-9a-fA-Fx]+)\s+'
        r'Micro-op\s+1\s+Number:\s+(\d+)\s+'
        r'Micro-op\s+2\s+Number:\s+(\d+)'
    )
    
    with open(txt_path, 'r') as f:
        for line in f:
            match = cacheline_pattern.search(line)
            if match:
                pc1, pc2, cacheline = match.group(1), match.group(2), match.group(3)
                op_num1, op_num2 = int(match.group(4)), int(match.group(5))
                
                # Normalize cacheline address
                normalized_cl = cacheline.lower().lstrip('0x')
                if normalized_cl in super_hot_cachelines:
                    micro_op_pairs.append(MicroOpPair(pc1, pc2, cacheline, op_num1, op_num2))
    
    return micro_op_pairs

def compute_inter_pair_distances(micro_op_pairs):
    """Compute distances between CONSECUTIVE pairs in program order"""
    cacheline_groups = defaultdict(list)
    for pair in micro_op_pairs:
        cacheline_groups[pair.cacheline].append(pair)
    
    distances = []
    
    for cl, pairs in cacheline_groups.items():
        # Sort pairs by their starting micro-op number (op_num1)
        sorted_pairs = sorted(pairs, key=lambda x: x.op_num1)
        
        # Compare consecutive pairs only
        for i in range(1, len(sorted_pairs)):
            prev_pair = sorted_pairs[i-1]
            current_pair = sorted_pairs[i]
            
            # Calculate gap: start of current - end of previous
            gap = current_pair.op_num1 - prev_pair.op_num2
            
            if gap >= 0:
                distances.append(gap)
            # (Optional: Add else clause to handle negative gaps/overlaps)
    
    return distances

def plot_histogram(workload_name, distances, bins, output_dir, max_y=None, show_percentage=True):
    """Generate and save histogram plot with percentage option"""
    plt.figure(figsize=(10, 6))
    
    # Process bins
    bin_edges = [float(x) for x in bins.split(',')] + [float('inf')]
    hist, _ = np.histogram(distances, bins=bin_edges)
    
    # Create labels
    bin_labels = []
    for i in range(len(bin_edges)-1):
        if i == 0:
            bin_labels.append('0')
        elif bin_edges[i+1] == float('inf'):
            bin_labels.append(f'>{int(bin_edges[i])}')
        else:
            bin_labels.append(f'{int(bin_edges[i])}-{int(bin_edges[i+1]-1)}')
    
    # Calculate percentages
    total = sum(hist)
    percentages = np.zeros_like(hist, dtype=float)
    if total > 0:
        percentages = (hist / total) * 100
    
    # Plot styling
    if show_percentage:
        values = percentages
        ylabel = 'Percentage of Micro-op Pairs (%)'
        filename_suffix = '_percent'
    else:
        values = hist
        ylabel = 'Number of Micro-op Pairs'
        filename_suffix = ''
    
    bars = plt.bar(range(len(values)), values, color='#2F8E9E', width=0.7)
    plt.title(f'Distances Between Load Micro-op PC Pairs: {workload_name}', fontsize=14)
    plt.ylabel(ylabel, fontsize=12)
    plt.xlabel('Distance Between Load Micro-op PC Pairs', fontsize=12)
    plt.xticks(range(len(bin_labels)), bin_labels, rotation=45, ha='right')
    
    # Add value labels
    max_height = max(values) if len(values) > 0 else 0
    label_height = max_height * 0.05 if max_height > 0 else 1
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
    
    # Save outputs
    base_path = os.path.join(output_dir, f'{workload_name}_superhot{filename_suffix}')
    plt.savefig(f'{base_path}.png', dpi=300, bbox_inches='tight')
    plt.savefig(f'{base_path}.pdf', format='pdf', bbox_inches='tight')
    plt.close()
    
    # Save raw data
    if show_percentage:
        with open(f'{base_path}.csv', 'w') as f:
            f.write('bin,count,percentage\n')
            for i, (count, pct) in enumerate(zip(hist, percentages)):
                f.write(f'{bin_labels[i]},{count},{pct:.2f}\n')
    else:
        with open(f'{base_path}.csv', 'w') as f:
            f.write('distance\n')
            f.write('\n'.join(map(str, distances)))
    
    return hist, percentages

def plot_combined_histogram(workload_data, bins, output_dir, max_y=None, show_percentage=True):
    """Create combined histogram for all workloads"""
    plt.figure(figsize=(12, 8))
    
    # Process bins
    bin_edges = [float(x) for x in bins.split(',')] + [float('inf')]
    
    # Create labels
    bin_labels = []
    for i in range(len(bin_edges)-1):
        if i == 0:
            bin_labels.append('0')
        elif bin_edges[i+1] == float('inf'):
            bin_labels.append(f'>{int(bin_edges[i])}')
        else:
            bin_labels.append(f'{int(bin_edges[i])}-{int(bin_edges[i+1]-1)}')
    
    # Prepare combined data
    all_distances = []
    for name, distances in workload_data.items():
        all_distances.extend(distances)
    
    # Calculate histogram
    combined_hist, _ = np.histogram(all_distances, bins=bin_edges)
    
    # Calculate percentages
    total = sum(combined_hist)
    percentages = np.zeros_like(combined_hist, dtype=float)
    if total > 0:
        percentages = (combined_hist / total) * 100
    
    # Plot styling
    if show_percentage:
        values = percentages
        ylabel = 'Percentage of Micro-op Pairs (%)'
        filename_suffix = '_percent'
    else:
        values = combined_hist
        ylabel = 'Number of Micro-op Pairs'
        filename_suffix = ''
    
    bars = plt.bar(range(len(values)), values, color='#2F8E9E', width=0.7)
    plt.title('Combined Distances Between Load Micro-op PC Pairs (All Applications)', fontsize=14)
    plt.ylabel(ylabel, fontsize=12)
    plt.xlabel('Distance Between Load Micro-op PC Pairs', fontsize=12)
    plt.xticks(range(len(bin_labels)), bin_labels, rotation=45, ha='right')
    
    # Add value labels
    max_height = max(values) if len(values) > 0 else 0
    label_height = max_height * 0.05 if max_height > 0 else 1
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
    
    # Save outputs
    base_path = os.path.join(output_dir, f'combined_superhot{filename_suffix}')
    plt.savefig(f'{base_path}.png', dpi=300, bbox_inches='tight')
    plt.savefig(f'{base_path}.pdf', format='pdf', bbox_inches='tight')
    plt.close()
    
    # Save raw data
    with open(f'{base_path}.csv', 'w') as f:
        if show_percentage:
            f.write('bin,count,percentage\n')
            for i, (count, pct) in enumerate(zip(combined_hist, percentages)):
                f.write(f'{bin_labels[i]},{count},{pct:.2f}\n')
        else:
            f.write('bin,count\n')
            for i, count in enumerate(combined_hist):
                f.write(f'{bin_labels[i]},{count}\n')
    
    return combined_hist, percentages

def plot_stacked_comparison(workload_data, bins, output_dir, max_y=None):
    """Create stacked bar chart comparing all workloads"""
    plt.figure(figsize=(14, 10))
    
    # Process bins
    bin_edges = [float(x) for x in bins.split(',')] + [float('inf')]
    
    # Create labels
    bin_labels = []
    for i in range(len(bin_edges)-1):
        if i == 0:
            bin_labels.append('0')
        elif bin_edges[i+1] == float('inf'):
            bin_labels.append(f'>{int(bin_edges[i])}')
        else:
            bin_labels.append(f'{int(bin_edges[i])}-{int(bin_edges[i+1]-1)}')
    
    # Calculate histograms and percentages for each workload
    workload_percentages = {}
    x = np.arange(len(bin_labels))
    width = 0.8 / len(workload_data) if workload_data else 0.8
    
    # Use a color map for different workloads
    colors = plt.cm.viridis(np.linspace(0, 0.9, len(workload_data)))
    
    # Calculate data
    for i, (name, distances) in enumerate(workload_data.items()):
        hist, _ = np.histogram(distances, bins=bin_edges)
        total = sum(hist)
        if total > 0:
            percentages = (hist / total) * 100
        else:
            percentages = np.zeros_like(hist, dtype=float)
        workload_percentages[name] = percentages
    
    # Plot bars side by side
    bars = []
    for i, (name, percentages) in enumerate(workload_percentages.items()):
        bar = plt.bar(x + (i - len(workload_data)/2 + 0.5) * width, 
                     percentages, width, label=name, color=colors[i])
        bars.append(bar)
    
    plt.xlabel('Distance Between Load Micro-op PC Pairs', fontsize=12)
    plt.ylabel('Percentage of Micro-op Pairs (%)', fontsize=12)
    plt.title('Comparison of Distances Across Applications', fontsize=14)
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
    base_path = os.path.join(output_dir, 'comparison_superhot_percent')
    plt.savefig(f'{base_path}.png', dpi=300, bbox_inches='tight')
    plt.savefig(f'{base_path}.pdf', format='pdf', bbox_inches='tight')
    plt.close()
    
    # Save raw data
    with open(f'{base_path}.csv', 'w') as f:
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
    
    # Store all workload data for combined analysis
    all_workload_data = {}
    
    # Process each workload
    for txt_file in glob.glob(os.path.join(args.workload_dir, '*.txt')):
        workload_name = os.path.basename(txt_file).split('.')[0]
        print(f"Processing {workload_name}...")
        
        # Get Super Hot cachelines from analysis directory
        super_hot_cls = get_super_hot_cachelines(args.analysis_dir, workload_name)
        print(f"  Found {len(super_hot_cls)} Super Hot cachelines")
        
        # Parse workload file with Super Hot filter
        micro_op_pairs = parse_workload_file(txt_file, super_hot_cls)
        print(f"  Found {len(micro_op_pairs)} Super Hot micro-op pairs")
        
        if len(micro_op_pairs) < 2:
            print("  Insufficient pairs for distance calculation")
            continue
        
        # Calculate inter-pair distances
        distances = compute_inter_pair_distances(micro_op_pairs)
        print(f"  Calculated {len(distances)} inter-pair distances")
        
        # Store distances for combined analysis
        all_workload_data[workload_name] = distances
        
        # Generate individual outputs (both raw counts and percentages)
        plot_histogram(workload_name, distances, args.bins, args.output, args.max_y, show_percentage=False)
        plot_histogram(workload_name, distances, args.bins, args.output, args.max_y, show_percentage=True)
    
    if all_workload_data:
        print("\nGenerating combined analysis...")
        # Generate combined histogram
        plot_combined_histogram(all_workload_data, args.bins, args.output, args.max_y, show_percentage=False)
        plot_combined_histogram(all_workload_data, args.bins, args.output, args.max_y, show_percentage=True)
        
        # Generate comparison chart if requested
        if args.stacked and len(all_workload_data) > 1:
            print("Generating comparison chart...")
            plot_stacked_comparison(all_workload_data, args.bins, args.output, args.max_y)
    
    print(f"\nAnalysis complete. Results saved to: {args.output}")

if __name__ == '__main__':
    main()


  