import re
import os
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
from collections import Counter, defaultdict

# Input file path
BFS_OUTPUT_FILE = "/users/deepmish/scarab/src/gap/bfs/bfs.txt"

def analyze_cacheline_distribution(input_file, max_distance=None):
    """
    Analyze the distribution of load micro-op PC pairs across cachelines,
    including statistics on hot, warm, and cold cachelines.
    
    Parameters:
    input_file (str): Path to the input file to analyze
    max_distance (int, optional): Maximum distance between micro-ops to consider. 
                                 If None, no distance constraint is applied.
    """
    distance_desc = "no limit" if max_distance is None else f"<= {max_distance}"
    print(f"Processing file: {input_file}")
    print(f"Filtering for pairs with distance {distance_desc}")
    
    # Check if file exists
    if not os.path.exists(input_file):
        print(f"Error: File not found at {input_file}")
        return None
    
    with open(input_file, 'r') as f:
        lines = f.readlines()
    
    # Data structures to store analysis results
    cacheline_to_pc_pairs = defaultdict(list)  # Cacheline -> List of PC pairs accessing it
    pc_pairs_count = Counter()                 # PC pair -> Count of occurrences
    
    # Parse the file to extract cacheline and PC pair information
    for line in lines:
        if "Op1 PC:" in line and "Op2 PC:" in line and "Cacheblock:" in line:
            try:
                # Extract PC addresses
                pc1_match = re.search(r'Op1 PC: (\w+)', line)
                pc2_match = re.search(r'Op2 PC: (\w+)', line)
                
                # Extract cacheline addresses
                cacheblock_match = re.search(r'Op1 Cacheblock: (\w+)', line)
                
                # Extract micro-op IDs to check distance
                op1_id_match = re.search(r'Op1 micro-op id: (\d+)', line)
                op2_id_match = re.search(r'Op2 micro-op id: (\d+)', line)
                
                # Extract base registers and memory sizes for additional filtering
                op1_base_reg_match = re.search(r'Op1 base reg: (\d+)', line)
                op2_base_reg_match = re.search(r'Op2 base reg: (\d+)', line)
                op1_mem_size_match = re.search(r'Op1 mem size: (\d+)', line)
                op2_mem_size_match = re.search(r'Op2 mem size: (\d+)', line)
                
                if all([pc1_match, pc2_match, cacheblock_match, op1_id_match, op2_id_match,
                        op1_base_reg_match, op2_base_reg_match, op1_mem_size_match, op2_mem_size_match]):
                    
                    pc1 = pc1_match.group(1)
                    pc2 = pc2_match.group(1)
                    cacheline = cacheblock_match.group(1)
                    op1_id = int(op1_id_match.group(1))
                    op2_id = int(op2_id_match.group(1))
                    op1_base_reg = int(op1_base_reg_match.group(1))
                    op2_base_reg = int(op2_base_reg_match.group(1))
                    op1_mem_size = int(op1_mem_size_match.group(1))
                    op2_mem_size = int(op2_mem_size_match.group(1))
                    
                    # Calculate distance
                    distance = abs(op2_id - op1_id)
                    
                    # Check if the constraints are satisfied:
                    # 1. Distance is within the limit (if specified)
                    # 2. Same base register
                    # 3. Same memory size
                    if ((max_distance is None or distance <= max_distance) and 
                        op1_base_reg == op2_base_reg and 
                        op1_mem_size == op2_mem_size):
                        
                        # Create PC pair and add to data structures
                        pc_pair = (pc1, pc2)
                        cacheline_to_pc_pairs[cacheline].append(pc_pair)
                        pc_pairs_count[pc_pair] += 1
            except Exception as e:
                print(f"Error parsing line: {e}")
                print(f"Line content: {line[:100]}...")
    
    # Calculate total number of PC pairs and unique PC pairs
    total_pc_pairs = sum(pc_pairs_count.values())
    unique_pc_pairs = len(pc_pairs_count)
    
    if total_pc_pairs == 0:
        print("No pairs found matching the criteria")
        return None
    
    print(f"Found {total_pc_pairs} total pairs and {unique_pc_pairs} unique PC pairs")
    
    # Calculate statistics for cachelines
    cacheline_counts = {cacheline: len(pc_pairs) for cacheline, pc_pairs in cacheline_to_pc_pairs.items()}
    total_cachelines = len(cacheline_counts)
    
    # Sort cachelines by access count (most accessed to least)
    sorted_cachelines = sorted(cacheline_counts.items(), key=lambda x: x[1], reverse=True)
    
    # Calculate cumulative distribution
    cumulative_accesses = []
    cumulative_cachelines = []
    cumulative_access_count = 0
    non_repeating_pc_pairs = []
    
    # Identify non-repeating (unique) PC pairs
    non_repeating = {pair: count for pair, count in pc_pairs_count.items() if count == 1}
    
    # Calculate statistics for the graph
    cumulative_data = []
    for i, (cacheline, access_count) in enumerate(sorted_cachelines):
        cumulative_access_count += access_count
        cumulative_percentage = (cumulative_access_count / total_pc_pairs) * 100
        cacheline_percentage = ((i + 1) / total_cachelines) * 100
        
        # Count non-repeating PC pairs in this cacheline
        non_repeating_in_cacheline = sum(1 for pair in cacheline_to_pc_pairs[cacheline] if pair in non_repeating)
        non_repeating_pc_pairs.append(non_repeating_in_cacheline)
        
        cumulative_accesses.append(cumulative_percentage)
        cumulative_cachelines.append(cacheline_percentage)
        
        cumulative_data.append({
            'cacheline': cacheline,
            'access_count': access_count,
            'cacheline_percentage': cacheline_percentage,
            'cumulative_access_percentage': cumulative_percentage,
            'non_repeating_count': non_repeating_in_cacheline,
            'non_repeating_percentage': (sum(non_repeating_pc_pairs) / len(non_repeating)) * 100 if non_repeating else 0
        })
    
    # Define temperature thresholds (based on your graph)
    super_hot_threshold = 5   # First 5% of cachelines
    hot_threshold = 15        # Next 10% of cachelines (up to 15%)
    warm_threshold = 50       # Next 35% of cachelines (up to 50%)
    # Above 50% is considered cold
    
    # Calculate temperature-based statistics
    super_hot_index = int(total_cachelines * (super_hot_threshold / 100))
    hot_index = int(total_cachelines * (hot_threshold / 100))
    warm_index = int(total_cachelines * (warm_threshold / 100))
    
    super_hot_accesses = sorted_cachelines[:super_hot_index] if super_hot_index > 0 else []
    hot_accesses = sorted_cachelines[super_hot_index:hot_index] if hot_index > super_hot_index else []
    warm_accesses = sorted_cachelines[hot_index:warm_index] if warm_index > hot_index else []
    cold_accesses = sorted_cachelines[warm_index:] if warm_index < len(sorted_cachelines) else []
    
    super_hot_count = sum(count for _, count in super_hot_accesses)
    hot_count = sum(count for _, count in hot_accesses)
    warm_count = sum(count for _, count in warm_accesses)
    cold_count = sum(count for _, count in cold_accesses)
    
    # Calculate non-repeating PC pairs by temperature
    non_repeating_super_hot = sum(non_repeating_pc_pairs[:super_hot_index]) if super_hot_index > 0 else 0
    non_repeating_hot = sum(non_repeating_pc_pairs[super_hot_index:hot_index]) if hot_index > super_hot_index else 0
    non_repeating_warm = sum(non_repeating_pc_pairs[hot_index:warm_index]) if warm_index > hot_index else 0
    non_repeating_cold = sum(non_repeating_pc_pairs[warm_index:]) if warm_index < len(non_repeating_pc_pairs) else 0
    
    # Calculate percentages of accesses and non-repeating pairs
    super_hot_access_pct = (super_hot_count / total_pc_pairs) * 100
    hot_access_pct = (hot_count / total_pc_pairs) * 100
    warm_access_pct = (warm_count / total_pc_pairs) * 100
    cold_access_pct = (cold_count / total_pc_pairs) * 100
    
    non_repeating_total = len(non_repeating)
    non_repeating_super_hot_pct = (non_repeating_super_hot / non_repeating_total) * 100 if non_repeating_total else 0
    non_repeating_hot_pct = (non_repeating_hot / non_repeating_total) * 100 if non_repeating_total else 0
    non_repeating_warm_pct = (non_repeating_warm / non_repeating_total) * 100 if non_repeating_total else 0
    non_repeating_cold_pct = (non_repeating_cold / non_repeating_total) * 100 if non_repeating_total else 0
    
    # Prepare results dictionary
    results = {
        'max_distance': max_distance,
        'distance_desc': distance_desc,
        'total_pc_pairs': total_pc_pairs,
        'unique_pc_pairs': unique_pc_pairs,
        'total_cachelines': total_cachelines,
        'non_repeating_pairs': len(non_repeating),
        'super_hot_threshold': super_hot_threshold,
        'hot_threshold': hot_threshold,
        'warm_threshold': warm_threshold,
        'super_hot_cachelines': len(super_hot_accesses),
        'hot_cachelines': len(hot_accesses),
        'warm_cachelines': len(warm_accesses),
        'cold_cachelines': len(cold_accesses),
        'super_hot_access_count': super_hot_count,
        'hot_access_count': hot_count,
        'warm_access_count': warm_count,
        'cold_access_count': cold_count,
        'super_hot_access_pct': super_hot_access_pct,
        'hot_access_pct': hot_access_pct,
        'warm_access_pct': warm_access_pct,
        'cold_access_pct': cold_access_pct,
        'non_repeating_super_hot': non_repeating_super_hot,
        'non_repeating_hot': non_repeating_hot,
        'non_repeating_warm': non_repeating_warm,
        'non_repeating_cold': non_repeating_cold,
        'non_repeating_super_hot_pct': non_repeating_super_hot_pct,
        'non_repeating_hot_pct': non_repeating_hot_pct,
        'non_repeating_warm_pct': non_repeating_warm_pct,
        'non_repeating_cold_pct': non_repeating_cold_pct,
        'cumulative_data': cumulative_data,
        'cumulative_cachelines': cumulative_cachelines,
        'cumulative_accesses': cumulative_accesses
    }
    
    # Return the analysis results
    return results

