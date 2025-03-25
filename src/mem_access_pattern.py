import os
import re
import matplotlib.pyplot as plt
import numpy as np
from collections import Counter, defaultdict
import matplotlib
matplotlib.rcParams['font.family'] = 'serif'
matplotlib.rcParams['font.serif'] = ['Times New Roman']

def analyze_memory_access_patterns():
    """
    Analyze whether memory accesses by fusion candidates are contiguous or scattered.
    This examines spatial locality patterns in cacheline addresses.
    """
    directory = "/users/deepmish/scarab/src/unique_cacheblock_pairs_fusion_candidates"
    
    results = {}
    
    # Process each file in the directory
    for filename in os.listdir(directory):
        if filename.endswith(".txt"):
            workload = filename.split('.')[0]
            file_path = os.path.join(directory, filename)
            
            # Data structures to track spatial patterns
            pc_pair_to_cachelines = defaultdict(list)  # Maps PC pairs to ordered list of cachelines they access
            cacheline_addresses = []  # All cacheline addresses in order of appearance
            pc_pairs_ordered = []     # All PC pairs in order of appearance
            
            # Read and parse the file
            with open(file_path, 'r') as f:
                for line in f:
                    # Match lines containing micro-op pairs and cacheline addresses
                    match = re.search(r'Micro-op 1: ([0-9a-f]+)\s+Micro-op 2: ([0-9a-f]+)\s+Cacheblock Address: ([0-9a-f]+)', line)
                    if match:
                        pc1, pc2, cacheline_addr = match.groups()
                        pc_pair = (pc1, pc2)
                        
                        # Convert cacheline address to integer for easier distance calculation
                        addr_int = int(cacheline_addr, 16)
                        
                        # Add to our tracking structures
                        pc_pair_to_cachelines[pc_pair].append(addr_int)
                        cacheline_addresses.append(addr_int)
                        pc_pairs_ordered.append(pc_pair)
            
            # Analyze repeating vs non-repeating PC pairs
            pc_pair_access_counts = Counter(pc_pairs_ordered)
            repeating_pairs = sum(1 for pair, count in pc_pair_access_counts.items() if count > 1)
            non_repeating_pairs = sum(1 for pair, count in pc_pair_access_counts.items() if count == 1)
            
            # Additional analysis - how many repeating pairs access the same cacheline vs different cachelines
            same_cacheline_repeating = 0
            multi_cacheline_repeating = 0
            
            for pair, count in pc_pair_access_counts.items():
                if count > 1:  # This is a repeating pair
                    unique_cachelines = len(set(pc_pair_to_cachelines[pair]))
                    if unique_cachelines == 1:
                        same_cacheline_repeating += 1
                    else:
                        multi_cacheline_repeating += 1
            
            # For transition analysis, we need repeating pairs that access different cachelines
            transition_capable_pairs = {
                pair: addrs for pair, addrs in pc_pair_to_cachelines.items()
                if pc_pair_access_counts[pair] > 1 and len(set(addrs)) > 1
            }
            
            # Calculate spatial locality metrics for transition-capable pairs
            
            # 1. For each transition-capable pair, calculate distances between consecutive cacheline accesses
            pc_pair_distances = {}
            for pc_pair, addresses in transition_capable_pairs.items():
                # Calculate distances only when cacheline changes
                distances = []
                for i in range(1, len(addresses)):
                    if addresses[i] != addresses[i-1]:  # Cacheline changed
                        distances.append(addresses[i] - addresses[i-1])
                
                if distances:  # Only store if there are actual transitions
                    pc_pair_distances[pc_pair] = distances
            
            # 2. Calculate overall distance distribution
            all_distances = []
            for distances in pc_pair_distances.values():
                all_distances.extend(distances)
            
            distance_distribution = Counter(all_distances)
            
            # 3. Calculate spatial locality metrics
            
            # Convert distances to cacheline distances (assuming 64-byte cachelines)
            cacheline_size = 64  # bytes
            
            # Categorize distances
            contiguous_count = 0  # Consecutive cachelines (distance = cacheline_size)
            near_count = 0       # Within 16 cachelines (reasonable spatial locality)
            scattered_count = 0  # Beyond 16 cachelines (poor spatial locality)
            
            for dist, count in distance_distribution.items():
                # Convert to cacheline count (how many cachelines apart)
                cacheline_distance = abs(dist) // cacheline_size
                
                if cacheline_distance == 1:  # Exactly 1 cacheline away (consecutive)
                    contiguous_count += count
                elif cacheline_distance <= 16:  # Within 16 cachelines
                    near_count += count
                else:  # More than 16 cachelines away
                    scattered_count += count
            
            total_transitions = contiguous_count + near_count + scattered_count
            
            # 4. Calculate spatial locality score
            if total_transitions > 0:
                contiguous_pct = (contiguous_count / total_transitions) * 100
                near_pct = (near_count / total_transitions) * 100
                scattered_pct = (scattered_count / total_transitions) * 100
            else:
                contiguous_pct = near_pct = scattered_pct = 0
            
            # 5. Determine common stride patterns
            # A stride is the distance between consecutive memory accesses by the same PC pair
            stride_distribution = Counter(all_distances)
            common_strides = stride_distribution.most_common(10)
            
            # Store results for this workload
            results[workload] = {
                'unique_pc_pairs': len(pc_pair_to_cachelines),
                'unique_cachelines': len(set(cacheline_addresses)),
                'total_transitions': total_transitions,
                'contiguous_count': contiguous_count,
                'near_count': near_count,
                'scattered_count': scattered_count,
                'contiguous_pct': contiguous_pct,
                'near_pct': near_pct,
                'scattered_pct': scattered_pct,
                'stride_distribution': stride_distribution,
                'common_strides': common_strides,
                'pc_pair_to_cachelines': pc_pair_to_cachelines,
                'transition_capable_pairs': transition_capable_pairs,
                'repeating_pairs': repeating_pairs,
                'non_repeating_pairs': non_repeating_pairs,
                'same_cacheline_repeating': same_cacheline_repeating,
                'multi_cacheline_repeating': multi_cacheline_repeating,
                'repetition_distribution': Counter(pc_pair_access_counts.values()),
                'total_pc_pair_occurrences': len(pc_pairs_ordered)
            }
    
    return results

