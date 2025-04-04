import re
import os
import numpy as np
import matplotlib.pyplot as plt
from collections import Counter, defaultdict

# Input file path
BFS_OUTPUT_FILE = "/users/deepmish/scarab/src/gap/bfs/bfs.txt"

def analyze_cacheline_distribution(input_file, max_distance=352):
    """
    Analyze the distribution of load micro-op PC pairs across cachelines,
    with three different filtering criteria:
    1. Same cacheblock only
    2. Same cacheblock + Same base register
    3. Same cacheblock + Same base register + Same memory size
    """
    print(f"Processing file: {input_file}")
    print(f"Filtering for pairs with distance <= {max_distance}")
    
    # Check if file exists
    if not os.path.exists(input_file):
        print(f"Error: File not found at {input_file}")
        return None
    
    # Data structures to store analysis results for each filtering criteria
    # 1. Same cacheblock only
    cache_only_pc_pairs = defaultdict(list)
    cache_only_count = Counter()
    
    # 2. Same cacheblock + Same base register
    cache_base_pc_pairs = defaultdict(list)
    cache_base_count = Counter()
    
    # 3. Same cacheblock + Same base register + Same memory size
    cache_base_mem_pc_pairs = defaultdict(list)  
    cache_base_mem_count = Counter()
    
    # Parse the file to extract cacheline and PC pair information
    matched_lines = 0
    pairs_by_criteria = {
        "Same Cacheblock": 0,
        "Same Cacheblock + Base Reg": 0,
        "Same Cacheblock + Base Reg + Mem Size": 0
    }
    
    with open(input_file, 'r') as f:
        for line_num, line in enumerate(f, 1):
            if line_num % 10000 == 0:
                print(f"Processing line {line_num}...")
                
            if "Op1 PC:" in line and "Op2 PC:" in line and "Cacheblock:" in line:
                matched_lines += 1
                try:
                    # Extract PC addresses
                    pc1_match = re.search(r'Op1 PC: (\w+)', line)
                    pc2_match = re.search(r'Op2 PC: (\w+)', line)
                    
                    # Extract cacheline addresses
                    cacheblock1_match = re.search(r'Op1 Cacheblock: (\w+)', line)
                    cacheblock2_match = re.search(r'Op2 Cacheblock: (\w+)', line)
                    
                    # Extract micro-op IDs
                    op1_id_match = re.search(r'Op1 micro-op id: (\d+)', line)
                    op2_id_match = re.search(r'Op2 micro-op id: (\d+)', line)
                    
                    # Extract base registers
                    op1_base_reg_match = re.search(r'Op1 base reg: (\d+)', line)
                    op2_base_reg_match = re.search(r'Op2 base reg: (\d+)', line)
                    
                    # Extract memory sizes (for third criteria)
                    op1_mem_size_match = re.search(r'Op1 mem size: (\d+)', line)
                    op2_mem_size_match = re.search(r'Op2 mem size: (\d+)', line)
                    
                    if all([pc1_match, pc2_match, cacheblock1_match, cacheblock2_match, 
                            op1_id_match, op2_id_match, op1_base_reg_match, op2_base_reg_match,
                            op1_mem_size_match, op2_mem_size_match]):
                        
                        pc1 = pc1_match.group(1)
                        pc2 = pc2_match.group(1)
                        cacheblock1 = cacheblock1_match.group(1)
                        cacheblock2 = cacheblock2_match.group(1)
                        op1_id = int(op1_id_match.group(1))
                        op2_id = int(op2_id_match.group(1))
                        op1_base_reg = int(op1_base_reg_match.group(1))
                        op2_base_reg = int(op2_base_reg_match.group(1))
                        op1_mem_size = int(op1_mem_size_match.group(1))
                        op2_mem_size = int(op2_mem_size_match.group(1))
                        
                        # Calculate distance
                        distance = abs(op2_id - op1_id)
                        
                        # Only process if within distance constraint
                        if distance <= max_distance:
                            # Create PC pair
                            pc_pair = (pc1, pc2)
                            
                            # Check if same cacheblock (Criteria 1)
                            if cacheblock1 == cacheblock2:
                                cacheline = cacheblock1
                                cache_only_pc_pairs[cacheline].append(pc_pair)
                                cache_only_count[pc_pair] += 1
                                pairs_by_criteria["Same Cacheblock"] += 1
                                
                                # Print sample for debugging (only the first few)
                                if pairs_by_criteria["Same Cacheblock"] <= 3:
                                    print(f"Same Cacheblock: PCs=({pc1}, {pc2}), Distance={distance}")
                                
                                # Check if same base register (Criteria 2)
                                if op1_base_reg == op2_base_reg:
                                    cache_base_pc_pairs[cacheline].append(pc_pair)
                                    cache_base_count[pc_pair] += 1
                                    pairs_by_criteria["Same Cacheblock + Base Reg"] += 1
                                    
                                    if pairs_by_criteria["Same Cacheblock + Base Reg"] <= 3:
                                        print(f"Same Cacheblock + Base Reg: PCs=({pc1}, {pc2}), BaseReg={op1_base_reg}")
                                    
                                    # Check if same memory size (Criteria 3)
                                    if op1_mem_size == op2_mem_size:
                                        cache_base_mem_pc_pairs[cacheline].append(pc_pair)
                                        cache_base_mem_count[pc_pair] += 1
                                        pairs_by_criteria["Same Cacheblock + Base Reg + Mem Size"] += 1
                                        
                                        if pairs_by_criteria["Same Cacheblock + Base Reg + Mem Size"] <= 3:
                                            print(f"All Constraints: PCs=({pc1}, {pc2}), MemSize={op1_mem_size}")
                except Exception as e:
                    print(f"Error parsing line {line_num}: {e}")
                    print(f"Line content: {line[:100]}...")
    
    print(f"Processed {matched_lines} matched lines from file")
    
    # Print statistics for each criteria
    for criteria, count in pairs_by_criteria.items():
        print(f"{criteria}: {count} pairs")
    
    # Process results for each criteria
    results = {}
    
    # Process Criteria 1: Same Cacheblock
    print("\nProcessing Same Cacheblock results...")
    results["Same Cacheblock"] = process_criteria_results(
        cache_only_pc_pairs, cache_only_count, "Same Cacheblock")
    
    # Process Criteria 2: Same Cacheblock + Base Reg
    print("\nProcessing Same Cacheblock + Base Reg results...")
    results["Same Cacheblock + Base Reg"] = process_criteria_results(
        cache_base_pc_pairs, cache_base_count, "Same Cacheblock + Base Reg")
    
    # Process Criteria 3: Same Cacheblock + Base Reg + Mem Size
    print("\nProcessing Same Cacheblock + Base Reg + Mem Size results...")
    results["Same Cacheblock + Base Reg + Mem Size"] = process_criteria_results(
        cache_base_mem_pc_pairs, cache_base_mem_count, "Same Cacheblock + Base Reg + Mem Size")
    
    # Plot combined results
    plot_combined_results(results, max_distance)
    
    return results

