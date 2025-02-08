#!/bin/bash

# Define LQ and SQ size configurations
LQ_SQ_SIZES=(
    "128 72"
    "64 36"
    "96 72"
    "128 56"
    "160 72"
    "128 96"
    "160 96"
    "192 128"
)

# Path to trace file
TRACE_PATH="/users/deepmish/datacenterGz/cassandra/trace.gz"

# Instruction limit
INST_LIMIT=100000000  # 100 million instructions

# Base results directory
RESULTS_DIR="./scarab_results"
mkdir -p "$RESULTS_DIR"

# List of statistic files to copy (modify if needed)
STAT_FILES=(
    "core.stat.0.out"
    "core.stat.0.csv"
    "bp.stat.0.out"
    "bp.stat.0.csv"
    "inst.stat.0.out"
    "inst.stat.0.csv"
    "memory.stat.0.out"
    "memory.stat.0.csv"
    "power.stat.0.out"
    "power.stat.0.csv"
    "fetch.stat.0.out"
    "fetch.stat.0.csv"
    "pref.stat.0.out"
    "pref.stat.0.csv"
)

# Run Scarab for each LQ/SQ configuration
for SIZE in "${LQ_SQ_SIZES[@]}"; do
    LQ=$(echo $SIZE | awk '{print $1}')
    SQ=$(echo $SIZE | awk '{print $2}')
    
    # Create a separate directory for each LQ/SQ configuration
    CONFIG_DIR="$RESULTS_DIR/LQ${LQ}_SQ${SQ}"
    mkdir -p "$CONFIG_DIR"

    echo "Running Scarab with LQ=$LQ and SQ=$SQ..."
    
    # Run Scarab
    ./scarab --frontend pt --fetch_off_path_ops 0 \
        --cbp_trace_r0="$TRACE_PATH" --inst_limit "$INST_LIMIT" \
        --load_queue_entries "$LQ" --store_queue_entries "$SQ" \
        > "$CONFIG_DIR/run_output.txt" 2>&1
    
    echo "Scarab run completed. Copying stat files..."
    
    # Copy stat files to the corresponding config directory
    for FILE in "${STAT_FILES[@]}"; do
        if [ -f "$FILE" ]; then
            cp "$FILE" "$CONFIG_DIR/"
        fi
    done

    echo "Results saved in $CONFIG_DIR"
done

echo "All experiments completed. Check results in $RESULTS_DIR"
