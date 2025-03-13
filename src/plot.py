#!/usr/bin/env python3
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import numpy as np
from collections import Counter, defaultdict

def parse_fusion_events(filename):
    """Parse the fusion event file with fixed-width format"""
    
    # Skip the first two lines (header and separator)
    with open(filename, 'r') as f:
        lines = f.readlines()[2:]
    
    data = []
    
    for line in lines:
        if not line.strip():  # Skip empty lines
            continue
            
        parts = line.strip().split()
        if len(parts) < 8:  # Basic validation
            continue
            
        data.append({
            'DonorPC': parts[0],
            'ReceiverPC': parts[1],
            'DonorMemAddr': parts[2],
            'ReceiverMemAddr': parts[3],
            'DonorCacheline': parts[4],
            'ReceiverCacheline': parts[5],
            'OffsetInCL1': int(parts[6]),
            'OffsetInCL2': int(parts[7])
        })
    
    return pd.DataFrame(data)

def analyze_fusion_events(df):
    """Analyze fusion events data for PC pairs and cachelines"""
    
    # Create PC pairs (sorted to make (A,B) and (B,A) count as the same pair)
    df['PCPair'] = df.apply(lambda row: 
                       tuple(sorted([row['DonorPC'], row['ReceiverPC']])), 
                       axis=1)
    
    # Get unique counts
    unique_pc_pairs = df['PCPair'].nunique()
    unique_pcs = set(df['DonorPC'].unique()) | set(df['ReceiverPC'].unique())
    unique_donor_cachelines = df['DonorCacheline'].nunique()
    unique_receiver_cachelines = df['ReceiverCacheline'].nunique()
    unique_cachelines = set(df['DonorCacheline'].unique()) | set(df['ReceiverCacheline'].unique())
    
    print(f"Total fusion events: {len(df)}")
    print(f"Unique PC pairs: {unique_pc_pairs}")
    print(f"Unique individual PCs: {len(unique_pcs)}")
    print(f"Unique cachelines: {len(unique_cachelines)}")
    print(f"Unique donor cachelines: {unique_donor_cachelines}")
    print(f"Unique receiver cachelines: {unique_receiver_cachelines}")
    
    # Count occurrences of each PC pair
    pc_pair_counts = Counter(df['PCPair'])
    
    # Map PC pairs to the cachelines they access
    pc_pair_to_cachelines = defaultdict(set)
    for _, row in df.iterrows():
        pc_pair = row['PCPair']
        pc_pair_to_cachelines[pc_pair].add(row['DonorCacheline'])
        pc_pair_to_cachelines[pc_pair].add(row['ReceiverCacheline'])
    
    # Count cachelines per PC pair
    cachelines_per_pc_pair = {pc_pair: len(cachelines) for pc_pair, cachelines in pc_pair_to_cachelines.items()}
    
    # Map cachelines to the PC pairs that access them
    cacheline_to_pc_pairs = defaultdict(set)
    for _, row in df.iterrows():
        cacheline_to_pc_pairs[row['DonorCacheline']].add(row['PCPair'])
        cacheline_to_pc_pairs[row['ReceiverCacheline']].add(row['PCPair'])
    
    # Count PC pairs per cacheline
    pc_pairs_per_cacheline = {cacheline: len(pc_pairs) for cacheline, pc_pairs in cacheline_to_pc_pairs.items()}
    
    # Find PC pairs with multiple occurrences
    repeat_pc_pairs = {pc_pair: count for pc_pair, count in pc_pair_counts.items() if count > 1}
    single_pc_pairs = {pc_pair: count for pc_pair, count in pc_pair_counts.items() if count == 1}
    
    print(f"PC pairs that occur multiple times: {len(repeat_pc_pairs)}")
    print(f"PC pairs that occur only once: {len(single_pc_pairs)}")
    
    # Calculate statistics
    avg_cachelines_per_pc_pair = sum(len(cachelines) for cachelines in pc_pair_to_cachelines.values()) / len(pc_pair_to_cachelines)
    avg_pc_pairs_per_cacheline = sum(len(pc_pairs) for pc_pairs in cacheline_to_pc_pairs.values()) / len(cacheline_to_pc_pairs)
    
    print(f"Average cachelines accessed per PC pair: {avg_cachelines_per_pc_pair:.2f}")
    print(f"Average PC pairs per cacheline: {avg_pc_pairs_per_cacheline:.2f}")
    
    return {
        'pc_pair_counts': pc_pair_counts,
        'cachelines_per_pc_pair': cachelines_per_pc_pair,
        'pc_pairs_per_cacheline': pc_pairs_per_cacheline,
        'pc_pair_to_cachelines': pc_pair_to_cachelines,
        'cacheline_to_pc_pairs': cacheline_to_pc_pairs
    }

