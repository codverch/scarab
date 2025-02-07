#!/bin/bash

# Array of traces
traces=("clang" "gcc" "mongodb" "mysql" "postgres" "verilator" "xgboost")

# Function to extract IPC from the output
extract_ipc() {
    output=$1
    # Extract IPC value from the output
    ipc=$(echo "$output" | grep -oP '\d+\.\d+(?= IPC)')
    echo "$ipc"
}

# Function to run scarab and extract IPC
run_scarab_and_get_ipc() {
    TRACE=$1
    do_fusion=$2
    echo "Running for TRACE: $TRACE with fusion $do_fusion..."

    # Run scarab command and capture the output
    output=$(export TRACE=$TRACE && ./scarab --frontend memtrace --cbp_trace_r0=/home/humza/simpoint_traces/$TRACE/traces_simp/trace --memtrace_modules_log=/home/humza/simpoint_traces/$TRACE/traces_simp/bin --do_fusion $do_fusion)

    # Extract and print the IPC
    ipc=$(extract_ipc "$output")
    echo "IPC for $TRACE (fusion $do_fusion): $ipc"
}

# Run the commands in parallel for each trace
for TRACE in "${traces[@]}"; do
    # Run with fusion=0 and fusion=1 in parallel
    (
        run_scarab_and_get_ipc "$TRACE" 0
    ) &
    (
        run_scarab_and_get_ipc "$TRACE" 1
    ) &
done

# Wait for all background jobs to finish
wait

echo "All processes completed."
