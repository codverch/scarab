#!/bin/bash

# Directory to read files from
# directory="//home/humza/spectraces/"
directory="/home/humza/simpoint_traces/"
# scarab directory with PARAMS.in also present
pathToScarabDir="/home/humza/starting_again/cvscarab/src"

# copy over the baseline CLI options here
baseline_options=" --do_fusion 0"

INST_LIMIT=100000000
WARMUP_INSTS=45000000

# Set this equal to the maximum threads on your device
max_simul_proc=23

currdir=$(pwd)

prefix="no_fusion"

# Loop through all files in the directory and add them to the array
for file in "$directory"/*; do
    if [[ ! "$file" =~ \.(gz|tar|zip)$ ]]; then
        workloads+=("$(basename "$file")")
    fi
done

results_dir="result_$prefix"
# rm -rf $results_dir
mkdir $results_dir

ignore_list=()

check_running_procs() {
    while (( $(jobs -r | wc -l) >= max_simul_proc )); do
        sleep 1
    done
}

pids=()

cd $pathToScarabDir

for workload in ${workloads[@]}; do
    trace_dir="$directory/$workload"
    trace_count=0

    echo "$trace_dir"
    
    for trace_file in "$trace_dir"/traces_simp/trace/*.zip; do
    # for trace_file in "$trace_dir"/trace/*.zip; do
        trace_number=$(basename "$trace_file" .zip)
        RED='\033[0;31m'
        BLUE='\033[0;34m'
        NC='\033[0m' # No Color
        
        check_running_procs
        
        if [[ trace_count -eq 0 ]]; then
            if [[ $workload == pt_* ]]; then
                # echo -ne "${RED}$workload${NC} is pt trace, processing ${BLUE}trace.gz${NC}                  \r"
                echo -ne "Ignoring pt trace                                   \t\t\t\t\r"
            else
                echo -ne "${RED}$workload${NC} is a dynamorio trace, processing ${BLUE}$trace_number.zip${NC}                         \r"
            fi
        else
            echo -ne "${RED}$workload${NC} is a dynamorio trace, processing ${BLUE}$trace_count${NC} zips                        \r"
        fi

        if [[ $workload == pt_* ]]; then
            # $pathToScarabDir/scarab --frontend pt --fetch_off_path_ops 0 $baseline_options --cbp_trace_r0="$directory/$workload/trace.gz" --full_warmup $WARMUP_INSTS --inst_limit $INST_LIMIT > "$currdir/$results_dir/${workload}_${trace_number}.txt" &
            echo -ne ""
        else
            $pathToScarabDir/scarab --frontend memtrace --fetch_off_path_ops 0 $baseline_options --cbp_trace_r0="$trace_file" --memtrace_modules_log="$trace_dir/traces_simp/bin" --full_warmup $WARMUP_INSTS --inst_limit $INST_LIMIT > "$currdir/$results_dir/${workload}_${trace_number}.txt" &
        fi

        pids+=($!)
        trace_count=$((trace_count + 1))
    done
    # echo "$pathToScarabDir/scarab --frontend memtrace --cbp_trace_r0="$trace_dir/trace" --memtrace_modules_log="$trace_dir/raw" --full_warmup $WARMUP_INSTS --inst_limit $INST_LIMIT > "$currdir/$results_dir/${workload}_1.txt" &"
    
    # $pathToScarabDir/scarab --frontend memtrace --cbp_trace_r0="$trace_dir/trace" --memtrace_modules_log="$trace_dir/raw" --full_warmup $WARMUP_INSTS --inst_limit $INST_LIMIT > "$currdir/$results_dir/${workload}_1.txt" &

    pids+=($!)
    trace_count=$((trace_count + 1))

done

# Wait for all background processes to finish
for pid in ${pids[@]}; do
    wait $pid
done

cd $currdir/$results_dir
printf "Total instructions are %s of which %s are warmup\n" "$INST_LIMIT" "$WARMUP_INSTS"

declare -A total_insts
declare -A total_cycles

# Process each output file to group by workload type and calculate totals
for file in *.txt; do
    workload_type=$(echo $file | sed -e 's/_[^_]*\.txt$//')
    while IFS= read -r line; do
        if [[ $line == *"insts:"* ]]; then
            insts=$(echo $line | grep -oP 'insts:\K[0-9]+')
            cycles=$(echo $line | grep -oP 'cycles:\K[0-9]+')
            total_insts[$workload_type]=$(( ${total_insts[$workload_type]:-0} + $insts ))
            total_cycles[$workload_type]=$(( ${total_cycles[$workload_type]:-0} + $cycles ))
            break
        fi
    done < "$file"
done


# Define color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color


printf "${BLUE}%-20s\t%-12s\t%-12s\t%-12s${NC}\n" "workload type" "total insts" "total cycles" "effective IPC"

for workload_type in "${!total_insts[@]}"; do
    insts=${total_insts[$workload_type]}
    cycles=${total_cycles[$workload_type]}
    ipc=$(echo "scale=2; $insts / $cycles" | bc -l)
    printf "${RED}%-20s${NC}\t${GREEN}%-12s${NC}\t${GREEN}%-12s${NC}\t${GREEN}%-12.2f${NC}\n" "$workload_type" "$insts" "$cycles" "$ipc"
done