def generate_spatial_locality_report(results):
    """Generate a detailed report on spatial locality patterns"""
    
    print("\n===== Memory Access Pattern Analysis =====\n")
    
    print(f"{'Workload':<10} | {'Contiguous %':<15} | {'Near %':<15} | {'Scattered %':<15} | {'Repeating Pairs':<15} | {'Transition Capable':<15}")
    print("-" * 100)
    
    for workload, data in sorted(results.items()):
        contiguous_pct = data['contiguous_pct']
        near_pct = data['near_pct']
        scattered_pct = data['scattered_pct']
        repeating_pairs = data['repeating_pairs']
        transition_capable = len(data['transition_capable_pairs'])
        
        print(f"{workload:<10} | {contiguous_pct:<15.2f} | {near_pct:<15.2f} | {scattered_pct:<15.2f} | {repeating_pairs:<15,d} | {transition_capable:<15,d}")
    
    print("\n===== Detailed Access Pattern Analysis by Workload =====\n")
    
    for workload, data in sorted(results.items()):
        print(f"\n{workload.upper()}:")
        print(f"  Total unique PC pairs: {data['unique_pc_pairs']:,}")
        
        # PC pair repetition details
        total_pairs = data['repeating_pairs'] + data['non_repeating_pairs']
        repeating_pct = (data['repeating_pairs'] / total_pairs * 100) if total_pairs > 0 else 0
        non_repeating_pct = (data['non_repeating_pairs'] / total_pairs * 100) if total_pairs > 0 else 0
        
        print(f"  Repeating PC pairs: {data['repeating_pairs']:,} ({repeating_pct:.2f}%)")
        print(f"  Non-repeating PC pairs: {data['non_repeating_pairs']:,} ({non_repeating_pct:.2f}%)")
        
        # Breakdown of repeating pairs
        same_cl = data['same_cacheline_repeating']
        multi_cl = data['multi_cacheline_repeating']
        same_cl_pct = (same_cl / data['repeating_pairs'] * 100) if data['repeating_pairs'] > 0 else 0
        multi_cl_pct = (multi_cl / data['repeating_pairs'] * 100) if data['repeating_pairs'] > 0 else 0
        
        print(f"\n  Repeating PC pair breakdown:")
        print(f"    Access same cacheline: {same_cl:,} ({same_cl_pct:.2f}% of repeating)")
        print(f"    Access multiple cachelines: {multi_cl:,} ({multi_cl_pct:.2f}% of repeating)")
        
        print(f"\n  Total cacheline transitions analyzed: {data['total_transitions']:,}")
        print(f"  Transition-capable PC pairs: {len(data['transition_capable_pairs']):,}")
        
        print(f"\n  SPATIAL LOCALITY PATTERNS (TRANSITION-CAPABLE PAIRS ONLY):")
        print(f"  Contiguous accesses (1 cacheline apart): {data['contiguous_count']:,} ({data['contiguous_pct']:.2f}%)")
        print(f"  Near accesses (2-16 cachelines apart): {data['near_count']:,} ({data['near_pct']:.2f}%)")
        print(f"  Scattered accesses (>16 cachelines apart): {data['scattered_count']:,} ({data['scattered_pct']:.2f}%)")
        
        # Show distribution of repetition counts
        print(f"\n  Distribution of PC pair repetition counts:")
        repetition_dist = data['repetition_distribution']
        # Group counts higher than 10 together
        grouped_dist = {}
        for count, freq in sorted(repetition_dist.items()):
            if count <= 10:
                grouped_dist[count] = freq
            else:
                grouped_dist[11] = grouped_dist.get(11, 0) + freq  # Use 11 as a numeric key instead of '>10'
        
        # Custom sort to handle the grouped values properly
        for count, freq in sorted(grouped_dist.items()):
            percentage = (freq / total_pairs * 100) if total_pairs > 0 else 0
            if count <= 10:
                print(f"    Appears {count} times: {freq:,} PC pairs ({percentage:.2f}%)")
            else:  # This is our >10 group
                print(f"    Appears >10 times: {freq:,} PC pairs ({percentage:.2f}%)")
        
        print("\n  Most common strides (in bytes):")
        for stride, count in data['common_strides']:
            percentage = (count / data['total_transitions']) * 100 if data['total_transitions'] > 0 else 0
            # Convert stride to a more readable form (in cachelines)
            cacheline_stride = stride / 64  # Assuming 64-byte cachelines
            stride_description = (
                f"{stride} bytes "
                f"({cacheline_stride:.2f} cachelines)"
            )
            print(f"    {stride_description}: {count:,} occurrences ({percentage:.2f}%)")

