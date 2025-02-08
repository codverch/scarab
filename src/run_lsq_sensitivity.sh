#!/bin/bash

directory="/users/deepmish/simpoint_traces"
pathToScarabDir="/users/deepmish/scarab/src"

baseline_options=" "

INST_LIMIT=100000000
WARMUP_INSTS=45000000

# Set this equal to the maximum threads on your device
max_simul_proc=23

currdir=$(pwd)

prefix="vanilla"

# Define LQ and SQ sizes
LQ_SQ_SIZES=(
    "128 72"   # Default; LQ 128, SQ 72
    "64 36"    # Half the default; LQ 64, SQ 36
    "256 144"  # 2x the default; LQ 256, SQ 144
    "512 288"  # 4x the default; LQ 512, SQ 288
)

# Collect workload filenames
for file in "$directory"/*; do
    if [[ ! "$file" =~ \.(gz|tar|zip)$ ]]; then
        workloads+=("$(basename "$file")")
    fi
done

results_dir="result_$prefix"
rm -rf $results_dir
mkdir $results_dir

check_running_procs() {
    while (( $(jobs -r | wc -l) >= max_simul_proc )); do
        sleep 1
    done
}

pids=()

cd $pathToScarabDir

# Loop over LQ and SQ sizes
for SIZE in "${LQ_SQ_SIZES[@]}"; do
    LQ=$(echo $SIZE | awk '{print $1}')
    SQ=$(echo $SIZE | awk '{print $2}')
    CONFIG_DIR="$results_dir/LQ${LQ}_SQ${SQ}"
    mkdir -p "$CONFIG_DIR"

    for workload in ${workloads[@]}; do
        trace_dir="$directory/$workload"
        trace_count=0

        for trace_file in "$trace_dir/traces_simp/trace/*.zip"; do
            trace_number=$(basename "$trace_file" .zip)
            RED='\033[0;31m'
            BLUE='\033[0;34m'
            NC='\033[0m' # No Color

            check_running_procs

            if [[ trace_count -eq 0 ]]; then
                if [[ $workload == pt_* ]]; then
                    echo -ne "${RED}$workload${NC} is pt trace, processing ${BLUE}trace.gz${NC}                  \r"
                else
                    echo -ne "${RED}$workload${NC} is a dynamorio trace, processing ${BLUE}$trace_number.zip${NC}                         \r"
                fi
            else
                echo -ne "${RED}$workload${NC} is a dynamorio trace, processing ${BLUE}$trace_count${NC} zips                        \r"
            fi

            if [[ $workload == pt_* ]]; then
                $pathToScarabDir/scarab --frontend pt --fetch_off_path_ops 0 $baseline_options \
                    --cbp_trace_r0="$directory/$workload/trace.gz" --full_warmup $WARMUP_INSTS --inst_limit $INST_LIMIT \
                    --load_queue_entries "$LQ" --store_queue_entries "$SQ" \
                    > "$CONFIG_DIR/${workload}_${trace_number}.txt" &
            else
                $pathToScarabDir/scarab --frontend memtrace --fetch_off_path_ops 0 $baseline_options \
                    --cbp_trace_r0="$trace_file" --memtrace_modules_log="$trace_dir/traces_simp/bin" \
                    --full_warmup $WARMUP_INSTS --inst_limit $INST_LIMIT \
                    --load_queue_entries "$LQ" --store_queue_entries "$SQ" \
                    > "$CONFIG_DIR/${workload}_${trace_number}.txt" &
            fi

            pids+=($!)
            trace_count=$((trace_count + 1))
        done
    done
done

# Wait for all background processes to finish
for pid in ${pids[@]}; do
    wait $pid
done

cd $currdir/$results_dir
printf "Total instructions are %s of which %s are warmup\n" "$INST_LIMIT" "$WARMUP_INSTS"

declare -A total_insts
declare -A total_cycles

# Process each output file to calculate totals
for CONFIG_DIR in "$results_dir"/LQ*; do
    echo "Processing directory: $CONFIG_DIR"
    for file in "$CONFIG_DIR"/*.txt; do
        echo "  Processing file: $file"
        while IFS= read -r line; do
            if [[ $line == *"insts:"* ]]; then
                insts=$(echo $line | grep -oP 'insts:\K[0-9]+')
                cycles=$(echo $line | grep -oP 'cycles:\K[0-9]+')
                total_insts[$CONFIG_DIR]=$(( ${total_insts[$CONFIG_DIR]:-0} + $insts ))
                total_cycles[$CONFIG_DIR]=$(( ${total_cycles[$CONFIG_DIR]:-0} + $cycles ))
                echo "    Found insts: $insts, cycles: $cycles"
                break
            fi
        done < "$file"
    done
done

# Write IPC to a file for each directory
for CONFIG_DIR in "${!total_insts[@]}"; do
    insts=${total_insts[$CONFIG_DIR]}
    cycles=${total_cycles[$CONFIG_DIR]}
    ipc=$(echo "scale=2; $insts / $cycles" | bc -l)
    echo "IPC: $ipc" > "$CONFIG_DIR/ipc.txt"
done

# Define color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

printf "${BLUE}%-20s\t%-12s\t%-12s\t%-12s${NC}\n" "LQ / SQ Size" "Total Insts" "Total Cycles" "Effective IPC"

for CONFIG_DIR in "${!total_insts[@]}"; do
    insts=${total_insts[$CONFIG_DIR]}
    cycles=${total_cycles[$CONFIG_DIR]}
    ipc=$(echo "scale=2; $insts / $cycles" | bc -l)
    config_name=$(basename "$CONFIG_DIR")
    printf "${RED}%-20s${NC}\t${GREEN}%-12s${NC}\t${GREEN}%-12s${NC}\t${GREEN}%-12.2f${NC}\n" "$config_name" "$insts" "$cycles" "$ipc"
done

echo "All experiments completed. Check results in $results_dir"
