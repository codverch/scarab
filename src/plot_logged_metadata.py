import os
import re
import glob
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import seaborn as sns

# Define the distance categories
distance_categories = [
    (1, 10, "1-10"),
    (11, 20, "11-20"),
    (21, 30, "21-30"),
    (31, 40, "31-40"),
    (41, 50, "41-50"),
    (51, 60, "51-60"),
    (61, 70, "61-70"),
    (71, 80, "71-80"),
    (81, 90, "81-90"),
    (91, 100, "91-100"),
    (101, 999, "101-999"),
    (1000, 9999, "1000-9999"),
    (10000, float('inf'), ">10K")
]

def get_category(distance):
    """Get the category label for a given distance"""
    for min_val, max_val, label in distance_categories:
        if min_val <= distance <= max_val:
            return label
    return None  # Should not happen given our categories

def parse_log_files(directory_path):
    """Parse all log files in the directory and extract micro-op pair information"""
    all_data = []
    benchmarks = []
    
    # Get all text files in the directory
    log_files = glob.glob(os.path.join(directory_path, '*.txt'))
    
    for log_file in log_files:
        benchmark = os.path.basename(log_file).split('.')[0]
        benchmarks.append(benchmark)
        print(f"Processing {benchmark}...")
        
        with open(log_file, 'r') as f:
            content = f.readlines()
        
        # Skip header lines
        content = [line for line in content if line.startswith("Op1 PC:")]
        
        for line in content:
            # Extract fields using regex with more flexible spacing
            pattern = r'Op1 PC: (\w+)\s+Op2 PC: (\w+)\s+Op1 Cacheblock: (\w+)\s+Op2 Cacheblock: (\w+)\s+Op1 Offset: (\d+)\s+Op2 offset: (\d+)\s+Op1 base reg: (\d+)\s+Op2 base reg: (\d+)\s+Op1 micro-op id: (\d+)\s+Op2 micro-op id: (\d+)\s+Op1 mem size: (\d+)\s+Op2 mem size: (\d+)'
            
            match = re.match(pattern, line)
            if match:
                op1_pc, op2_pc, op1_cacheblock, op2_cacheblock, op1_offset, op2_offset, op1_base_reg, op2_base_reg, op1_micro_op_id, op2_micro_op_id, op1_mem_size, op2_mem_size = match.groups()
                
                # Convert to appropriate types
                op1_micro_op_id = int(op1_micro_op_id)
                op2_micro_op_id = int(op2_micro_op_id)
                op1_mem_size = int(op1_mem_size)
                op2_mem_size = int(op2_mem_size)
                op1_base_reg = int(op1_base_reg)
                op2_base_reg = int(op2_base_reg)
                
                # Calculate distance
                distance = op2_micro_op_id - op1_micro_op_id
                
                # Check same memory size and base register
                same_mem_size = op1_mem_size == op2_mem_size
                same_base_reg = op1_base_reg == op2_base_reg
                
                all_data.append({
                    'benchmark': benchmark,
                    'op1_pc': op1_pc,
                    'op2_pc': op2_pc,
                    'cacheblock': op1_cacheblock,  # Both cache blocks are same as per filters
                    'distance': distance,
                    'distance_category': get_category(distance),
                    'same_mem_size': same_mem_size,
                    'same_base_reg': same_base_reg
                })
            else:
                print(f"Warning: Could not parse line: {line.strip()}")
    
    return pd.DataFrame(all_data), benchmarks

