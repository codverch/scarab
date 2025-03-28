#!/usr/bin/env python3
"""
Fusion Analyzer

This script analyzes distances between micro-op pairs that access the same cacheline
and generates histograms for each workload.

Usage:
    python fusion_analyzer.py <input_directory> [--output OUTPUT_DIR]
"""

import os
import re
import glob
import argparse
import numpy as np
import matplotlib.pyplot as plt
from collections import defaultdict, namedtuple

# Define a structure to hold micro-op pair information
MicroOpPair = namedtuple('MicroOpPair', ['pc1', 'pc2', 'cacheline', 'op_num1', 'op_num2'])

def parse_args():
    parser = argparse.ArgumentParser(description='Analyze distances between micro-op pairs accessing the same cacheline')
    parser.add_argument('input', help='Directory containing the workload files')
    parser.add_argument('--output', '-o', default='fusion_analysis', help='Output directory for results')
    parser.add_argument('--debug', action='store_true', help='Print debug information')
    parser.add_argument('--title', default='Inter Micro-op PC Pair Distance (in µops) for Pairs Accessing the Same Cacheline', 
                       help='Title template for histograms')
    parser.add_argument('--bins', default='0,1,10,20,30,40,50,60,70,80,90,100', 
                       help='Comma-separated list of bin edges')
    parser.add_argument('--max-y', type=float, default=None,
                       help='Maximum value for y-axis (default: auto-calculated)')
    return parser.parse_args()

def parse_file(file_path, debug=False):
    """Parse a file to extract micro-op pairs."""
    micro_op_pairs = []
    
    with open(file_path, 'r') as f:
        line_num = 0
        for line in f:
            line_num += 1
            # Skip header lines
            if "Micro-op 1:" not in line:
                continue
                
            # Try to extract micro-op pair information with flexible spacing
            pattern = r'Micro-op\s+1:\s+([0-9a-fA-Fx]+)\s+Micro-op\s+2:\s+([0-9a-fA-Fx]+)\s+Cacheblock\s+Address:\s+([0-9a-fA-Fx]+)\s+Micro-op\s+1\s+Number:\s+(\d+)'
            match = re.search(pattern, line)
            
            if match:
                pc1 = match.group(1)
                pc2 = match.group(2)
                cacheline = match.group(3)
                op_num1 = int(match.group(4))
                
                # Try to get op_num2 if it exists
                op_num2_pattern = r'Micro-op\s+2\s+Number:\s+(\d+)'
                op_num2_match = re.search(op_num2_pattern, line)
                
                if op_num2_match:
                    op_num2 = int(op_num2_match.group(1))
                    micro_op_pairs.append(MicroOpPair(pc1, pc2, cacheline, op_num1, op_num2))
                    
                    if debug and len(micro_op_pairs) <= 5:
                        print(f"Line {line_num}: Micro-op 1 ({op_num1}) and Micro-op 2 ({op_num2})")
                else:
                    # Some lines might be missing op_num2, skip them
                    if debug:
                        print(f"Warning: Line {line_num} missing op_num2: {line.strip()}")
    
    return micro_op_pairs

def compute_distances(micro_op_pairs):
    """Compute distances between micro-op pairs in the same cacheline."""
    # Group pairs by cacheline
    cacheline_groups = defaultdict(list)
    for pair in micro_op_pairs:
        cacheline_groups[pair.cacheline].append(pair)
    
    # Compute distances between pairs in the same cacheline
    distances = []
    
    for cacheline, pairs in cacheline_groups.items():
        # Only compute distances if there are at least 2 pairs for this cacheline
        if len(pairs) < 2:
            continue
            
        # Compare each pair with every other pair
        for i in range(len(pairs)):
            for j in range(i+1, len(pairs)):
                # Calculate distance between op_num values
                distance = abs(pairs[i].op_num2 - pairs[j].op_num2)
                distances.append(distance)
    
    return distances

