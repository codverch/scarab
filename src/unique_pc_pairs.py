import os
import re
import matplotlib.pyplot as plt
import numpy as np
from collections import Counter
import matplotlib
matplotlib.rcParams['font.family'] = 'serif'
matplotlib.rcParams['font.serif'] = ['Times New Roman']

def analyze_unique_pc_pairs():
    directory = "/users/deepmish/scarab/src/unique_pc_pairs_fusion_candidates"
    
    results = {}
    
    # Process each file in the directory
    for filename in os.listdir(directory):
        if filename.endswith(".txt"):
            workload = filename.split('.')[0]
            file_path = os.path.join(directory, filename)
            
            # Extract micro-op pairs from the file
            pairs = []
            with open(file_path, 'r') as f:
                for line in f:
                    # Updated regex to handle multiple spaces
                    match = re.match(r'Micro-op 1: ([0-9a-f]+)\s+Micro-op 2: ([0-9a-f]+)', line)
                    if match:
                        pc1, pc2 = match.groups()
                        pairs.append((pc1, pc2))
            
            # Count occurrences of each pair
            pair_counts = Counter(pairs)
            
            # Calculate metrics
            total_pairs = len(pairs)
            unique_pairs = len(pair_counts)
            
            # Avoid division by zero
            if total_pairs > 0:
                unique_fraction = unique_pairs / total_pairs
            else:
                unique_fraction = 0
            
            # Calculate frequency distribution of occurrences
            occurrence_counts = Counter(pair_counts.values())
            
            # Store results for this workload
            results[workload] = {
                'total_pairs': total_pairs,
                'unique_pairs': unique_pairs,
                'unique_fraction': unique_fraction,
                'occurrence_counts': occurrence_counts,
                'top_pairs': pair_counts.most_common(10)
            }
    
    return results

def generate_report(results):
    """Generate a text report of the analysis results"""
    
    print("\n===== Micro-op PC Pair Uniqueness Analysis =====\n")
    
    print(f"{'Workload':<10} | {'Total Pairs':<12} | {'Unique Pairs':<12} | {'Unique %':<10} | {'Top Pairs %':<10}")
    print("-" * 65)
    
    for workload, data in sorted(results.items()):
        total = data['total_pairs']
        unique = data['unique_pairs']
        fraction = data['unique_fraction'] * 100
        
        # Calculate what percentage of pairs are covered by the top 10 most common pairs
        top_pairs_count = sum(count for _, count in data['top_pairs'])
        top_pairs_percent = (top_pairs_count / total * 100) if total > 0 else 0
        
        print(f"{workload:<10} | {total:<12,d} | {unique:<12,d} | {fraction:<10.2f}% | {top_pairs_percent:<10.2f}%")
    
    print("\n=== Detailed Analysis ===\n")
    
    for workload, data in sorted(results.items()):
        print(f"\n{workload.upper()}:")
        print(f"  Total PC pairs: {data['total_pairs']:,}")
        print(f"  Unique PC pairs: {data['unique_pairs']:,}")
        print(f"  Unique fraction: {data['unique_fraction']:.4f} ({data['unique_fraction']*100:.2f}%)")
        
        if data['top_pairs']:
            print(f"  Top {len(data['top_pairs'])} most common PC pairs:")
            for i, ((pc1, pc2), count) in enumerate(data['top_pairs'], 1):
                percentage = (count / data['total_pairs'] * 100) if data['total_pairs'] > 0 else 0
                print(f"    {i}. ({pc1}, {pc2}): {count:,} occurrences ({percentage:.2f}%)")

def generate_top_pairs_chart(results):
    """Generate a bar chart showing the top PC pairs distribution"""
    workloads = sorted(results.keys())
    
    # Set up the figure
    fig, axes = plt.subplots(1, len(workloads), figsize=(20, 6), sharey=True)
    fig.subplots_adjust(top=0.85, bottom=0.20)
    
    for i, workload in enumerate(workloads):
        data = results[workload]
        ax = axes[i]
        
        # Get the top 8 pairs for visualization
        top_pairs = data['top_pairs'][:8]
        labels = [f"{i+1}" for i in range(len(top_pairs))]  # Just use numbers for x-axis
        values = [count / data['total_pairs'] * 100 for _, count in top_pairs]
        
        # Plot the bars
        bars = ax.bar(labels, values, color='teal')
        
        # Add percentage labels on top of bars
        for bar in bars:
            height = bar.get_height()
            ax.text(bar.get_x() + bar.get_width()/2., height + 1,
                   f'{height:.1f}%', ha='center', va='bottom', fontsize=9)
        
        # Set the title and labels
        ax.set_title(workload.capitalize(), fontweight='bold')
        ax.set_ylim(0, max(values) * 1.2)  # Add some space for percentage labels
        ax.grid(axis='y', linestyle='--', alpha=0.7)
        
        if i == 0:
            ax.set_ylabel('Load Micro-op PC Pairs\nAccessing Same Cacheline (%)')
        
        ax.set_xlabel('Top PC Pairs')
    
    # Add legend outside the plot
    fig.legend(['8-Byte Aligned Strides'], loc='lower center', bbox_to_anchor=(0.5, 0.02), ncol=1)
    
    plt.suptitle('Distribution of Top Memory Access Patterns Across Datacenter Applications', fontsize=16, fontweight='bold')
    plt.tight_layout(rect=[0, 0.08, 1, 0.95])  # Adjusted to make room for legend
    plt.savefig('top_pc_pairs_distribution.png', dpi=300, bbox_inches='tight')
    plt.close()

