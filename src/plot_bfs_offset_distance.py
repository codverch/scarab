import os
import re
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from collections import defaultdict

def parse_dump_file(file_path):
    """Parse the bfs_offset_dump.txt file and extract relevant information."""
    data = []
    
    # Check if file exists
    if not os.path.exists(file_path):
        print(f"Error: File {file_path} not found.")
        return None
    
    with open(file_path, 'r') as f:
        lines = f.readlines()
    
    # Skip header lines
    for line in lines:
        if line.startswith("Op1 PC:"):
            # Extract data using regex
            pattern = r"Op1 PC: ([\da-f]+), Op2 PC: ([\da-f]+), Op1 Cacheline: ([\da-f]+), Op2 Cacheline: ([\da-f]+), Op1 Mem size: (\d+), Op2 Mem size: (\d+), Op1 Base reg: (\d+), Op2 Base reg: (\d+), Op1 byte in block offset: (\d+), Op2 byte in block offset: (\d+)"
            match = re.search(pattern, line)
            
            if match:
                entry = {
                    'op1_pc': int(match.group(1), 16),
                    'op2_pc': int(match.group(2), 16),
                    'op1_cacheline': int(match.group(3), 16),
                    'op2_cacheline': int(match.group(4), 16),
                    'op1_mem_size': int(match.group(5)),
                    'op2_mem_size': int(match.group(6)),
                    'op1_base_reg': int(match.group(7)),
                    'op2_base_reg': int(match.group(8)),
                    'op1_offset': int(match.group(9)),
                    'op2_offset': int(match.group(10))
                }
                data.append(entry)
    
    return data

def analyze_memory_accesses(data):
    """Analyze memory access patterns and cacheline spans."""
    # Cacheline size is typically 64 bytes
    CACHELINE_SIZE = 64
    
    access_patterns = {
        'overlapping': 0,
        'adjacent': 0,
        'distant': 0
    }
    
    # Add overlap details
    overlap_types = {
        'fully_overlapping': 0,
        'partially_overlapping': 0
    }
    
    cacheline_spans = {
        'op1_spans': 0,
        'op2_spans': 0,
        'both_span': 0,
        'neither_spans': 0
    }
    
    for entry in data:
        # Calculate memory regions accessed by each operation
        op1_start = entry['op1_offset']
        op1_end = op1_start + entry['op1_mem_size'] - 1
        
        op2_start = entry['op2_offset']
        op2_end = op2_start + entry['op2_mem_size'] - 1
        
        # Check if accesses are in the same cacheline
        same_cacheline = entry['op1_cacheline'] == entry['op2_cacheline']
        
        # Determine access pattern
        if op1_end >= op2_start and op2_end >= op1_start and same_cacheline:
            access_patterns['overlapping'] += 1
            
            # Determine if fully or partially overlapping
            op1_range = set(range(op1_start, op1_end + 1))
            op2_range = set(range(op2_start, op2_end + 1))
            
            if op1_range == op2_range:
                overlap_types['fully_overlapping'] += 1
            else:
                overlap_types['partially_overlapping'] += 1
                
        elif (op1_end + 1 == op2_start or op2_end + 1 == op1_start) and same_cacheline:
            access_patterns['adjacent'] += 1
        else:
            access_patterns['distant'] += 1
        
        # Check for cacheline spans
        op1_spans = (op1_start + entry['op1_mem_size'] > CACHELINE_SIZE)
        op2_spans = (op2_start + entry['op2_mem_size'] > CACHELINE_SIZE)
        
        if op1_spans and op2_spans:
            cacheline_spans['both_span'] += 1
        elif op1_spans:
            cacheline_spans['op1_spans'] += 1
        elif op2_spans:
            cacheline_spans['op2_spans'] += 1
        else:
            cacheline_spans['neither_spans'] += 1
    
    # Calculate percentages
    total = len(data)
    access_percentages = {key: (value / total) * 100 for key, value in access_patterns.items()}
    span_percentages = {key: (value / total) * 100 for key, value in cacheline_spans.items()}
    
    # Calculate overlap percentages (based on total overlapping pairs)
    total_overlapping = access_patterns['overlapping']
    if total_overlapping > 0:
        overlap_percentages = {key: (value / total_overlapping) * 100 for key, value in overlap_types.items()}
    else:
        overlap_percentages = {key: 0 for key in overlap_types.keys()}
    
    return access_percentages, span_percentages, overlap_percentages, total