def generate_individual_histograms(workload_data, bins, output_dir, title_template, max_y=None):
    """Create individual histograms for each workload."""
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
    
    # Filter out workloads with no data
    workload_data = {k: v for k, v in workload_data.items() if v}
    
    if not workload_data:
        print("No data available for visualization")
        return
    
    # Set up high-quality plotting parameters
    plt.rcParams['figure.dpi'] = 300
    plt.rcParams['savefig.dpi'] = 300
    plt.rcParams['font.family'] = 'serif'
    plt.rcParams['font.serif'] = ['Times New Roman'] + plt.rcParams['font.serif']
    
    # Convert string bins to numeric list if needed
    if isinstance(bins, str):
        bins = [float(x) for x in bins.split(',')]
    
    # Add infinity as the last bin edge if not present
    if bins[-1] != float('inf'):
        bins.append(float('inf'))
    
    # Create custom bin labels
    bin_labels = []
    for i in range(len(bins)-1):
        if i == 0:
            bin_labels.append('0')  # First bin is exactly 0
        elif i == len(bins) - 2:  # Last bin before infinity
            bin_labels.append(f'>{int(bins[i])}')
        else:
            bin_labels.append(f'{int(bins[i])}-{int(bins[i+1]-1)}')
    
    # Define nice colors
    bar_color = '#2F8E9E'
    
    # Map workload names to display names
    workload_names_map = {
        'clang': 'Clang',
        'gcc': 'Gcc',
        'mysql': 'Mysql',
        'mongodb': 'Mongodb',
        'postgres': 'Postgres'
    }
    
    # Compute overall max count if not specified
    if max_y is None:
        max_count = 0
        for workload, micro_op_pairs in workload_data.items():
            distances = compute_distances(micro_op_pairs)
            hist, _ = np.histogram(distances, bins=bins)
            max_count = max(max_count, max(hist))
        
        # Round max count up to nearest 100, add some padding for labels
        max_y = 100 * np.ceil(max_count / 100) + 100
    
    # Process each workload
    for workload, micro_op_pairs in workload_data.items():
        distances = compute_distances(micro_op_pairs)
        
        # Compute histogram
        hist, _ = np.histogram(distances, bins=bins)
        
        # Create figure
        plt.figure(figsize=(10, 6))
        
        # Plot bars
        bars = plt.bar(range(len(hist)), hist, color=bar_color, width=0.7)
        
        # Add count labels on top of each bar
        for bar in bars:
            height = bar.get_height()
            plt.text(
                bar.get_x() + bar.get_width()/2,  # x-position (center of bar)
                height + max_y * 0.02,            # y-position (just above bar)
                f'{int(height):,}',               # text (integer count)
                ha='center',                      # horizontal alignment
                va='bottom',                      # vertical alignment
                fontsize=10,                      # font size
                rotation=0                        # horizontal text
            )
        
        # Set title and labels
        display_name = workload_names_map.get(workload, workload.capitalize())
        plt.title(title_template.format(workload=display_name), fontsize=16, pad=10)
        plt.ylabel('Number of Inter Micro-op Pairs\nAccessing Same Cacheline', fontsize=11)
        
        # Set xticks and labels
        plt.xticks(range(len(bin_labels)), bin_labels, fontsize=9, rotation=45, ha='right')
        
        # Set consistent y-axis limits
        plt.ylim(0, max_y)
        
        # Add grid lines
        plt.grid(True, axis='y', linestyle='--', alpha=0.3)
        
        # Remove top and right spines
        plt.gca().spines['top'].set_visible(False)
        plt.gca().spines['right'].set_visible(False)
        
        # Adjust layout
        plt.tight_layout()
        
        # Save figure for this workload
        output_base = os.path.join(output_dir, f'{workload}_inter_pair_distance_histogram')
        plt.savefig(f'{output_base}.png', dpi=300, bbox_inches='tight')
        plt.savefig(f'{output_base}.pdf', format='pdf', bbox_inches='tight')
        
        plt.close()
        
        print(f"Histogram for {workload} saved to {output_base}.png and {output_base}.pdf")

