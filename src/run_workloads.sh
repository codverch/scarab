#!/bin/bash
directory="/users/deepmish/simpoint_traces"
pathToScarabDir="/users/deepmish/scarab/src"
baseline_options=" "
INST_LIMIT=100000000
WARMUP_INSTS=45000000
max_simul_proc=23
currdir=$(pwd)
prefix="rob_sensitivity"

CONFIG_SIZES=(
"192"
"96"
"384"
"768"
)

# Collect workloads
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

# Create main workload directories first
for workload in ${workloads[@]}; do
    mkdir -p "$results_dir/$workload"
done

for workload in ${workloads[@]}; do
    for SIZE in "${CONFIG_SIZES[@]}"; do
        ROB=$(echo $SIZE | awk '{print $1}')
        
        CONFIG_DIR="$results_dir/$workload/ROB${ROB}"
        mkdir -p "$CONFIG_DIR"
        
        trace_dir="$directory/$workload"
        for trace_file in "$trace_dir"/traces_simp/trace/*.zip; do
            trace_number=$(basename "$trace_file" .zip)
            check_running_procs
            
            if [[ $workload == pt_* ]]; then
                $pathToScarabDir/scarab --frontend pt --fetch_off_path_ops 0 $baseline_options \
                    --cbp_trace_r0="$directory/$workload/trace.gz" --full_warmup $WARMUP_INSTS --inst_limit $INST_LIMIT \
                    --node_table_size "$ROB" \
                    > "$CONFIG_DIR/${workload}_${trace_number}.txt" &
            else
                $pathToScarabDir/scarab --frontend memtrace --fetch_off_path_ops 0 $baseline_options \
                    --cbp_trace_r0="$trace_file" --memtrace_modules_log="$trace_dir/traces_simp/bin" \
                    --full_warmup $WARMUP_INSTS --inst_limit $INST_LIMIT \
                    --node_table_size "$ROB" \
                    > "$CONFIG_DIR/${workload}_${trace_number}.txt" &
            fi
            pids+=($!)
        done
    done
done

for pid in ${pids[@]}; do
    wait $pid
done

cd "$results_dir"
echo "Processing results..."

# Process results for each workload
for workload_dir in */; do
    workload=${workload_dir%/}
    declare -A workload_total_insts
    declare -A workload_total_cycles
    
    for CONFIG_DIR in "$workload_dir"LQ*; do
        config_name=$(basename "$CONFIG_DIR")
        while IFS= read -r line < <(grep "insts:" "$CONFIG_DIR"/*.txt); do
            insts=$(echo $line | grep -oP 'insts:\K[0-9]+')
            cycles=$(echo $line | grep -oP 'cycles:\K[0-9]+')
            workload_total_insts[$config_name]=$(( ${workload_total_insts[$config_name]:-0} + $insts ))
            workload_total_cycles[$config_name]=$(( ${workload_total_cycles[$config_name]:-0} + $cycles ))
        done
        
        # Calculate and write IPC for this configuration
        insts=${workload_total_insts[$config_name]}
        cycles=${workload_total_cycles[$config_name]}
        ipc=$(echo "scale=2; $insts / $cycles" | bc -l)
        echo "IPC: $ipc" > "$CONFIG_DIR/ipc.txt"
    done
    
    # Create summary file for each workload
    echo "Summary for $workload:" > "$workload_dir/summary.txt"
    for config_name in "${!workload_total_insts[@]}"; do
        insts=${workload_total_insts[$config_name]}
        cycles=${workload_total_cycles[$config_name]}
        ipc=$(echo "scale=2; $insts / $cycles" | bc -l)
        echo "$config_name - IPC: $ipc" >> "$workload_dir/summary.txt"
    done
done

echo "All results processed. Check results in $results_dir"
