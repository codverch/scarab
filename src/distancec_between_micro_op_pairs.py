#!/usr/bin/env python3
"""
Custom Fusion Analyzer

This script analyzes distances between Micro-op 1 and Micro-op 2 within each fusion pair
and generates a combined histogram comparing all workloads side by side with custom
bins and scales but matching the aesthetic style of the provided example.

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
from matplotlib.gridspec import GridSpec
import matplotlib as mpl

# Define a structure to hold fusion pair information
FusionPair = namedtuple('FusionPair', ['pc1', 'pc2', 'cacheline', 'op_num1', 'op_num2', 'distance'])

def parse_args():
    parser = argparse.ArgumentParser(description='Analyze distances within fusion pairs')
    parser.add_argument('input', help='Directory containing the workload files')
    parser.add_argument('--output', '-o', default='fusion_analysis', help='Output directory for results')
    parser.add_argument('--debug', action='store_true', help='Print debug information')
    parser.add_argument('--title', default='Intra Micro-op PC Pair Distance (in µops) for Pairs Accessing the Same Cacheline', 
                       help='Title for the combined histogram')
    parser.add_argument('--bins', default='0,1,10,20,30,40,50,60,70,80,90,100', 
                       help='Comma-separated list of bin edges')
    parser.add_argument('--max-y', type=float, default=None,
                       help='Maximum value for y-axis (default: auto-calculated)')
    return parser.parse_args()

def parse_file(file_path, debug=False):
    """Parse a file to extract fusion pairs and their internal distances."""
    fusion_pairs = []
    
    with open(file_path, 'r') as f:
        line_num = 0
        for line in f:
            line_num += 1
            # Skip header lines
            if "Micro-op 1:" not in line:
                continue
                
            # Try to extract fusion pair information with flexible spacing
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
                    # Calculate the distance directly: op_num2 - op_num1 - 1
                    # The -1 accounts for not counting the endpoints
                    distance = op_num2 - op_num1 - 1
                    fusion_pairs.append(FusionPair(pc1, pc2, cacheline, op_num1, op_num2, distance))
                    
                    if debug and len(fusion_pairs) <= 5:
                        print(f"Line {line_num}: Distance between Micro-op 1 ({op_num1}) and Micro-op 2 ({op_num2}) = {distance}")
                else:
                    # Some lines might be missing op_num2, skip them
                    if debug:
                        print(f"Warning: Line {line_num} missing op_num2: {line.strip()}")
    
    return fusion_pairs

def generate_combined_histogram(workload_data, bins, output_dir, title, max_y=None):
    """Create a combined histogram with all workloads side by side in one figure."""
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
    
    # Filter out workloads with no data
    workload_data = {k: v for k, v in workload_data.items() if v}
    
    if not workload_data:
        print("No data available for visualization")
        return
    
    # Set up the figure with high quality settings
    plt.rcParams['figure.dpi'] = 300
    plt.rcParams['savefig.dpi'] = 300
    plt.rcParams['font.family'] = 'serif'
    plt.rcParams['font.serif'] = ['Times New Roman'] + plt.rcParams['font.serif']
    
    fig = plt.figure(figsize=(15, 8))
    
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
    
    # Create a subplot for each workload
    num_workloads = len(workload_data)
    
    # Create a grid of subplots
    gs = GridSpec(1, num_workloads, figure=fig)
    
    # Define nice colors - using teal similar to example
    bar_color = '#2F8E9E'
    
    # Find max percentage for consistent y-axis
    if max_y is None:
        max_percentage = 0
        for workload, fusion_pairs in workload_data.items():
            distances = [pair.distance for pair in fusion_pairs]
            hist, _ = np.histogram(distances, bins=bins)
            percentages = hist * 100 / len(distances)
            max_percentage = max(max_percentage, max(percentages))
        
        # Round max percentage up to nearest 5, add some padding for labels
        max_y = 5 * np.ceil(max_percentage / 5) + 5  # Added padding for label visibility
    
    # Map workload names to display names
    workload_names_map = {
        'clang': 'Clang',
        'gcc': 'Gcc',
        'mysql': 'Mysql',
        'mongodb': 'Mongodb',
        'postgres': 'Postgres'
    }
    
    # Sort workloads alphabetically to match the example
    workload_names = sorted(workload_data.keys())
    
    # Plot each workload
    for i, workload in enumerate(workload_names):
        fusion_pairs = workload_data[workload]
        distances = [pair.distance for pair in fusion_pairs]
        
        # Compute histogram
        hist, _ = np.histogram(distances, bins=bins)
        percentages = hist * 100 / len(distances)
        
        # Create subplot
        ax = fig.add_subplot(gs[0, i])
        
        # Plot bars
        bars = ax.bar(range(len(hist)), percentages, color=bar_color, width=0.7)
        
        # Add percentage labels on top of each bar
        for bar in bars:
            height = bar.get_height()
            ax.text(
                bar.get_x() + bar.get_width()/2,  # x-position (center of bar)
                height + 0.5,                      # y-position (just above bar)
                f'{height:.0f}%',                  # text (rounded to nearest integer)
                ha='center',                       # horizontal alignment
                va='bottom',                       # vertical alignment
                fontsize=9,                        # font size
                rotation=0                         # horizontal text
            )
        
        # Set title and labels
        display_name = workload_names_map.get(workload, workload.capitalize())
        ax.set_title(display_name, fontsize=16, pad=10)
        
        # Only add y-label to the first subplot
        if i == 0:
            ax.set_ylabel('Load Micro-op Pairs Accessing\nSame Cacheline (%)', fontsize=11)
        

        # Set xticks and labels
        ax.set_xticks(range(len(bin_labels)))
        ax.set_xticklabels(bin_labels, fontsize=9, rotation=45, ha='right')
        
        # Set consistent y-axis limits
        ax.set_ylim(0, max_y)
        
        # Add grid lines
        ax.grid(True, axis='y', linestyle='--', alpha=0.3)
        
        # Remove top and right spines
        ax.spines['top'].set_visible(False)
        ax.spines['right'].set_visible(False)
    
    # Add main title
    plt.suptitle(title, fontsize=18, y=0.95)
    

    # Adjust layout
    plt.tight_layout(rect=[0, 0.05, 1, 0.93])
    
    # Save figure in high resolution
    output_path = os.path.join(output_dir, 'combined_distance_histogram.png')
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    
    # Also save as PDF for vector graphics
    pdf_output_path = os.path.join(output_dir, 'combined_distance_histogram.pdf')
    plt.savefig(pdf_output_path, format='pdf', bbox_inches='tight')
    
    plt.close()
    
    print(f"Combined histogram saved to {output_path} and {pdf_output_path}")

def write_summary_report(workload_data, bins, output_dir):
    """Write a summary report with metadata and distance statistics for all workloads."""
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
    
    report_path = os.path.join(output_dir, 'combined_summary.txt')
    
    with open(report_path, 'w') as f:
        f.write("===== Combined Fusion Pair Distance Analysis Summary =====\n\n")
        
        f.write(f"{'Workload':<15} | {'Total Pairs':<15} | {'Avg Distance':<15} | {'Median':<10} | {'% ≤10':<10}\n")
        f.write(f"{'-'*15} | {'-'*15} | {'-'*15} | {'-'*10} | {'-'*10}\n")
        
        for workload in sorted(workload_data.keys()):
            fusion_pairs = workload_data[workload]
            
            if fusion_pairs:
                distances = [pair.distance for pair in fusion_pairs]
                small_dist_pct = sum(1 for d in distances if d <= 10) * 100 / len(distances)
                
                f.write(f"{workload:<15} | {len(fusion_pairs):<15,d} | {np.mean(distances):<15.2f} | {np.median(distances):<10.2f} | {small_dist_pct:<10.2f}%\n")
            else:
                f.write(f"{workload:<15} | {'0':<15} | {'N/A':<15} | {'N/A':<10} | {'N/A':<10}\n")
                
        f.write("\n\n")
        
        # Detailed statistics by workload
        for workload, fusion_pairs in sorted(workload_data.items()):
            if not fusion_pairs:
                continue
                
            f.write(f"===== {workload} =====\n")
            distances = [pair.distance for pair in fusion_pairs]
            
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
    
    print(f"Combined summary report written to {report_path}")

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
        for workload, fusion_pairs in sorted(workload_data.items()):
            if not fusion_pairs:
                continue
                
            distances = [pair.distance for pair in fusion_pairs]
            hist, _ = np.histogram(distances, bins=bins)
            percentages = hist * 100 / len(distances)
            
            f.write(f"{workload}," + ",".join(f"{p:.2f}" for p in percentages) + "\n")
    
    print(f"Histogram data CSV written to {hist_data_path}")
    
    # Write raw pair data for each workload
    for workload, fusion_pairs in workload_data.items():
        if not fusion_pairs:
            continue
            
        csv_path = os.path.join(output_dir, f'{workload}_fusion_pairs.csv')
        
        with open(csv_path, 'w') as f:
            f.write("pc1,pc2,cacheline,op_num1,op_num2,distance\n")
            
            for pair in fusion_pairs:
                f.write(f"{pair.pc1},{pair.pc2},{pair.cacheline},{pair.op_num1},{pair.op_num2},{pair.distance}\n")
        
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
        
        # Parse file to extract fusion pairs
        fusion_pairs = parse_file(file_path, args.debug)
        print(f"  Found {len(fusion_pairs)} fusion pairs")
        
        # Store fusion pairs for this workload
        workload_data[workload] = fusion_pairs
    
    # Generate combined histogram
    generate_combined_histogram(workload_data, bins, args.output, args.title, args.max_y)
    
    # Write combined summary report
    write_summary_report(workload_data, bins, args.output)
    
    # Write CSV data
    write_csv_data(workload_data, bins, args.output)
    
    print(f"Analysis complete. Results saved to {args.output}/")

if __name__ == "__main__":
    main()