def visualize_analysis(analysis_results):
    """Create visualizations for the analysis results"""
    pc_pair_counts = analysis_results['pc_pair_counts']
    cachelines_per_pc_pair = analysis_results['cachelines_per_pc_pair']
    pc_pairs_per_cacheline = analysis_results['pc_pairs_per_cacheline']
    
    # Set up the plots
    plt.figure(figsize=(15, 10))
    
    # Plot 1: Distribution of PC pair occurrences
    plt.subplot(2, 2, 1)
    occurrences = list(pc_pair_counts.values())
    # Count frequency of each occurrence count
    occurrence_counts = Counter(occurrences)
    x = sorted(occurrence_counts.keys())
    y = [occurrence_counts[i] for i in x]
    
    plt.bar(x, y)
    plt.xlabel('Number of Occurrences')
    plt.ylabel('Number of PC Pairs')
    plt.title('Distribution of PC Pair Occurrences')
    plt.yscale('log')
    plt.grid(True, alpha=0.3)
    
    # Plot 2: Distribution of cachelines per PC pair
    plt.subplot(2, 2, 2)
    cacheline_counts = list(cachelines_per_pc_pair.values())
    # Count frequency of each cacheline count
    cacheline_count_freq = Counter(cacheline_counts)
    x = sorted(cacheline_count_freq.keys())
    y = [cacheline_count_freq[i] for i in x]
    
    plt.bar(x, y)
    plt.xlabel('Number of Cachelines')
    plt.ylabel('Number of PC Pairs')
    plt.title('Distribution of Cachelines per PC Pair')
    plt.yscale('log')
    plt.grid(True, alpha=0.3)
    
    # Plot 3: Distribution of PC pairs per cacheline
    plt.subplot(2, 2, 3)
    pc_pair_counts_per_cl = list(pc_pairs_per_cacheline.values())
    # Count frequency of each PC pair count
    pc_pair_count_freq = Counter(pc_pair_counts_per_cl)
    x = sorted(pc_pair_count_freq.keys())
    y = [pc_pair_count_freq[i] for i in x]
    
    plt.bar(x, y)
    plt.xlabel('Number of PC Pairs')
    plt.ylabel('Number of Cachelines')
    plt.title('Distribution of PC Pairs per Cacheline')
    plt.yscale('log')
    plt.grid(True, alpha=0.3)
    
    # Plot 4: Top 20 most frequent PC pairs
    plt.subplot(2, 2, 4)
    top_pc_pairs = sorted(pc_pair_counts.items(), key=lambda x: x[1], reverse=True)[:20]
    x = [f"Pair {i+1}" for i in range(len(top_pc_pairs))]
    y = [count for _, count in top_pc_pairs]
    
    plt.bar(x, y)
    plt.xlabel('PC Pair Rank')
    plt.ylabel('Occurrence Count')
    plt.title('Top 20 Most Frequent PC Pairs')
    plt.xticks(rotation=45)
    plt.grid(True, alpha=0.3)
    
    plt.tight_layout()
    plt.savefig('fusion_analysis.png', dpi=300)
    
    # Additional plot: Scatter plot of PC pairs vs cachelines
    plt.figure(figsize=(10, 8))
    
    # For each PC pair, get its occurrence count and number of cachelines it accesses
    pc_occurrences = []
    cacheline_counts = []
    for pc_pair in analysis_results['pc_pair_to_cachelines']:
        pc_occurrences.append(pc_pair_counts[pc_pair])
        cacheline_counts.append(len(analysis_results['pc_pair_to_cachelines'][pc_pair]))
    
    # Create a scatter plot with hexbin for density
    plt.hexbin(pc_occurrences, cacheline_counts, gridsize=20, cmap='viridis', bins='log')
    plt.colorbar(label='Log Count')
    plt.xlabel('PC Pair Occurrence Count')
    plt.ylabel('Number of Cachelines Accessed')
    plt.title('Relationship Between PC Pair Frequency and Cacheline Diversity')
    plt.grid(True, alpha=0.3)
    
    plt.tight_layout()
    plt.savefig('pc_cacheline_relationship.png', dpi=300)
    
    print("Visualizations saved to fusion_analysis.png and pc_cacheline_relationship.png")