def process_criteria_results(cacheline_to_pc_pairs, pc_pairs_count, criteria_name):
    """Process results for a specific filtering criteria."""
    # Calculate total number of PC pairs and unique PC pairs
    total_pc_pairs = sum(pc_pairs_count.values())
    unique_pc_pairs = len(pc_pairs_count)
    
    if total_pc_pairs == 0:
        print(f"No pairs found for criteria: {criteria_name}")
        return None
    
    print(f"Found {total_pc_pairs} total pairs and {unique_pc_pairs} unique PC pairs for {criteria_name}")
    
    # Calculate statistics for cachelines
    cacheline_counts = {cacheline: len(pc_pairs) for cacheline, pc_pairs in cacheline_to_pc_pairs.items()}
    total_cachelines = len(cacheline_counts)
    print(f"Found {total_cachelines} unique cachelines for {criteria_name}")
    
    # Sort cachelines by access count (most accessed to least)
    sorted_cachelines = sorted(cacheline_counts.items(), key=lambda x: x[1], reverse=True)
    
    # Print top 5 hottest cachelines
    print(f"\nTop 5 hottest cachelines for {criteria_name}:")
    for i, (cacheline, count) in enumerate(sorted_cachelines[:min(5, len(sorted_cachelines))]):
        percentage = (count / total_pc_pairs) * 100
        print(f"{i+1}. Cacheline {cacheline}: {count} accesses ({percentage:.2f}% of total)")
    
    # Calculate cumulative distribution
    cumulative_accesses = []
    cumulative_cachelines = []
    cumulative_access_count = 0
    non_repeating_pc_pairs = []
    
    # Identify non-repeating (unique) PC pairs
    non_repeating = {pair: count for pair, count in pc_pairs_count.items() if count == 1}
    print(f"Found {len(non_repeating)} non-repeating PC pairs for {criteria_name}")
    
    # Calculate statistics for the graph
    for i, (cacheline, access_count) in enumerate(sorted_cachelines):
        cumulative_access_count += access_count
        cumulative_percentage = (cumulative_access_count / total_pc_pairs) * 100
        cacheline_percentage = ((i + 1) / total_cachelines) * 100
        
        # Count non-repeating PC pairs in this cacheline
        non_repeating_in_cacheline = sum(1 for pair in cacheline_to_pc_pairs[cacheline] if pair in non_repeating)
        non_repeating_pc_pairs.append(non_repeating_in_cacheline)
        
        cumulative_accesses.append(cumulative_percentage)
        cumulative_cachelines.append(cacheline_percentage)
    
    # Define temperature thresholds
    super_hot_threshold = 5
    hot_threshold = 15
    warm_threshold = 50
    
    # Ensure the CDF starts at 0
    cum_cachelines = [0] + list(cumulative_cachelines)
    cum_accesses = [0] + list(cumulative_accesses)
    
    # Extract values at key thresholds
    threshold_values = {}
    for threshold in [super_hot_threshold, hot_threshold, warm_threshold]:
        threshold_y = np.interp(threshold, cum_cachelines, cum_accesses)
        threshold_values[threshold] = threshold_y
        print(f"At {threshold}% of cachelines: {threshold_y:.2f}% of accesses for {criteria_name}")
    
    return {
        'total_pc_pairs': total_pc_pairs,
        'unique_pc_pairs': unique_pc_pairs,
        'total_cachelines': total_cachelines,
        'non_repeating_pairs': len(non_repeating),
        'cumulative_cachelines': cum_cachelines,
        'cumulative_accesses': cum_accesses,
        'threshold_values': threshold_values
    }