def create_graphs(access_percentages, span_percentages, overlap_percentages, total):
    """Create visualizations for the analysis results with detailed research-oriented labels."""
    # Graph 1: Memory Access Patterns
    labels1 = list(access_percentages.keys())
    values1 = list(access_percentages.values())
    
    plt.figure(figsize=(12, 8))
    
    # More descriptive labels for research context
    readable_labels1 = {
        'overlapping': 'Overlapping\nBytes',
        'adjacent': 'Adjacent\nBytes',
        'distant': 'Distant\nBytes'
    }
    plot_labels1 = [readable_labels1[label] for label in labels1]
    
    colors1 = ['#3498db', '#2ecc71', '#e74c3c']
    bars1 = plt.bar(plot_labels1, values1, color=colors1, width=0.6)
    
    plt.title('Distribution of Memory Access Patterns in BFS Load Micro-op PC Pairs\nWithin Shared Cacheblocks', fontsize=14, fontweight='bold')
    plt.ylabel('Percentage of Load Micro-op PC Pairs (%)', fontsize=12)
    plt.ylim(0, max(values1) * 1.2)  # Dynamic y-limit based on data
    plt.grid(axis='y', linestyle='--', alpha=0.7)
    
    # Add percentage labels on bars
    for bar in bars1:
        height = bar.get_height()
        plt.text(bar.get_x() + bar.get_width()/2., height + 1,
                f"{height:.1f}%", ha='center', fontsize=11, fontweight='bold')
    
    plt.tight_layout()
    plt.savefig('bfs_memory_access_patterns.png', dpi=300)
    
    # Graph 2: Cacheline Span Analysis
    labels2 = list(span_percentages.keys())
    values2 = list(span_percentages.values())
    
    plt.figure(figsize=(12, 8))
    
    # More descriptive labels for research context
    readable_labels2 = {
        'op1_spans': 'Op1 Spans\nCacheline',
        'op2_spans': 'Op2 Spans\nCacheline',
        'both_span': 'Both Span\nCacheline',
        'neither_spans': 'No Cacheline\nSpans'
    }
    plot_labels2 = [readable_labels2[label] for label in labels2]
    
    colors2 = ['#f39c12', '#9b59b6', '#1abc9c', '#34495e']
    bars2 = plt.bar(plot_labels2, values2, color=colors2, width=0.6)
    
    plt.title('Cacheline Boundary Analysis of BFS Load Micro-op PC Pairs\nAccessing Shared Cacheblocks', fontsize=14, fontweight='bold')
    plt.ylabel('Percentage of Load Micro-op PC Pairs (%)', fontsize=12)
    plt.ylim(0, max(values2) * 1.2)  # Dynamic y-limit based on data
    plt.grid(axis='y', linestyle='--', alpha=0.7)
    
    # Add percentage labels on bars
    for bar in bars2:
        height = bar.get_height()
        plt.text(bar.get_x() + bar.get_width()/2., height + 1,
                f"{height:.1f}%", ha='center', fontsize=11, fontweight='bold')
    
    # Add a note about cacheline size
    plt.figtext(0.5, 0.01, "Analysis based on 64-byte cacheline size", 
                ha="center", fontsize=10, style='italic')
    
    plt.tight_layout()
    plt.savefig('bfs_cacheline_span_analysis.png', dpi=300)
    
    # Graph 3: Overlap Type Analysis
    labels3 = list(overlap_percentages.keys())
    values3 = list(overlap_percentages.values())
    
    plt.figure(figsize=(10, 7))
    
    # More descriptive labels for research context
    readable_labels3 = {
        'fully_overlapping': 'Fully\nOverlapping',
        'partially_overlapping': 'Partially\nOverlapping'
    }
    plot_labels3 = [readable_labels3[label] for label in labels3]
    
    colors3 = ['#3498db', '#9b59b6']
    bars3 = plt.bar(plot_labels3, values3, color=colors3, width=0.5)
    
    plt.title('Types of Byte Overlaps in BFS Load Micro-op PC Pairs\nWithin Shared Cacheblocks', fontsize=14, fontweight='bold')
    plt.ylabel('Percentage of Overlapping Pairs (%)', fontsize=12)
    plt.ylim(0, max(values3) * 1.2 if max(values3) > 0 else 100)  # Dynamic y-limit based on data
    plt.grid(axis='y', linestyle='--', alpha=0.7)
    
    # Add percentage labels on bars
    for bar in bars3:
        height = bar.get_height()
        plt.text(bar.get_x() + bar.get_width()/2., height + 1,
                f"{height:.1f}%", ha='center', fontsize=11, fontweight='bold')
    
    # Add a note about calculation basis
    plt.figtext(0.5, 0.01, "Percentages calculated from total overlapping pairs", 
                ha="center", fontsize=10, style='italic')
    
    plt.tight_layout()
    plt.savefig('bfs_overlap_type_analysis.png', dpi=300)
    
    print("Graphs saved as 'bfs_memory_access_patterns.png', 'bfs_cacheline_span_analysis.png', and 'bfs_overlap_type_analysis.png'")

def main():
    # File path
    file_path = "/users/deepmish/scarab/src/bfs_offset_dump.txt"
    
    # Parse the file
    data = parse_dump_file(file_path)
    
    if data:
        # Analyze the data
        access_percentages, span_percentages, overlap_percentages, total = analyze_memory_accesses(data)
        
        # Print analysis results
        print(f"Total BFS Load Micro-op PC Pairs analyzed: {total}")
        print("\nMemory Access Patterns within Shared Cacheblocks:")
        for pattern, percentage in access_percentages.items():
            print(f"  {pattern.capitalize()}: {percentage:.2f}%")
        
        print("\nCacheline Boundary Analysis:")
        for span, percentage in span_percentages.items():
            print(f"  {span.replace('_', ' ').capitalize()}: {percentage:.2f}%")
        
        print("\nOverlap Type Analysis:")
        for overlap, percentage in overlap_percentages.items():
            print(f"  {overlap.replace('_', ' ').capitalize()}: {percentage:.2f}%")
        
        # Create visualizations
        create_graphs(access_percentages, span_percentages, overlap_percentages, total)

if __name__ == "__main__":
    main()