def analyze_data(df, benchmark=None):
    """Analyze the data to generate required statistics"""
    # Filter data for specific benchmark if provided
    if benchmark:
        df = df[df['benchmark'] == benchmark]
    
    # Total number of micro-op pairs
    total_pairs = len(df)
    
    if total_pairs == 0:
        print(f"No data found for benchmark: {benchmark}")
        return None
    
    # Sort the distance categories properly
    ordered_categories = [cat[2] for cat in distance_categories]
    
    # Count by distance category (with same cache block - which is all entries)
    category_counts = df['distance_category'].value_counts()
    # Ensure all categories are present, even if count is 0
    for cat in ordered_categories:
        if cat not in category_counts.index:
            category_counts[cat] = 0
            
    # Sort by our predefined order
    category_counts = category_counts.reindex(ordered_categories)
    category_percentages = (category_counts / total_pairs) * 100
    
    # Count pairs with same memory size by distance category
    same_mem_size_df = df[df['same_mem_size'] == True]
    same_mem_size_counts = same_mem_size_df['distance_category'].value_counts()
    
    # Ensure all categories are present, even if count is 0
    for cat in ordered_categories:
        if cat not in same_mem_size_counts.index:
            same_mem_size_counts[cat] = 0
            
    # Sort by our predefined order and calculate percentages
    same_mem_size_counts = same_mem_size_counts.reindex(ordered_categories)
    
    # Calculate percentages relative to total pairs (all with same cache block)
    same_mem_size_percentages = pd.Series(index=ordered_categories, dtype=float)
    for cat in ordered_categories:
        same_mem_size_percentages[cat] = (same_mem_size_counts[cat] / total_pairs) * 100
    
    # Count pairs with same memory size and same base register by distance category
    same_mem_size_reg_df = same_mem_size_df[same_mem_size_df['same_base_reg'] == True]
    same_mem_size_reg_counts = same_mem_size_reg_df['distance_category'].value_counts()
    
    # Ensure all categories are present, even if count is 0
    for cat in ordered_categories:
        if cat not in same_mem_size_reg_counts.index:
            same_mem_size_reg_counts[cat] = 0
            
    # Sort by our predefined order and calculate percentages
    same_mem_size_reg_counts = same_mem_size_reg_counts.reindex(ordered_categories)
    
    # Calculate percentages relative to total pairs (all with same cache block)
    same_mem_size_reg_percentages = pd.Series(index=ordered_categories, dtype=float)
    for cat in ordered_categories:
        same_mem_size_reg_percentages[cat] = (same_mem_size_reg_counts[cat] / total_pairs) * 100
    
    return {
        'total_pairs': total_pairs,
        'category_percentages': category_percentages,
        'same_mem_size_percentages': same_mem_size_percentages,
        'same_mem_size_reg_percentages': same_mem_size_reg_percentages,
        'category_counts': category_counts,
        'same_mem_size_counts': same_mem_size_counts,
        'same_mem_size_reg_counts': same_mem_size_reg_counts
    }

