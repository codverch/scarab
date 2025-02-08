#!/bin/bash
directory="/users/deepmish/simpoint_traces"
pathToScarabDir="/users/deepmish/scarab/src"
baseline_options=" "
INST_LIMIT=100000000
WARMUP_INSTS=45000000
max_simul_proc=23
currdir=$(pwd)
prefix="vanilla"

CONFIG_SIZES=(
"64 36"
"128 72"
"256 144"
"512 288"
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
        LQ=$(echo $SIZE | awk '{print $1}')
        SQ=$(echo $SIZE | awk '{print $2}')
        
        CONFIG_DIR="$results_dir/$workload/LQ${LQ}_SQ${SQ}"
        mkdir -p "$CONFIG_DIR"
        
        trace_dir="$directory/$workload"
        for trace_file in "$trace_dir"/traces_simp/trace/*.zip; do
            trace_number=$(basename "$trace_file" .zip)
            check_running_procs
            
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
        done
    done
done

for pid in ${pids[@]}; do
    wait $pid
done