def write_summary_report(workload_data, bins, output_dir):
    """Write a summary report with metadata and distance statistics for all workloads."""
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
    
    report_path = os.path.join(output_dir, 'inter_pair_distance_summary.txt')
    
    with open(report_path, 'w') as f:
        f.write("===== Inter Micro-op Pair Distance Analysis Summary =====\n\n")
        
        f.write(f"{'Workload':<15} | {'Total Pairs':<15} | {'Avg Distance':<15} | {'Median':<10} | {'% ≤10':<10}\n")
        f.write(f"{'-'*15} | {'-'*15} | {'-'*15} | {'-'*10} | {'-'*10}\n")
        
        for workload in sorted(workload_data.keys()):
            micro_op_pairs = workload_data[workload]
            
            if micro_op_pairs:
                distances = compute_distances(micro_op_pairs)
                small_dist_pct = sum(1 for d in distances if d <= 10) * 100 / len(distances)
                
                f.write(f"{workload:<15} | {len(micro_op_pairs):<15,d} | {np.mean(distances):<15.2f} | {np.median(distances):<10.2f} | {small_dist_pct:<10.2f}%\n")
            else:
                f.write(f"{workload:<15} | {'0':<15} | {'N/A':<15} | {'N/A':<10} | {'N/A':<10}\n")
                
        f.write("\n\n")
        
        # Detailed statistics by workload
        for workload, micro_op_pairs in sorted(workload_data.items()):
            if not micro_op_pairs:
                continue
                
            f.write(f"===== {workload} =====\n")
            distances = compute_distances(micro_op_pairs)
            
            # Compute histogram
            hist, _ = np.histogram(distances, bins=bins)
            
            f.write(f"{'Range':<12} | {'Count':<10} | {'Percentage':<10}\n")
            f.write(f"{'-'*12} | {'-'*10} | {'-'*10}\n")
            
            bin_labels = []
            for i in range(len(bins)-1):
                if i == 0:
                    bin_labels.append('0')
                elif bins[i+1] == float('inf'):
                    bin_labels.append(f"> {int(bins[i])}")
                else:
                    bin_labels.append(f"{int(bins[i])}-{int(bins[i+1]-1)}")
            
            for i, count in enumerate(hist):
                percentage = count * 100 / len(distances)
                f.write(f"{bin_labels[i]:<12} | {count:<10,d} | {percentage:<10.2f}%\n")
                
            f.write("\n")
    
    print(f"Summary report written to {report_path}")

def write_csv_data(workload_data, bins, output_dir):
    """Write CSV data for all workloads."""
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
    
    # Write histogram data CSV
    hist_data_path = os.path.join(output_dir, 'histogram_data.csv')
    
    with open(hist_data_path, 'w') as f:
        # Create header with bin labels
        bin_labels = []
        for i in range(len(bins)-1):
            if i == 0:
                bin_labels.append('0')
            elif bins[i+1] == float('inf'):
                bin_labels.append(f">{int(bins[i])}")
            else:
                bin_labels.append(f"{int(bins[i])}-{int(bins[i+1]-1)}")
        
        f.write("workload," + ",".join(bin_labels) + "\n")
        
        # Write data for each workload
        for workload, micro_op_pairs in sorted(workload_data.items()):
            if not micro_op_pairs:
                continue
                
            distances = compute_distances(micro_op_pairs)
            hist, _ = np.histogram(distances, bins=bins)
            
            f.write(f"{workload}," + ",".join(f"{c}" for c in hist) + "\n")
    
    print(f"Histogram data CSV written to {hist_data_path}")
    
    # Write raw pair data for each workload
    for workload, micro_op_pairs in workload_data.items():
        if not micro_op_pairs:
            continue
            
        csv_path = os.path.join(output_dir, f'{workload}_inter_pairs.csv')
        
        with open(csv_path, 'w') as f:
            f.write("pc1,pc2,cacheline,op_num1,op_num2\n")
            
            for pair in micro_op_pairs:
                f.write(f"{pair.pc1},{pair.pc2},{pair.cacheline},{pair.op_num1},{pair.op_num2}\n")
        
        print(f"CSV data written to {csv_path}")

def main():
    args = parse_args()
    
    # Check if input directory exists
    if not os.path.exists(args.input):
        print(f"Error: Input directory '{args.input}' does not exist.")
        return
    
    # Create output directory
    if not os.path.exists(args.output):
        os.makedirs(args.output)
    
    # Parse bins
    bins = [float(x) for x in args.bins.split(',')]
    if bins[-1] != float('inf'):
        bins.append(float('inf'))
    
    # Get list of workload files
    if os.path.isdir(args.input):
        file_pattern = os.path.join(args.input, "*.txt")
        files = glob.glob(file_pattern)
    else:
        # If input is a single file, just use that
        files = [args.input]
    
    if not files:
        print(f"No workload files found in {args.input}")
        return
    
    # Process each workload file
    workload_data = {}
    
    for file_path in files:
        workload = os.path.splitext(os.path.basename(file_path))[0]
        print(f"Processing {workload}...")
        
        # Parse file to extract micro-op pairs
        micro_op_pairs = parse_file(file_path, args.debug)
        print(f"  Found {len(micro_op_pairs)} micro-op pairs")
        
        # Store micro-op pairs for this workload
        workload_data[workload] = micro_op_pairs
    
    # Generate individual histograms
    title_template = "Inter Micro-op PC Pair Distance: {workload}"
    generate_individual_histograms(workload_data, bins, args.output, title_template, args.max_y)
    
    # Write combined summary report
    write_summary_report(workload_data, bins, args.output)
    
    # Write CSV data
    write_csv_data(workload_data, bins, args.output)
    
    print(f"Analysis complete. Results saved to {args.output}/")

if __name__ == "__main__":
    main()