def plot_side_by_side_only(results, output_dir, benchmark=None):
    """Generate only the side-by-side bar chart with specified custom styling"""
    # Create the output directory if it doesn't exist
    os.makedirs(output_dir, exist_ok=True)
    
    # Set up the style with clean white background
    plt.style.use('default')
    plt.rcParams.update({
        'font.size': 14,
        'font.family': 'sans-serif',
        'font.weight': 'bold',
        'axes.labelweight': 'bold',
        'axes.titleweight': 'bold',
        'axes.titlesize': 24,
        'axes.labelsize': 18,
        'xtick.labelsize': 14,
        'ytick.labelsize': 14,
        'figure.facecolor': 'white',
        'axes.facecolor': 'white',
        'axes.grid': True,
        'grid.color': '#E0E0E0',
        'grid.linestyle': '-',
        'grid.linewidth': 0.8,
    })
    
    # Set up categories order
    ordered_categories = [cat[2] for cat in distance_categories]
    
    benchmark_name = benchmark if benchmark else "All Benchmarks"
    
    # Define the custom colors as requested with brighter background colors
    colors = {
        'bar_colors': ['#111111', '#990000', '#F5D300'],  # Black, Carnegie Red, Yellow
        'very_short': '#EDF36E',  # Light yellow
        'short': '#97D985',       # Light green
        'medium': '#B6A4A0',      # Light blue
        'long': '#F26B5E'         # Light Carnegie red
    }
    
    # Define zones based on distance categories
    zones = [
        # Very short distance: 1-20
        (0, 2, colors['very_short'], 'Very Short\nDistance'),
        # Short distance: 21-50
        (2, 5, colors['short'], 'Short\nDistance'),
        # Medium distance: 51-100
        (5, 10, colors['medium'], 'Medium\nDistance'),
        # Long distance: >100
        (10, len(ordered_categories), colors['long'], 'Long\nDistance')
    ]
    
    # Prepare data for side-by-side bars
    data = pd.DataFrame({
        'Same Cacheblock': results['category_percentages'],
        'Same Mem Size': results['same_mem_size_percentages'],
        'Same Mem Size + Base Reg': results['same_mem_size_reg_percentages']
    })
    
    # Melt the data for side-by-side bars
    data_melted = data.reset_index()
    data_melted = pd.melt(data_melted, id_vars='index', var_name='Property', value_name='Percentage')
    
    # Create figure and adjust layout
    fig, ax = plt.subplots(figsize=(18, 10))
    
    # Create title at top
    plt.suptitle(f'Distance Between Fusion Candidates (in Micro-ops) - {benchmark_name}', 
                y=0.98, fontsize=24, fontweight='bold')
    
   # Add background zones with brighter aesthetic colors
    for idx, (start_idx, end_idx, color, label) in enumerate(zones):
        if 0 <= start_idx < len(ordered_categories) and start_idx < end_idx:
            end_idx = min(end_idx, len(ordered_categories))
            
            # Calculate x positions for the zone
            start_pos = start_idx - 0.5 if start_idx > 0 else -0.5
            width = end_idx - start_idx

            # Add colored background with brighter colors
            rect = plt.Rectangle(
                (start_pos, 0), 
                width, 
                100,  # Max at 100 to avoid "105.0" text
                facecolor=color,
                alpha=0.5,  # Semi-transparent
                edgecolor='#AAAAAA',
                linewidth=0.5,
                zorder=-1
            )
            ax.add_patch(rect)

            # Adjust label position to avoid overlap with the legend
            y_pos = 50 if idx == len(zones) - 1 else 75 + (idx * 5)

            # Create a darker version of the zone color for the text
            # Extract RGB components and make them darker
            r_hex = color[1:3]
            g_hex = color[3:5]
            b_hex = color[5:7]
            
            # Convert to integers
            r = int(r_hex, 16)
            g = int(g_hex, 16)
            b = int(b_hex, 16)
            
            # Make color darker (reduce by 60% but ensure it's not too dark)
            r = max(int(r * 0.4), 20)
            g = max(int(g * 0.4), 20)
            b = max(int(b * 0.4), 20)
            
            # Convert back to hex
            darker_color = f'#{r:02x}{g:02x}{b:02x}'
            
            # Add zone label with matching but darker text color
            ax.text(
                start_pos + width / 2, 
                y_pos,
                label,
                ha='center',
                va='center',
                fontsize=16,
                fontweight='bold',
                color=darker_color  # Darker version of zone color
            )

    # Plot grouped bars with specified colors - MOVED OUTSIDE THE LOOP
    bar_plot = sns.barplot(
        x='index', 
        y='Percentage', 
        hue='Property', 
        data=data_melted,
        palette=colors['bar_colors'],
        ax=ax,
        edgecolor='black',
        linewidth=1.0
    )
    
    # Add value labels on TOP of the bars
    bars = ax.patches
    
    # Get number of categories and properties
    n_categories = len(ordered_categories)
    n_properties = 3  # Same Cache Block, Same Mem Size, Same Mem Size + Base Reg
    
    # Calculate positions and add labels
    for i, bar in enumerate(bars):
        height = bar.get_height()
        
        # Skip very small values to avoid cluttering
        if height < 0.5:
            continue
            
        # Add label on TOP of each bar
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            height + 0.5,  # Position slightly above the bar
            f'{height:.1f}',
            ha='center',
            va='bottom',
            fontsize=9,
            fontweight='bold',
            color='black'
        )
    
    # Set axis limits and labels
    ax.set_ylim(0, 100)  # Set y-limit to 99 to remove the 100.0 label at top
    ax.set_ylabel('% of Load Micro-Op PC Pairs')
    ax.set_xlabel('Distance Between Load Micro-Op PC Pairs Accessing Same Cacheblock')
    
    # Set specific ticks to avoid unwanted values
    ax.set_yticks([0, 20, 40, 60, 80, 100])
    ax.yaxis.set_major_formatter('{:.0f}'.format)
    
    # Improve legend - place at top right of the plot
    handles, labels = ax.get_legend_handles_labels()
    ax.legend(handles, labels, 
              loc='upper right',  # Position at top right as requested
              frameon=True, 
              fontsize=12)
    
    # Rotate x-tick labels
    plt.xticks(rotation=45, ha='right')
    
    # Adjust layout and save with high quality
    plt.tight_layout(rect=[0, 0, 1, 0.95])  # Leave space for title
    plt.savefig(os.path.join(output_dir, f'{benchmark_name.lower().replace(" ", "_")}_side_by_side.png'), 
               dpi=300, bbox_inches='tight', facecolor='white')
    plt.close()

