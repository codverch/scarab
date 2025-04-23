#!/bin/bash

SCARAB_DIR="/users/DTRDNT/main/scarab/src"
TRACE_ROOT="/users/DTRDNT/main/simpoint_traces"
MAX_PARALLEL=8 
MY_DIR=$(pwd)
RECORD_DIR="$MY_DIR/records"

baseline_options="--uop_cache_enable 0"

mkdir -p "$RECORD_DIR"
cd "$SCARAB_DIR" || exit 1

run_scarab() {
    local trace_zip="$1"
    local bin_dir="$2"
    # Extract the workload name from the parent directory

    wname=$(basename $(realpath $(dirname $bin_dir/../../..))) #ASSUMPTION: the workload name is two dirs above the trace folder -- it can be three as in simpoint traces
    local workload_name="$wname"
    # Generate a record file name in the format: workload_number.record (e.g., clang_62.record)
    local record_file="$RECORD_DIR/${workload_name}_$(basename "$trace_zip" .zip).record"
    local vanilla_output_name="$RECORD_DIR/${workload_name}_$(basename "$trace_zip" .zip)_vanilla.txt"
    local fused_output_name="$RECORD_DIR/${workload_name}_$(basename "$trace_zip" .zip)_fused.txt"
    local parsed_output_name="$RECORD_DIR/${workload_name}_$(basename "$trace_zip" .zip).out"
    
    # Check if the record file already exists
    if [[ -f "$record_file" ]]; then
        echo "Skipping $(basename "$trace_zip") - Record already exists, this is unusual"
        return
    fi

    # Run Scarab with the provided options and record the output
    echo "Running Scarab on $wname $(basename "$trace_zip")"
    ./scarab $baseline_options --frontend memtrace --cbp_trace_r0="$trace_zip" --memtrace_modules_log="$bin_dir" \
            --record 1 --record_file "$record_file" > "$vanilla_output_name" 
    
    echo "python parsing $record_file to $parsed_output_name"
    pypy3 python_parser.py --record_file $record_file --output_file $parsed_output_name > /dev/null 2>&1

    rm $record_file

    echo "Running Scarab again on $wname $(basename "$trace_zip") with $parsed_output_name"
    ./scarab $baseline_options --frontend memtrace --cbp_trace_r0="$trace_zip" --memtrace_modules_log="$bin_dir" \
            --run 1 --run_file "$parsed_output_name" > "$fused_output_name" 

    rm $parsed_output_name
    

}

# Loop through all the zip files in the TRACE_ROOT directory
find "$TRACE_ROOT" -type f -name "*.zip" | while read -r trace_zip; do
    # Derive the corresponding bin directory for each trace
    bin_dir="$(dirname "$trace_zip")/../bin"

    if [[ "$trace_zip" == *"cpu_schedule.bin.zip"* ]]; then
        continue  # Skip this iteration and go to the next, actual trace file
    fi
    
    # Run Scarab for each trace
    { run_scarab "$trace_zip" "$bin_dir"; } & 
    
    # Limit the number of parallel jobs
    while (( $(jobs -rp | wc -l) >= MAX_PARALLEL )); do
        sleep 1  # Wait a bit before checking again
    done
done

# Wait for all background jobs to finish
wait

echo "All traces processed!"