def plot_basic_charts(results):
    """Create simple bar charts showing repeating vs non-repeating and contiguity"""
    workloads = sorted(results.keys())
    
    # 1. First chart: Repeating vs Non-repeating pairs
    fig1, ax1 = plt.subplots(figsize=(12, 7))
    
    repeating_counts = [results[workload]['repeating_pairs'] for workload in workloads]
    non_repeating_counts = [results[workload]['non_repeating_pairs'] for workload in workloads]
    total_counts = [rep + non_rep for rep, non_rep in zip(repeating_counts, non_repeating_counts)]
    
    # Calculate percentages
    repeating_pct = [(rep / total) * 100 if total > 0 else 0 for rep, total in zip(repeating_counts, total_counts)]
    non_repeating_pct = [(non_rep / total) * 100 if total > 0 else 0 for non_rep, total in zip(non_repeating_counts, total_counts)]
    
    # Set up the bar positions
    indices = np.arange(len(workloads))
    
    # Create the stacked bars
    bars1 = ax1.bar(indices, repeating_pct, label='Repeating PC Pairs', color='#1f77b4')
    bars2 = ax1.bar(indices, non_repeating_pct, bottom=repeating_pct, label='Non-repeating PC Pairs', color='#ff7f0e')
    
    # Add concrete numbers as text
    for i, workload in enumerate(workloads):
        total = total_counts[i]
        rep = repeating_counts[i]
        non_rep = non_repeating_counts[i]
        ax1.text(i, 105, f"Total: {total:,}\nRep: {rep:,}\nNon-rep: {non_rep:,}", ha='center', fontsize=8,
                bbox=dict(facecolor='white', alpha=0.7, boxstyle='round'))
    
    ax1.set_ylim(0, 120)  # Leave room for annotations
    ax1.set_ylabel('Percentage of PC Pairs (%)')
    ax1.set_xlabel('Datacenter Applications')
    ax1.set_title('Distribution of Repeating vs Non-repeating PC Pairs', fontsize=14, fontweight='bold')
    ax1.set_xticks(indices)
    ax1.set_xticklabels([w.capitalize() for w in workloads])
    
    # Move legend outside the chart
    ax1.legend(bbox_to_anchor=(1.05, 1), loc='upper left')
    
    ax1.grid(axis='y', alpha=0.3)
    
    # Add explanatory note
    fig1.text(0.5, 0.01, "Note: Repeating PC pairs appear multiple times in the trace",
             ha='center', fontsize=10, bbox=dict(facecolor='#ffffcc', alpha=0.8))
    
    # Adjust layout to make room for external legend
    plt.tight_layout(rect=[0, 0.05, 0.85, 0.95])
    plt.savefig('pc_pair_repetition.png', dpi=300, bbox_inches='tight')
    plt.close()
    
    # 2. Second chart: Repeating pairs breakdown - same vs different cachelines
    fig2, ax2 = plt.subplots(figsize=(12, 7))
    
    # Prepare data
    same_cl_counts = [results[workload]['same_cacheline_repeating'] for workload in workloads]
    multi_cl_counts = [results[workload]['multi_cacheline_repeating'] for workload in workloads]
    
    # Calculate percentages
    repeating_totals = [same + multi for same, multi in zip(same_cl_counts, multi_cl_counts)]
    same_cl_pct = [(same / total * 100) if total > 0 else 0 for same, total in zip(same_cl_counts, repeating_totals)]
    multi_cl_pct = [(multi / total * 100) if total > 0 else 0 for multi, total in zip(multi_cl_counts, repeating_totals)]
    
    # Create the stacked bars
    bars3 = ax2.bar(indices, same_cl_pct, label='Access Same Cacheline', color='#9467bd')
    bars4 = ax2.bar(indices, multi_cl_pct, bottom=same_cl_pct, label='Access Multiple Cachelines', color='#2ca02c')
    
    # Add concrete numbers as text
    for i, workload in enumerate(workloads):
        same_cl = same_cl_counts[i]
        multi_cl = multi_cl_counts[i]
        total = same_cl + multi_cl
        ax2.text(i, 105, f"Total repeating: {total:,}\nSame CL: {same_cl:,}\nMulti CL: {multi_cl:,}", ha='center', fontsize=8,
                bbox=dict(facecolor='white', alpha=0.7, boxstyle='round'))
    
    ax2.set_ylim(0, 120)  # Leave room for annotations
    ax2.set_ylabel('Percentage of Repeating PC Pairs (%)')
    ax2.set_xlabel('Datacenter Applications')
    ax2.set_title('Repeating PC Pairs: Access Pattern Breakdown', fontsize=14, fontweight='bold')
    ax2.set_xticks(indices)
    ax2.set_xticklabels([w.capitalize() for w in workloads])
    
    # Move legend outside the chart
    ax2.legend(bbox_to_anchor=(1.05, 1), loc='upper left')
    
    ax2.grid(axis='y', alpha=0.3)
    
    # Add explanatory note
    fig2.text(0.5, 0.01, "Note: Only repeating PC pairs are included in this analysis",
             ha='center', fontsize=10, bbox=dict(facecolor='#ffffcc', alpha=0.8))
    
    # Adjust layout to make room for external legend
    plt.tight_layout(rect=[0, 0.05, 0.85, 0.95])
    plt.savefig('repeating_pairs_breakdown.png', dpi=300, bbox_inches='tight')
    plt.close()
    
    # 3. Third chart: Contiguity patterns for transition-capable pairs
    fig3, ax3 = plt.subplots(figsize=(12, 7))
    
    # Prepare data
    contiguous_data = [results[workload]['contiguous_pct'] for workload in workloads]
    near_data = [results[workload]['near_pct'] for workload in workloads]
    scattered_data = [results[workload]['scattered_pct'] for workload in workloads]
    
    # Create the stacked bars
    bars5 = ax3.bar(indices, contiguous_data, label='Contiguous (1 cacheline)', color='#2ca02c')
    bars6 = ax3.bar(indices, near_data, bottom=contiguous_data, label='Near (2-16 cachelines)', color='#ff7f0e')
    
    # Calculate the bottom positions for scattered bars
    bottom_scattered = [c + n for c, n in zip(contiguous_data, near_data)]
    bars7 = ax3.bar(indices, scattered_data, bottom=bottom_scattered, label='Scattered (>16 cachelines)', color='#d62728')
    
    # Add transition counts as text
    for i, workload in enumerate(workloads):
        transitions = results[workload]['total_transitions']
        capable_pairs = len(results[workload]['transition_capable_pairs'])
        ax3.text(i, 105, f"{transitions:,} transitions\nfrom {capable_pairs:,} pairs", 
                ha='center', fontsize=8, bbox=dict(facecolor='white', alpha=0.7, boxstyle='round'))
    
    ax3.set_ylim(0, 120)  # Leave room for annotations
    ax3.set_ylabel('Percentage of Transitions (%)')
    ax3.set_xlabel('Datacenter Applications')
    ax3.set_title('Spatial Locality of Memory Accesses (Transition-Capable PC Pairs Only)', fontsize=14, fontweight='bold')
    ax3.set_xticks(indices)
    ax3.set_xticklabels([w.capitalize() for w in workloads])
    
    # Move legend outside the chart
    ax3.legend(bbox_to_anchor=(1.05, 1), loc='upper left')
    
    ax3.grid(axis='y', alpha=0.3)
    
    # Add explanatory note
    fig3.text(0.5, 0.01, "NOTE: Analysis is based ONLY on PC pairs that access multiple different cachelines",
             ha='center', fontsize=10, bbox=dict(facecolor='#ffffcc', alpha=0.8))
    
    # Adjust layout to make room for external legend
    plt.tight_layout(rect=[0, 0.05, 0.85, 0.95])
    plt.savefig('spatial_locality.png', dpi=300, bbox_inches='tight')
    plt.close()

def main():
    # Perform the analysis
    results = analyze_memory_access_patterns()
    
    # Generate the report
    generate_spatial_locality_report(results)
    
    # Generate basic charts
    try:
        plot_basic_charts(results)
        print("\nAnalysis complete. Basic visualizations saved to:")
        print("  - pc_pair_repetition.png")
        print("  - repeating_pairs_breakdown.png")
        print("  - spatial_locality.png")
    except Exception as e:
        print(f"\nError generating visualizations: {e}")
        print("Text report was still generated successfully.")

if __name__ == "__main__":
    main()