def save_statistics(results, output_file, benchmark=None):
    """Save the statistics to a text file"""
    ordered_categories = [cat[2] for cat in distance_categories]
    benchmark_name = benchmark if benchmark else "All Benchmarks"
    
    with open(output_file, 'w') as f:
        f.write(f"Statistics for {benchmark_name}\n")
        f.write("=" * 80 + "\n\n")
        
        f.write(f"Total micro-op pairs (all with same cache block): {results['total_pairs']}\n\n")
        
        f.write("Distribution by distance category:\n")
        for category in ordered_categories:
            count = results['category_counts'][category]
            percentage = results['category_percentages'][category]
            f.write(f"  {category}: {count} pairs ({percentage:.2f}% of total)\n")
        
        f.write("\nPairs with same cache block + same memory size by distance category:\n")
        for category in ordered_categories:
            count = results['same_mem_size_counts'][category]
            percentage = results['same_mem_size_percentages'][category]
            f.write(f"  {category}: {count} pairs ({percentage:.2f}% of total)\n")
        
        f.write("\nPairs with same cache block + same memory size + same base register by distance category:\n")
        for category in ordered_categories:
            count = results['same_mem_size_reg_counts'][category]
            percentage = results['same_mem_size_reg_percentages'][category]
            f.write(f"  {category}: {count} pairs ({percentage:.2f}% of total)\n")

def main():
    # Set the path to the log files directory
    log_directory = "/users/deepmish/scarab/src/logging_metadata"
    
    # Create a results directory
    results_directory = "/users/deepmish/scarab/src/results"
    os.makedirs(results_directory, exist_ok=True)
    
    # Get ordered categories from the distance_categories list
    ordered_categories = [cat[2] for cat in distance_categories]
    
    # Parse log files
    print(f"Parsing log files from {log_directory}...")
    df, benchmarks = parse_log_files(log_directory)
    
    if df.empty:
        print("No data found in log files.")
        return
    
    print(f"Found {len(df)} micro-op pairs across all benchmarks.")
    
    # First, analyze and plot all data combined
    print("\nAnalyzing data for all benchmarks combined...")
    all_results = analyze_data(df)
    
    print(f"Generating plots for all benchmarks combined in {results_directory}...")
    plot_side_by_side_only(all_results, results_directory)
    
    # Save statistics for all benchmarks combined
    all_stats_file = os.path.join(results_directory, 'all_statistics.txt')
    save_statistics(all_results, all_stats_file)
    print(f"Statistics saved to {all_stats_file}")
    
    # Now analyze and plot each benchmark separately
    for benchmark in benchmarks:
        print(f"\nAnalyzing data for {benchmark}...")
        benchmark_directory = os.path.join(results_directory, benchmark)
        os.makedirs(benchmark_directory, exist_ok=True)
        
        benchmark_results = analyze_data(df, benchmark)
        if benchmark_results:
            print(f"Generating plots for {benchmark} in {benchmark_directory}...")
            plot_side_by_side_only(benchmark_results, benchmark_directory, benchmark)
            
            # Save statistics for this benchmark
            benchmark_stats_file = os.path.join(benchmark_directory, 'statistics.txt')
            save_statistics(benchmark_results, benchmark_stats_file, benchmark)
            print(f"Statistics saved to {benchmark_stats_file}")
    
    print("\nAll plots and statistics generated successfully!")
    print(f"Results saved in {results_directory}")

if __name__ == "__main__":
    main()