def main():
    # Parse the fusion events file
    df = parse_fusion_events('fusion_events.txt')
    
    # Analyze the data
    analysis_results = analyze_fusion_events(df)
    
    # Visualize the results
    visualize_analysis(analysis_results)
    
    # Generate a report to answer Tanvir's question
    pc_pair_counts = analysis_results['pc_pair_counts']
    
    # Compute how many PC pairs account for different percentages of fusion events
    total_events = sum(pc_pair_counts.values())
    unique_pairs = len(pc_pair_counts)
    
    sorted_counts = sorted(pc_pair_counts.values(), reverse=True)
    cumulative = 0
    thresholds = [25, 50, 75, 90, 95, 99]
    pairs_needed = []
    
    for i, count in enumerate(sorted_counts):
        cumulative += count
        percentage = (cumulative / total_events) * 100
        
        for j, threshold in enumerate(thresholds):
            if percentage >= threshold and j < len(thresholds) and thresholds[j] > 0:
                pairs_needed.append((threshold, i + 1))
                thresholds[j] = -1  # Mark as processed
    
    # Write a report
    with open('fusion_analysis_report.txt', 'w') as f:
        f.write("Fusion Events Analysis Report\n")
        f.write("============================\n\n")
        f.write(f"Total fusion events: {total_events}\n")
        f.write(f"Unique PC pairs: {unique_pairs}\n\n")
        
        f.write("PC Pair Coverage:\n")
        for threshold, count in pairs_needed:
            f.write(f"{threshold}% of all fusion events are covered by {count} PC pairs ")
            f.write(f"({count/unique_pairs*100:.2f}% of all unique pairs)\n")
            
        # Calculate single vs multiple occurrence statistics
        single_occurrence = sum(1 for count in pc_pair_counts.values() if count == 1)
        multiple_occurrences = unique_pairs - single_occurrence
        
        f.write(f"\nPC pairs that occur only once: {single_occurrence} ({single_occurrence/unique_pairs*100:.2f}%)\n")
        f.write(f"PC pairs that occur multiple times: {multiple_occurrences} ({multiple_occurrences/unique_pairs*100:.2f}%)\n")
        
        # Calculate percentages of events from single vs multiple occurrences
        single_events = single_occurrence  # Each single occurrence pair contributes 1 event
        multiple_events = total_events - single_events
        
        f.write(f"\nFusion events from single-occurrence PC pairs: {single_events} ({single_events/total_events*100:.2f}%)\n")
        f.write(f"Fusion events from multiple-occurrence PC pairs: {multiple_events} ({multiple_events/total_events*100:.2f}%)\n")
        
        f.write("\nConclusion: ")
        if single_occurrence / unique_pairs > 0.5:
            f.write("Most PC pairs appear only once, making prediction challenging.\n")
            if single_events / total_events > 0.5:
                f.write("These single-occurrence pairs also account for most fusion events, ")
                f.write("confirming Deepanjali's observation about prediction difficulty due to cold misses.\n")
            else:
                f.write("However, the majority of fusion events come from PC pairs that recur multiple times, ")
                f.write("suggesting that a prediction mechanism could still be effective for most events.\n")
        else:
            f.write("Most PC pairs appear multiple times, suggesting potential for prediction.\n")
            if single_events / total_events > 0.3:
                f.write("However, a significant portion of fusion events still come from PC pairs that appear only once, ")
                f.write("confirming the cold miss challenge noted by Deepanjali.\n")
            else:
                f.write("The majority of fusion events also come from these recurring PC pairs, ")
                f.write("suggesting strong potential for effective PC-based prediction.\n")
    
    print("Analysis report saved to fusion_analysis_report.txt")

if __name__ == "__main__":
    main()