def generate_occurrence_distribution(results):
    """Generate a histogram of PC pair occurrences"""
    workloads = sorted(results.keys())
    
    # Define occurrence bins
    bins = [1, 2, 3, 4, 5, 10, 20, 50, 100, 500, 1000]
    bin_labels = ['1', '2', '3', '4', '5', '6-10', '11-20', '21-50', '51-100', '101-500', '500+']
    
    # Set up the figure with similar style to the provided graph
    fig, axes = plt.subplots(1, len(workloads), figsize=(20, 6), sharey=True)
    fig.subplots_adjust(top=0.85, bottom=0.20)
    
    for i, workload in enumerate(workloads):
        data = results[workload]
        ax = axes[i]
        
        # Prepare data for the bins
        binned_data = np.zeros(len(bins))
        total_unique = data['unique_pairs']
        
        for occurrences, count in data['occurrence_counts'].items():
            # Find which bin this occurrence count belongs to
            bin_idx = 0
            while bin_idx < len(bins) - 1 and occurrences > bins[bin_idx]:
                bin_idx += 1
            
            binned_data[bin_idx] += count
        
        # Convert to percentages
        percentages = (binned_data / total_unique * 100) if total_unique > 0 else np.zeros(len(bins))
        
        # Plot the bars
        bars = ax.bar(bin_labels, percentages, color='teal')
        
        # Add percentage labels on top of bars
        for bar in bars:
            height = bar.get_height()
            if height > 0.5:  # Only add labels if percentage is significant
                ax.text(bar.get_x() + bar.get_width()/2., height + 0.5,
                       f'{height:.0f}%', ha='center', va='bottom', fontsize=9)
        
        # Set the title and labels
        ax.set_title(workload.capitalize(), fontweight='bold')
        ax.set_ylim(0, 100)  # Percentage scale
        ax.grid(axis='y', linestyle='--', alpha=0.7)
        
        if i == 0:
            ax.set_ylabel('Load Micro-op PC Pairs\nAccessing Same Cacheline (%)')
        
        ax.set_xlabel('Occurrences per Unique PC Pair')
        
        # Rotate x-tick labels for better readability
        plt.setp(ax.get_xticklabels(), rotation=45, ha='right')
    
    # Add legend outside the plot
    fig.legend(['8-Byte Aligned Strides'], loc='lower center', bbox_to_anchor=(0.5, 0.02), ncol=1)
    
    plt.suptitle('Distribution of Memory Access Patterns Across Datacenter Applications', fontsize=16, fontweight='bold')
    plt.tight_layout(rect=[0, 0.08, 1, 0.95])  # Adjusted to make room for legend
    plt.savefig('pc_pair_occurrences_distribution.png', dpi=300, bbox_inches='tight')
    plt.close()

def generate_uniqueness_barchart(results):
    """Generate a bar chart showing the uniqueness fraction of PC pairs by workload"""
    workloads = sorted(results.keys())
    
    # Set up the figure
    fig, ax = plt.subplots(figsize=(12, 6))
    fig.subplots_adjust(bottom=0.15)
    
    # Prepare data
    unique_fractions = [results[workload]['unique_fraction'] * 100 for workload in workloads]
    repetition_coverage = []
    
    for workload in workloads:
        # Calculate what percentage of pairs are covered by repeated pairs
        total = results[workload]['total_pairs']
        repeated_pairs_count = total - results[workload]['unique_pairs']
        repetition_percentage = (repeated_pairs_count / total * 100) if total > 0 else 0
        repetition_coverage.append(repetition_percentage)
    
    # Plot the bars
    bar_width = 0.35
    indices = np.arange(len(workloads))
    
    bars1 = ax.bar(indices - bar_width/2, unique_fractions, bar_width, 
                  color='teal')
    bars2 = ax.bar(indices + bar_width/2, repetition_coverage, bar_width,
                  color='darkred')
    
    # Add percentage labels on top of bars
    for bar in bars1:
        height = bar.get_height()
        ax.text(bar.get_x() + bar.get_width()/2., height + 1,
               f'{height:.1f}%', ha='center', va='bottom', fontsize=9)
    
    for bar in bars2:
        height = bar.get_height()
        ax.text(bar.get_x() + bar.get_width()/2., height + 1,
               f'{height:.1f}%', ha='center', va='bottom', fontsize=9)
    
    # Set the title and labels
    ax.set_title('Memory Access Pattern Uniqueness Across Datacenter Applications', fontsize=16, fontweight='bold')
    ax.set_ylabel('Load Micro-op PC Pairs\nAccessing Same Cacheline (%)')
    ax.set_xlabel('Datacenter Applications')
    ax.set_xticks(indices)
    ax.set_xticklabels([w.capitalize() for w in workloads])
    
    # Add legend below the chart
    fig.legend(['Unique Pairs (%)', 'Coverage of Repeated Pairs (%)'], 
               loc='lower center', bbox_to_anchor=(0.5, 0.02), ncol=2)
    
    ax.grid(axis='y', linestyle='--', alpha=0.7)
    plt.tight_layout(rect=[0, 0.08, 1, 0.95])  # Adjusted to make room for legend
    plt.savefig('pc_pair_uniqueness_chart.png', dpi=300, bbox_inches='tight')
    plt.close()

def main():
    # Analyze the data
    results = analyze_unique_pc_pairs()
    
    # Generate the report
    generate_report(results)
    
    # Generate visualizations
    generate_top_pairs_chart(results)
    generate_occurrence_distribution(results)
    generate_uniqueness_barchart(results)
    
    print("Analysis complete. Visualizations saved to:")
    print("  - top_pc_pairs_distribution.png")
    print("  - pc_pair_occurrences_distribution.png")
    print("  - pc_pair_uniqueness_chart.png")

if __name__ == "__main__":
    main()