def plot_multiple_distances(input_file, distances=[None, 10, 140, 352], output_file="cacheline_multiple_distances.png"):
    """
    Create a graph with multiple lines, each representing a different distance constraint.
    
    Parameters:
    input_file (str): Path to the input file
    distances (list): List of distance constraints to analyze
    output_file (str): Path to save the output plot
    """
    # Set up the plot with serif font like in the reference
    plt.figure(figsize=(12, 8))
    plt.rcParams.update({
        'font.family': 'serif',
        'font.size': 12,
        'font.weight': 'bold',
        'axes.labelweight': 'bold',
        'axes.titleweight': 'bold',
        'axes.labelsize': 16,
        'axes.titlesize': 20,
        'xtick.labelsize': 14,
        'ytick.labelsize': 14,
    })
    
    # Define temperature zone colors
    zone_colors = {
        'super_hot': '#F34128',  # Light red
        'hot': '#F36C28',        # Light orange/red
        'warm': '#F9EE4A',       # Light yellow
        'cold': '#9EF1F5'        # Light blue
    }
    
    # Define line colors and styles for different distance constraints
    distance_colors = ['#4169E1', '#FF4500', '#008000', '#800080']  # Royal blue, OrangeRed, Green, Purple
    distance_markers = ['o', 's', '^', 'D']  # Circle, Square, Triangle, Diamond
    
    # Add temperature zones with adjusted alpha to match reference (only once for the background)
    super_hot_threshold = 5
    hot_threshold = 15
    warm_threshold = 50
    
    # Super hot zone (0-5%)
    plt.axvspan(0, super_hot_threshold, color=zone_colors['super_hot'], alpha=1.0, edgecolor='black', linewidth=0.5)
    
    # Hot zone (5-15%)
    plt.axvspan(super_hot_threshold, hot_threshold, color=zone_colors['hot'], alpha=1.0, edgecolor='black', linewidth=0.5)
    
    # Warm zone (15-50%)
    plt.axvspan(hot_threshold, warm_threshold, color=zone_colors['warm'], alpha=1.0, edgecolor='black', linewidth=0.5)
    
    # Cold zone (50-100%)
    plt.axvspan(warm_threshold, 100, color=zone_colors['cold'], alpha=1.0, edgecolor='black', linewidth=0.5)
    
    # Add threshold lines with dashed style
    plt.axvline(x=super_hot_threshold, color='black', linestyle='--', alpha=0.7, linewidth=1.5)
    plt.axvline(x=hot_threshold, color='black', linestyle='--', alpha=0.7, linewidth=1.5)
    plt.axvline(x=warm_threshold, color='black', linestyle='--', alpha=0.7, linewidth=1.5)
    
    # Add temperature zone labels
    plt.text(2.5, 35, 'super hot', color='#990000', fontsize=16, fontweight='bold', ha='center', va='center')
    plt.text(10, 25, 'hot', color='#000000', fontsize=16, fontweight='bold', ha='center', va='center')
    plt.text(32.5, 35, 'warm', color='#8B8000', fontsize=16, fontweight='bold', ha='center', va='center')
    plt.text(75, 80, 'cold', color='#000080', fontsize=16, fontweight='bold', ha='center', va='center')
    
    # Analyze and plot for each distance constraint
    for i, distance in enumerate(distances):
        print(f"\nAnalyzing with distance constraint: {distance if distance is not None else 'No limit'}")
        results = analyze_cacheline_distribution(input_file, max_distance=distance)
        
        if results:
            # Ensure the CDF starts at 0
            # Prepend 0,0 to the cumulative data
            cum_cachelines = [0] + list(results['cumulative_cachelines'])
            cum_accesses = [0] + list(results['cumulative_accesses'])
            
            # Plot the cumulative distribution for this distance constraint
            label = f"Distance: {results['distance_desc']}"
            plt.plot(
                cum_cachelines,
                cum_accesses,
                f"-{distance_markers[i]}",
                color=distance_colors[i],
                linewidth=2.0,
                markersize=6,
                label=label,
                markevery=max(1, len(cum_cachelines) // 20)  # Plot fewer markers for clarity
            )
            
            # Add markers for important points
            for threshold in [super_hot_threshold, hot_threshold, warm_threshold]:
                threshold_y = np.interp(threshold, cum_cachelines, cum_accesses)
                plt.plot(threshold, threshold_y, 'ko', markersize=8, markeredgewidth=1.5)
            
            # Print summary statistics
            print_summary_statistics(results)
        else:
            print(f"Analysis failed for distance constraint: {distance if distance is not None else 'No limit'}")
    
    # Set axis labels and title
    plt.xlabel('Unique cachelines (%)', fontsize=14, fontweight='bold')
    plt.ylabel('Non-repeating load micro-op PC pairs (%)', fontsize=14, fontweight='bold')
    plt.title('Distribution of Load Micro-op PC Pairs Across Cachelines in BFS\n'
              'Under Different Distance Constraints', 
              fontsize=16, fontweight='bold', pad=20)
    
    # Add legend in bottom right
    legend = plt.legend(title='Distance Constraints', loc='lower right', frameon=True, framealpha=1.0, 
                        fontsize=12, title_fontsize=12, edgecolor='black')
    legend.get_frame().set_facecolor('white')
    
    # Set axis limits
    plt.xlim(0, 100)
    plt.ylim(0, 100)
    
    # Add grid with light gray lines
    plt.grid(True, alpha=0.3, linestyle='-', color='#E0E0E0')
    
    # Add a thin black border around the entire plot
    for spine in plt.gca().spines.values():
        spine.set_visible(True)
        spine.set_color('black')
        spine.set_linewidth(1.0)
    
    # Save the figure with tight layout and high resolution
    plt.tight_layout()
    plt.savefig(output_file, dpi=300, bbox_inches='tight', facecolor='white', edgecolor='none')
    print(f"\nPlot with multiple distance constraints saved to {output_file}")
    plt.close()

def print_summary_statistics(results):
    """Print summary statistics for a single distance constraint analysis"""
    print(f"\nDistance constraint: {results['distance_desc']}")
    print(f"Total load micro-op PC pairs: {results['total_pc_pairs']}")
    print(f"Unique load micro-op PC pairs: {results['unique_pc_pairs']}")
    print(f"Non-repeating PC pairs: {results['non_repeating_pairs']} ({results['non_repeating_pairs']/results['unique_pc_pairs']*100:.2f}% of unique pairs)")
    print(f"Total unique cachelines: {results['total_cachelines']}")
    
    # Extract values at key thresholds
    super_hot_y = np.interp(results['super_hot_threshold'], [0] + list(results['cumulative_cachelines']), [0] + list(results['cumulative_accesses']))
    hot_y = np.interp(results['hot_threshold'], [0] + list(results['cumulative_cachelines']), [0] + list(results['cumulative_accesses']))
    warm_y = np.interp(results['warm_threshold'], [0] + list(results['cumulative_cachelines']), [0] + list(results['cumulative_accesses']))
    
    print(f"At {results['super_hot_threshold']}% of cachelines: {super_hot_y:.2f}% of accesses")
    print(f"At {results['hot_threshold']}% of cachelines: {hot_y:.2f}% of accesses")
    print(f"At {results['warm_threshold']}% of cachelines: {warm_y:.2f}% of accesses")

def print_detailed_statistics(results):
    """Print detailed statistics about the cacheline distribution"""
    print("\n" + "="*80)
    print(f"CACHELINE DISTRIBUTION STATISTICS (Distance: {results['distance_desc']})")
    print("="*80)
    
    print(f"\nTotal load micro-op PC pairs: {results['total_pc_pairs']}")
    print(f"Unique load micro-op PC pairs: {results['unique_pc_pairs']}")
    print(f"Non-repeating PC pairs: {results['non_repeating_pairs']} ({results['non_repeating_pairs']/results['unique_pc_pairs']*100:.2f}% of unique pairs)")
    print(f"Total unique cachelines: {results['total_cachelines']}")
    
    print("\nCACHELINE TEMPERATURE STATISTICS:")
    print("-"*50)
    print(f"Super Hot Cachelines (0-{results['super_hot_threshold']}%):")
    print(f"  - Count: {results['super_hot_cachelines']} cachelines ({results['super_hot_cachelines']/results['total_cachelines']*100:.2f}% of all cachelines)")
    print(f"  - Accesses: {results['super_hot_access_count']} ({results['super_hot_access_pct']:.2f}% of all accesses)")
    print(f"  - Non-repeating pairs: {results['non_repeating_super_hot']} ({results['non_repeating_super_hot_pct']:.2f}% of all non-repeating pairs)")
    
    print(f"\nHot Cachelines ({results['super_hot_threshold']}-{results['hot_threshold']}%):")
    print(f"  - Count: {results['hot_cachelines']} cachelines ({results['hot_cachelines']/results['total_cachelines']*100:.2f}% of all cachelines)")
    print(f"  - Accesses: {results['hot_access_count']} ({results['hot_access_pct']:.2f}% of all accesses)")
    print(f"  - Non-repeating pairs: {results['non_repeating_hot']} ({results['non_repeating_hot_pct']:.2f}% of all non-repeating pairs)")
    
    print(f"\nWarm Cachelines ({results['hot_threshold']}-{results['warm_threshold']}%):")
    print(f"  - Count: {results['warm_cachelines']} cachelines ({results['warm_cachelines']/results['total_cachelines']*100:.2f}% of all cachelines)")
    print(f"  - Accesses: {results['warm_access_count']} ({results['warm_access_pct']:.2f}% of all accesses)")
    print(f"  - Non-repeating pairs: {results['non_repeating_warm']} ({results['non_repeating_warm_pct']:.2f}% of all non-repeating pairs)")
    
    print(f"\nCold Cachelines ({results['warm_threshold']}-100%):")
    print(f"  - Count: {results['cold_cachelines']} cachelines ({results['cold_cachelines']/results['total_cachelines']*100:.2f}% of all cachelines)")
    print(f"  - Accesses: {results['cold_access_count']} ({results['cold_access_pct']:.2f}% of all accesses)")
    print(f"  - Non-repeating pairs: {results['non_repeating_cold']} ({results['non_repeating_cold_pct']:.2f}% of all non-repeating pairs)")
    
    print("\nKEY POINTS ON CUMULATIVE DISTRIBUTION:")
    print("-"*50)
    
    # Extract values at key thresholds
    super_hot_y = np.interp(results['super_hot_threshold'], [0] + list(results['cumulative_cachelines']), [0] + list(results['cumulative_accesses']))
    hot_y = np.interp(results['hot_threshold'], [0] + list(results['cumulative_cachelines']), [0] + list(results['cumulative_accesses']))
    warm_y = np.interp(results['warm_threshold'], [0] + list(results['cumulative_cachelines']), [0] + list(results['cumulative_accesses']))
    
    print(f"At {results['super_hot_threshold']}% of cachelines: {super_hot_y:.2f}% of accesses")
    print(f"At {results['hot_threshold']}% of cachelines: {hot_y:.2f}% of accesses")
    print(f"At {results['warm_threshold']}% of cachelines: {warm_y:.2f}% of accesses")
    
    print("\nTOP 10 HOTTEST CACHELINES:")
    print("-"*50)
    
    # Get the top 10 cachelines by access count
    for i, entry in enumerate(results['cumulative_data'][:10]):
        print(f"{i+1}. Cacheline {entry['cacheline']}: {entry['access_count']} accesses "
              f"({entry['access_count']/results['total_pc_pairs']*100:.2f}% of total), "
              f"{entry['non_repeating_count']} non-repeating pairs")
    
    print("\n" + "="*80)

def analyze_distance_ranges(input_file):
    """
    Analyze the distribution of load micro-op PC pairs across different distance ranges
    as shown in the reference graph.
    
    Parameters:
    input_file (str): Path to the input file to analyze
    """
    print(f"Analyzing distance ranges for file: {input_file}")
    
    # Define the distance ranges from the graph
    distance_ranges = [
        (1, 10, "Very Short (1-10)"),        # Very Short Distance
        (11, 20, "Very Short (11-20)"),      # Very Short Distance
        (21, 30, "Short (21-30)"),           # Short Distance
        (31, 40, "Short (31-40)"),           # Short Distance
        (41, 50, "Short (41-50)"),           # Short Distance
        (51, 60, "Medium (51-60)"),          # Medium Distance
        (61, 70, "Medium (61-70)"),          # Medium Distance
        (71, 80, "Medium (71-80)"),          # Medium Distance
        (81, 90, "Medium (81-90)"),          # Medium Distance
        (91, 100, "Medium (91-100)"),        # Medium Distance
        (101, 999, "Long (101-999)"),        # Long Distance
        (1000, 9999, "Long (1000-9999)"),    # Long Distance
        (10000, float('inf'), "Long (>10K)") # Long Distance (anything over 10K)
    ]
    
    # Check if file exists
    if not os.path.exists(input_file):
        print(f"Error: File not found at {input_file}")
        return None
    
    with open(input_file, 'r') as f:
        lines = f.readlines()
    
    # Initialize counters for different filtering criteria
    range_counts = {
        "Same Cacheblock": defaultdict(int),
        "Same Mem Size": defaultdict(int),
        "Same Mem Size + Base Reg": defaultdict(int)
    }
    
    total_pairs = 0
    
    # Parse the file to extract information
    for line in lines:
        if "Op1 PC:" in line and "Op2 PC:" in line and "Cacheblock:" in line:
            try:
                # Extract PC addresses
                pc1_match = re.search(r'Op1 PC: (\w+)', line)
                pc2_match = re.search(r'Op2 PC: (\w+)', line)
                
                # Extract cacheline addresses
                cacheblock1_match = re.search(r'Op1 Cacheblock: (\w+)', line)
                cacheblock2_match = re.search(r'Op2 Cacheblock: (\w+)', line)
                
                # Extract micro-op IDs to check distance
                op1_id_match = re.search(r'Op1 micro-op id: (\d+)', line)
                op2_id_match = re.search(r'Op2 micro-op id: (\d+)', line)
                
                # Extract base registers and memory sizes for additional filtering
                op1_base_reg_match = re.search(r'Op1 base reg: (\d+)', line)
                op2_base_reg_match = re.search(r'Op2 base reg: (\d+)', line)
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
                    
                    # Calculate distance between micro-ops
                    distance = abs(op2_id - op1_id)
                    
                    # Determine which range this distance falls into
                    for min_dist, max_dist, range_label in distance_ranges:
                        if min_dist <= distance <= max_dist:
                            # Check different criteria
                            # Same Cacheblock
                            if cacheblock1 == cacheblock2:
                                range_counts["Same Cacheblock"][range_label] += 1
                                
                                # Same Mem Size (and Same Cacheblock implied)
                                if op1_mem_size == op2_mem_size:
                                    range_counts["Same Mem Size"][range_label] += 1
                                    
                                    # Same Mem Size + Base Reg (and Same Cacheblock implied)
                                    if op1_base_reg == op2_base_reg:
                                        range_counts["Same Mem Size + Base Reg"][range_label] += 1
                                        total_pairs += 1
                            
                            break
            except Exception as e:
                print(f"Error parsing line: {e}")
                print(f"Line content: {line[:100]}...")
    
    # Calculate percentages
    range_percentages = {
        criteria: {
            range_label: (count / total_pairs * 100) if total_pairs > 0 else 0
            for range_label, count in range_data.items()
        } 
        for criteria, range_data in range_counts.items()
    }
    
    # Plot the results as a bar chart similar to the reference
    plot_distance_ranges(range_percentages, distance_ranges, f"distance_ranges_distribution.png")
    
    return {
        "range_counts": range_counts,
        "range_percentages": range_percentages,
        "total_pairs": total_pairs,
        "distance_ranges": distance_ranges
    }

def plot_distance_ranges(range_percentages, distance_ranges, output_file):
    """
    Create a bar chart showing the distribution of load micro-op PC pairs
    across different distance ranges with multiple criteria.
    """
    plt.figure(figsize=(16, 10))
    plt.rcParams.update({
        'font.family': 'sans-serif',
        'font.size': 12,
        'font.weight': 'bold',
        'axes.labelweight': 'bold',
        'axes.titleweight': 'bold',
        'axes.labelsize': 14,
        'axes.titlesize': 16,
        'xtick.labelsize': 12,
        'ytick.labelsize': 12,
    })
    
    # Extract labels for x-axis
    range_labels = [label for _, _, label in distance_ranges]
    
    # Create x positions for bars
    x = np.arange(len(range_labels))
    width = 0.25  # width of the bars
    
    # Colors for different criteria (matching the reference)
    colors = {
        "Same Cacheblock": 'black',
        "Same Mem Size": '#8B4513',  # Dark brown
        "Same Mem Size + Base Reg": '#FFD700'  # Gold
    }
    
    # Define background colors for different distance categories
    bg_colors = {
        "Very Short": '#FFFACD',  # Light yellow
        "Short": '#E0FFEA',       # Light green
        "Medium": '#E6E6E6',      # Light gray
        "Long": '#FFE4E1'         # Light red/pink
    }
    
    # Add background colors for different distance categories
    current_category = None
    for i, (_, _, label) in enumerate(distance_ranges):
        category = label.split(' ')[0]  # Extract "Very Short", "Short", etc.
        
        if current_category != category:
            plt.axvspan(i - 0.5, i + 0.5, color=bg_colors[category], alpha=1.0)
            
            # Add category label in the middle of the span
            if current_category is not None:
                plt.text(mid_point, 80, f"{current_category}\nDistance", 
                         ha='center', va='center', fontsize=14, fontweight='bold')
            
            current_category = category
            start_idx = i
        
        # For the last category
        if i == len(distance_ranges) - 1:
            mid_point = (start_idx + i) / 2
            plt.text(mid_point, 80, f"{current_category}\nDistance", 
                     ha='center', va='center', fontsize=14, fontweight='bold')
    
    # Plot bars for each criteria
    for i, (criteria, color) in enumerate(colors.items()):
        values = [range_percentages[criteria].get(label, 0) for label in range_labels]
        plt.bar(x + (i - 1) * width, values, width, color=color, label=criteria, edgecolor='black', linewidth=0.5)
        
        # Add value labels on top of bars
        for j, value in enumerate(values):
            if value >= 1.0:  # Only show labels for values >= 1%
                plt.text(x[j] + (i - 1) * width, value + 0.5, f"{value:.1f}", 
                         ha='center', va='bottom', fontsize=9)
    
    # Customize the plot
    plt.xlabel('Distance Between Load Micro-Op PC Pairs Accessing Same Cacheblock', fontsize=14, fontweight='bold')
    plt.ylabel('% of Load Micro-Op PC Pairs', fontsize=14, fontweight='bold')
    plt.title('Distribution of Load Micro-Op PC Pairs by Distance Range', fontsize=16, fontweight='bold', pad=20)
    
    # Set x-tick labels to show simple distance ranges
    simple_labels = ["1-10", "11-20", "21-30", "31-40", "41-50", "51-60", "61-70", 
                      "71-80", "81-90", "91-100", "101-999", "1000-9999", ">10K"]
    plt.xticks(x, simple_labels, rotation=0)
    
    # Set y-axis limit
    plt.ylim(0, 100)
    
    # Add y-axis grid lines
    plt.grid(True, axis='y', alpha=0.3, linestyle='--')
    
    # Add legend
    plt.legend(loc='upper right', frameon=True)
    
    # Tight layout and save
    plt.tight_layout()
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"Distance range distribution plot saved to {output_file}")
    plt.close()

def main():
    """Main function to run the cacheline distribution analysis with multiple distance constraints"""
    # Define the distance constraints to analyze for cumulative distribution
    distances = [None, 10, 140, 352]
    
    # Plot multiple distance constraints on a single graph
    plot_multiple_distances(BFS_OUTPUT_FILE, distances=distances)
    
    # Analyze distance ranges as shown in the reference graph
    analyze_distance_ranges(BFS_OUTPUT_FILE)
    
    # For more detailed analysis of a specific distance constraint (optional)
    for distance in distances:
        results = analyze_cacheline_distribution(BFS_OUTPUT_FILE, max_distance=distance)
        if results:
            # Generate individual plots for each distance constraint (optional)
            distance_str = "no_limit" if distance is None else str(distance)
            plot_file = f"cacheline_distance_{distance_str}.png"
            
            # Print detailed statistics (optional)
            print_detailed_statistics(results)

if __name__ == "__main__":
    main()