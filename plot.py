import os
import re
import numpy as np
import matplotlib.pyplot as plt
from datetime import datetime

def analyze_cacheline_patterns(base_dir):
    # Set matplotlib params for a professional look
    plt.rcParams['figure.facecolor'] = 'white'
    plt.rcParams['axes.facecolor'] = 'white'
    plt.rcParams['grid.alpha'] = 0.3
    plt.rcParams['grid.color'] = '#cccccc'
    
    # Use consistent color
    bar_color = '#90EE90'
    
    # Workload name mapping
    workload_names = {
        'bfs': 'Breadth-First Search (BFS) - Direction Optimizing',
        'sssp': 'Single-Source Shortest Paths (SSSP)',
        'pr': 'PageRank (PR)',
        'cc': 'Connected Components (CC)',
        'bc': 'Betweenness Centrality (BC)',
        'tc': 'Triangle Counting (TC)'
    }
    
    data = {}
    # Read data
    for workload in os.listdir(base_dir):
        fetch_file = f"{base_dir}/{workload}/fetch.stat.0_.out"
        if not os.path.exists(fetch_file):
            continue
            
        distances = {i: 0 for i in range(65)}
        beyond_64_count = 0
        
        with open(fetch_file) as f:
            for line in f:
                match = re.match(r"SAME_CACHELINE_DISTANCE_(\d+)\s+(\d+)", line)
                if match:
                    distances[int(match.group(1))] = int(match.group(2))
                beyond_match = re.match(r"BEYOND_64\s+(\d+)", line)
                if beyond_match:
                    beyond_64_count = int(beyond_match.group(1))
                    
        data[workload] = {
            'distances': {k: v for k, v in distances.items() if k < 64},
            'beyond_64': beyond_64_count
        }

    # Plot regular distances (0-63)
    workloads = list(data.keys())
    n_workloads = len(workloads)
    
    fig, axes = plt.subplots(n_workloads, 1, figsize=(15, 6*n_workloads), dpi=150)
    if n_workloads == 1:
        axes = [axes]
    
    def format_value(value):
        if value >= 1e6:
            return f'{value/1e6:.1f}M'
        elif value >= 1e3:
            return f'{value/1e3:.1f}K'
        return str(value)
    
    for idx, workload in enumerate(workloads):
        distances = sorted(range(64))
        values = [data[workload]['distances'][d] for d in distances]
        
        # Create bars with consistent color
        bars = axes[idx].bar(distances, values, 
                           alpha=0.75,
                           color=bar_color,
                           edgecolor='#2f4f4f',
                           linewidth=0.5)
        
        # Add value labels on top of bars with improved positioning
        max_height = max(values)
        for bar in bars:
            height = bar.get_height()
            if height > 0:  # Only add labels for non-zero values
                # Calculate offset based on maximum height
                offset = max_height * 0.02  # 2% of max height
                axes[idx].text(bar.get_x() + bar.get_width()/2., height + offset,
                             format_value(height),
                             ha='center', va='bottom',
                             rotation=90,
                             fontsize=8)
        
        # Set y-axis limit to accommodate labels
        axes[idx].set_ylim(0, max_height * 1.15)  # Add 15% padding for labels
        
        # Enhance the subplot
        axes[idx].set_xlabel("Cacheline Distance (0-63)", fontsize=12)
        axes[idx].set_ylabel("Number of Accesses", fontsize=12)
        axes[idx].set_title(f"Cacheline Access Pattern Analysis - {workload_names.get(workload, workload)}",
                          fontsize=14, pad=20)
        axes[idx].set_xticks(range(0, 64, 4))
        axes[idx].grid(True, linestyle='--', alpha=0.3, color='gray')
        
        # Set axis below bars
        axes[idx].set_axisbelow(True)
        
        # Scientific notation for y-axis if values are large
        axes[idx].ticklabel_format(style='sci', axis='y', scilimits=(0,0))
        
        # Add minor ticks
        axes[idx].minorticks_on()
        
    plt.tight_layout(pad=3.0)
    
    # Add a super title
    fig.suptitle("Detailed Analysis of Cacheline Access Patterns in Memory Subsystem",
                 fontsize=16, y=1.02)
    
    # Add timestamp
    plt.figtext(0.99, 0.01, f'Generated: {datetime.now().strftime("%Y-%m-%d %H:%M")}',
                ha='right', va='bottom', fontsize=8, style='italic')
    
    plt.savefig("cacheline_access_patterns_0_63.png", 
                bbox_inches='tight', 
                dpi=300)
    plt.show()

    # Plot Beyond 64 data
    plt.figure(figsize=(10, 6), dpi=150)
    beyond_values = [data[workload]['beyond_64'] for workload in workloads]
    workload_labels = [workload_names.get(w, w) for w in workloads]
    
    bars = plt.bar(workload_labels, beyond_values, 
                   alpha=0.75,
                   color=bar_color,
                   edgecolor='#2f4f4f',
                   linewidth=0.5)
    
    # Add value labels on top of bars with improved positioning
    max_height = max(beyond_values)
    for bar in bars:
        height = bar.get_height()
        plt.text(bar.get_x() + bar.get_width()/2., height + (max_height * 0.02),
                format_value(height),
                ha='center', va='bottom')
    
    # Set y-axis limit to accommodate labels
    plt.ylim(0, max_height * 1.15)
    
    plt.xlabel("Workloads", fontsize=12)
    plt.ylabel("Number of Accesses", fontsize=12)
    plt.title("Analysis of Beyond-64 Micro-op Distance Accesses", 
              fontsize=14, pad=20)
    plt.xticks(rotation=45, ha='right')
    plt.grid(True, linestyle='--', alpha=0.3)
    plt.tight_layout()
    
    # Add timestamp
    plt.figtext(0.99, 0.01, f'Generated: {datetime.now().strftime("%Y-%m-%d %H:%M")}',
                ha='right', va='bottom', fontsize=8, style='italic')
    
    plt.savefig("cacheline_access_patterns_beyond_64.png", 
                bbox_inches='tight',
                dpi=300)
    plt.show()

# Run analysis
analyze_cacheline_patterns("/users/deepmish/result_GAPS_cacheline_access_patterns")
