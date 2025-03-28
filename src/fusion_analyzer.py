#!/usr/bin/env python3
"""
Fusion Pair Distance Analyzer

This script analyzes distances between Micro-op 1 and Micro-op 2 within each fusion pair.
It generates histograms showing the distribution of these distances for each workload.

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

# Define a structure to hold fusion pair information
FusionPair = namedtuple('FusionPair', ['pc1', 'pc2', 'cacheline', 'op_num1', 'op_num2', 'distance'])

def parse_args():
    parser = argparse.ArgumentParser(description='Analyze distances within fusion pairs')
    parser.add_argument('input', help='Directory containing the workload files')
    parser.add_argument('--output', '-o', default='fusion_analysis', help='Output directory for results')
    parser.add_argument('--debug', action='store_true', help='Print debug information')
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

def generate_histogram(workload, fusion_pairs, output_dir):
    """Create a histogram visualization for fusion pair distances."""
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
    
    if not fusion_pairs:
        print(f"No fusion pairs to visualize for {workload}")
        return
    
    # Extract distances
    distances = [pair.distance for pair in fusion_pairs]
    
    # Define the specific bins as requested
    bins = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, float('inf')]
    bin_labels = ["0-10", "10-20", "20-30", "30-40", "40-50", "50-60", "60-70", "70-80", "80-90", "90-100", ">100"]
    
    # Compute histogram
    hist, _ = np.histogram(distances, bins=bins)
    
    # Convert to percentages
    percentages = hist * 100 / len(distances)
    
    # Create the histogram
    plt.figure(figsize=(12, 6))
    bars = plt.bar(range(len(hist)), percentages, alpha=0.7)
    
    # Add percentage labels on top of each bar
    for i, bar in enumerate(bars):
        height = bar.get_height()
        if height > 0:  # Only add labels to visible bars
            plt.text(bar.get_x() + bar.get_width()/2., height + 0.5,
                    f'{percentages[i]:.1f}%',
                    ha='center', va='bottom', rotation=0)
    
    plt.title(f'{workload}: Distribution of Distances Within Fusion Pairs')
    plt.xlabel('Distance (micro-ops)')
    plt.ylabel('Percentage of Pairs (%)')
    plt.xticks(range(len(bin_labels)), bin_labels)
    plt.grid(True, alpha=0.3, linestyle='--', axis='y')
    
    # Add text with total count
    plt.figtext(0.15, 0.85, f"Total Fusion Pairs: {len(fusion_pairs):,}", 
                bbox=dict(facecolor='white', alpha=0.8))
    
    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, f'{workload}_fusion_distance_histogram.png'), dpi=300)
    plt.close()
    
    # Create a more detailed histogram for small distances (0-20)
    small_distances = [d for d in distances if d <= 20]
    if small_distances:
        plt.figure(figsize=(12, 6))
        
        # Use individual bins for 0-20
        small_bins = list(range(21))
        small_hist, _ = np.histogram(small_distances, bins=small_bins)
        small_percentages = small_hist * 100 / len(distances)  # percentage of ALL distances
        
        bars = plt.bar(range(len(small_hist)), small_percentages, alpha=0.7)
        
        # Add percentage labels
        for i, bar in enumerate(bars):
            height = bar.get_height()
            if height > 0:  # Only add labels to visible bars
                plt.text(bar.get_x() + bar.get_width()/2., height + 0.1,
                        f'{small_percentages[i]:.1f}%',
                        ha='center', va='bottom', rotation=90 if small_percentages[i] < 3 else 0,
                        fontsize=8 if small_percentages[i] < 5 else 10)
        
        plt.title(f'{workload}: Distribution of Small Distances (≤20) Within Fusion Pairs')
        plt.xlabel('Distance (micro-ops)')
        plt.ylabel('Percentage of All Pairs (%)')
        plt.xticks(range(21))
        plt.grid(True, alpha=0.3, linestyle='--', axis='y')
        
        plt.tight_layout()
        plt.savefig(os.path.join(output_dir, f'{workload}_small_fusion_distance_histogram.png'), dpi=300)
        plt.close()

def write_summary_report(workload, fusion_pairs, output_dir):
    """Write a summary report with metadata and distance statistics."""
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
    
    report_path = os.path.join(output_dir, f'{workload}_summary.txt')
    
    with open(report_path, 'w') as f:
        f.write(f"===== {workload}: Fusion Pair Distance Analysis Summary =====\n\n")
        
        # Metadata section
        f.write("===== Metadata =====\n")
        f.write(f"Total fusion pairs: {len(fusion_pairs)}\n")
        
        # Count unique cachelines
        unique_cachelines = len(set(pair.cacheline for pair in fusion_pairs))
        f.write(f"Total unique cachelines accessed: {unique_cachelines}\n")
        
        # Count unique PC pairs
        unique_pc_pairs = len(set((pair.pc1, pair.pc2) for pair in fusion_pairs))
        f.write(f"Unique PC pairs: {unique_pc_pairs}\n\n")
        
        # Distance statistics
        if fusion_pairs:
            distances = [pair.distance for pair in fusion_pairs]
            
            f.write("===== Distance Statistics =====\n")
            f.write(f"Average distance: {np.mean(distances):.2f} micro-ops\n")
            f.write(f"Median distance: {np.median(distances):.2f} micro-ops\n")
            f.write(f"Minimum distance: {min(distances)} micro-ops\n")
            f.write(f"Maximum distance: {max(distances)} micro-ops\n\n")
            
            # Distance distribution
            f.write("===== Distance Distribution =====\n")
            bins = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, float('inf')]
            bin_labels = ["0-10", "10-20", "20-30", "30-40", "40-50", "50-60", "60-70", "70-80", "80-90", "90-100", ">100"]
            
            hist, _ = np.histogram(distances, bins=bins)
            
            f.write(f"{'Range':<10} | {'Count':<10} | {'Percentage':<10}\n")
            f.write(f"{'-'*10} | {'-'*10} | {'-'*10}\n")
            
            for i, count in enumerate(hist):
                percentage = count * 100 / len(distances)
                f.write(f"{bin_labels[i]:<10} | {count:<10,d} | {percentage:<10.2f}%\n")
            
            # Focus on small distances (0-20)
            f.write("\n===== Small Distances (0-20) =====\n")
            f.write(f"{'Distance':<10} | {'Count':<10} | {'Percentage':<10}\n")
            f.write(f"{'-'*10} | {'-'*10} | {'-'*10}\n")
            
            for distance in range(21):
                count = sum(1 for d in distances if d == distance)
                percentage = count * 100 / len(distances)
                f.write(f"{distance:<10} | {count:<10,d} | {percentage:<10.2f}%\n")
            
            # Top cachelines
            cacheline_counts = defaultdict(int)
            for pair in fusion_pairs:
                cacheline_counts[pair.cacheline] += 1
                
            f.write("\n===== Top Cachelines by Pair Count =====\n")
            top_cachelines = sorted(cacheline_counts.items(), key=lambda x: x[1], reverse=True)[:10]
            
            for i, (cacheline, count) in enumerate(top_cachelines, 1):
                percentage = count * 100 / len(fusion_pairs)
                f.write(f"{i}. Cacheline {cacheline}: {count} pairs ({percentage:.2f}%)\n")
                
            # Example pairs with very small distances
            small_pairs = [pair for pair in fusion_pairs if pair.distance <= 5]
            if small_pairs:
                f.write("\n===== Examples of Pairs with Very Small Distances (≤5) =====\n")
                for i, pair in enumerate(sorted(small_pairs, key=lambda x: x.distance)[:10], 1):
                    f.write(f"{i}. Distance {pair.distance}: PC1={pair.pc1}, PC2={pair.pc2}, Cacheline={pair.cacheline}\n")
            
        else:
            f.write("No fusion pairs found for this workload.\n")
    
    print(f"Summary report written to {report_path}")

def write_csv_data(workload, fusion_pairs, output_dir):
    """Write raw fusion pair data to a CSV file."""
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
    
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
    
    # Debug: print a sample line from each file
    if args.debug:
        print("=== Debug Information ===")
        if os.path.isdir(args.input):
            files = glob.glob(os.path.join(args.input, "*.txt"))
            for file_path in files:
                try:
                    with open(file_path, 'r') as f:
                        for line in f:
                            if "Micro-op 1:" in line:
                                print(f"Sample line from {os.path.basename(file_path)}:")
                                print(f"  {line.strip()}")
                                break
                except Exception as e:
                    print(f"Error reading {file_path}: {e}")
        print("=== End Debug Information ===")
    
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
    combined_report_data = []
    
    for file_path in files:
        workload = os.path.splitext(os.path.basename(file_path))[0]
        print(f"Processing {workload}...")
        
        # Parse file to extract fusion pairs
        fusion_pairs = parse_file(file_path, args.debug)
        print(f"  Found {len(fusion_pairs)} fusion pairs")
        
        if fusion_pairs:
            # Generate histogram
            generate_histogram(workload, fusion_pairs, args.output)
            
            # Write summary report
            write_summary_report(workload, fusion_pairs, args.output)
            
            # Write CSV data
            write_csv_data(workload, fusion_pairs, args.output)
            
            # Collect data for combined report
            distances = [pair.distance for pair in fusion_pairs]
            small_dist_pct = sum(1 for d in distances if d <= 10) * 100 / len(distances)
            combined_report_data.append({
                'workload': workload,
                'total_pairs': len(fusion_pairs),
                'avg_distance': np.mean(distances),
                'median_distance': np.median(distances),
                'small_dist_pct': small_dist_pct
            })
    
    # Create combined summary report
    if combined_report_data:
        combined_report_path = os.path.join(args.output, 'combined_summary.txt')
        with open(combined_report_path, 'w') as f:
            f.write("===== Combined Summary for All Workloads =====\n\n")
            
            f.write(f"{'Workload':<15} | {'Total Pairs':<15} | {'Avg Distance':<15} | {'Median':<10} | {'% ≤10':<10}\n")
            f.write(f"{'-'*15} | {'-'*15} | {'-'*15} | {'-'*10} | {'-'*10}\n")
            
            for data in sorted(combined_report_data, key=lambda x: x['median_distance']):
                f.write(f"{data['workload']:<15} | {data['total_pairs']:<15,d} | {data['avg_distance']:<15.2f} | {data['median_distance']:<10.2f} | {data['small_dist_pct']:<10.2f}%\n")
        
        print(f"Combined summary written to {combined_report_path}")
        
        # Generate combined histogram
        plt.figure(figsize=(14, 8))
        
        # Prepare data
        workloads = [data['workload'] for data in combined_report_data]
        medians = [data['median_distance'] for data in combined_report_data]
        pct_small = [data['small_dist_pct'] for data in combined_report_data]
        
        # Sort by median distance
        sorted_indices = np.argsort(medians)
        sorted_workloads = [workloads[i] for i in sorted_indices]
        sorted_medians = [medians[i] for i in sorted_indices]
        sorted_pct_small = [pct_small[i] for i in sorted_indices]
        
        # Create bar chart
        bars = plt.bar(range(len(sorted_workloads)), sorted_medians, alpha=0.7)
        
        # Add labels
        for i, bar in enumerate(bars):
            height = bar.get_height()
            plt.text(bar.get_x() + bar.get_width()/2., height + 0.5,
                    f'{sorted_medians[i]:.1f}',
                    ha='center', va='bottom')
        
        plt.title('Median Distance Within Fusion Pairs by Workload')
        plt.xlabel('Workload')
        plt.ylabel('Median Distance (micro-ops)')
        plt.xticks(range(len(sorted_workloads)), sorted_workloads, rotation=45)
        plt.grid(True, alpha=0.3, linestyle='--', axis='y')
        
        plt.tight_layout()
        plt.savefig(os.path.join(args.output, 'combined_median_distances.png'), dpi=300)
        plt.close()
        
        # Create bar chart for percentage of small distances
        plt.figure(figsize=(14, 8))
        
        # Sort by percentage of small distances (descending)
        sorted_indices = np.argsort(pct_small)[::-1]
        sorted_workloads = [workloads[i] for i in sorted_indices]
        sorted_pct_small = [pct_small[i] for i in sorted_indices]
        
        bars = plt.bar(range(len(sorted_workloads)), sorted_pct_small, alpha=0.7)
        
        for i, bar in enumerate(bars):
            height = bar.get_height()
            plt.text(bar.get_x() + bar.get_width()/2., height + 0.5,
                    f'{sorted_pct_small[i]:.1f}%',
                    ha='center', va='bottom')
        
        plt.title('Percentage of Fusion Pairs with Small Distances (≤10) by Workload')
        plt.xlabel('Workload')
        plt.ylabel('Percentage (%)')
        plt.xticks(range(len(sorted_workloads)), sorted_workloads, rotation=45)
        plt.grid(True, alpha=0.3, linestyle='--', axis='y')
        
        plt.tight_layout()
        plt.savefig(os.path.join(args.output, 'combined_small_distances_percentage.png'), dpi=300)
        plt.close()
    
    print(f"Analysis complete. Results saved to {args.output}/")

if __name__ == "__main__":
    main()