def plot_combined_results(results, max_distance):
    """
    Plot the cumulative distributions for all three criteria on one graph.
    """
    plt.figure(figsize=(12, 8))
    
    # Define temperature thresholds
    super_hot_threshold = 5
    hot_threshold = 15
    warm_threshold = 50
    
    # Add color zones for different temperatures
    plt.axvspan(0, super_hot_threshold, color='#F34128', alpha=0.2, label='Super Hot')
    plt.axvspan(super_hot_threshold, hot_threshold, color='#F36C28', alpha=0.2, label='Hot')
    plt.axvspan(hot_threshold, warm_threshold, color='#F9EE4A', alpha=0.2, label='Warm')
    plt.axvspan(warm_threshold, 100, color='#9EF1F5', alpha=0.2, label='Cold')
    
    # Add threshold lines
    plt.axvline(x=super_hot_threshold, color='black', linestyle='--', alpha=0.5)
    plt.axvline(x=hot_threshold, color='black', linestyle='--', alpha=0.5)
    plt.axvline(x=warm_threshold, color='black', linestyle='--', alpha=0.5)
    
    # Define colors for each criteria
    colors = {
        "Same Cacheblock": "#4169E1",                     # Blue
        "Same Cacheblock + Base Reg": "#008000",          # Green
        "Same Cacheblock + Base Reg + Mem Size": "#FF4500" # Orange-Red
    }
    
    # Define line styles for each criteria
    line_styles = {
        "Same Cacheblock": "-",
        "Same Cacheblock + Base Reg": "--",
        "Same Cacheblock + Base Reg + Mem Size": "-."
    }
    
    # Define markers for each criteria
    markers = {
        "Same Cacheblock": "o",
        "Same Cacheblock + Base Reg": "s",
        "Same Cacheblock + Base Reg + Mem Size": "^"
    }
    
    # Plot each criteria
    for criteria, data in results.items():
        if data:  # Check if results exist for this criteria
            plt.plot(
                data['cumulative_cachelines'],
                data['cumulative_accesses'],
                linestyle=line_styles[criteria],
                color=colors[criteria],
                marker=markers[criteria],
                markersize=5,
                linewidth=2,
                label=f"{criteria} ({data['total_pc_pairs']} pairs)",
                markevery=max(1, len(data['cumulative_cachelines']) // 15)  # Place markers selectively
            )
            
            # Add markers at threshold points
            for threshold in [super_hot_threshold, hot_threshold, warm_threshold]:
                threshold_y = data['threshold_values'][threshold]
                plt.plot(threshold, threshold_y, 'ko', markersize=7)
    
    # Set labels and title
    plt.xlabel('Unique cachelines (%)', fontsize=14)
    plt.ylabel('Cumulative PC pairs (%)', fontsize=14)
    plt.title(f'Distribution of PC Pairs Across Cachelines (Distance ≤ {max_distance})', 
              fontsize=16, pad=20)
    
    # Set limits
    plt.xlim(0, 100)
    plt.ylim(0, 100)
    
    # Add grid
    plt.grid(True, alpha=0.3)
    
    # Add legend
    plt.legend(loc='lower right', fontsize=12)
    
    # Save the figure
    output_file = f"cacheline_distribution_{max_distance}_comparison.png"
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"Comparison plot saved to {output_file}")
    plt.close()

def main():
    """
    Main function to run the analysis with multiple filtering criteria.
    """
    print("=== BFS Cacheline Distribution Analysis (Multiple Criteria) ===")
    print(f"Input file: {BFS_OUTPUT_FILE}")
    
    # Check if input file exists
    if not os.path.exists(BFS_OUTPUT_FILE):
        print(f"ERROR: Input file not found at {BFS_OUTPUT_FILE}")
        return
    
    # Test file reading
    try:
        with open(BFS_OUTPUT_FILE, 'r') as f:
            first_line = next(f, None)
        print(f"Successfully opened input file. First line: {first_line[:50]}...")
    except Exception as e:
        print(f"ERROR reading file: {e}")
        return
    
    # Run analysis for distance <= 352 with all three criteria
    results = analyze_cacheline_distribution(BFS_OUTPUT_FILE, max_distance=352)
    
    if results:
        print("\n=== Analysis Summary ===")
        for criteria, data in results.items():
            if data:
                print(f"\n{criteria}:")
                print(f"  Total PC pairs: {data['total_pc_pairs']}")
                print(f"  Unique PC pairs: {data['unique_pc_pairs']}")
                print(f"  Total cachelines: {data['total_cachelines']}")
                print(f"  Non-repeating pairs: {data['non_repeating_pairs']}")
    else:
        print("Analysis did not produce results.")
    
    print("\nAnalysis complete!")

if __name__ == "__main__":
    main()