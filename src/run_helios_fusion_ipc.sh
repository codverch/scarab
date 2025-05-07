#!/bin/bash

# Define the base directories
CRONO_INIT_DIR="/mnt/fusion_tmpfs/dc1_crono_init"
CRONO_SOCLIVJ_DIR="/mnt/fusion_tmpfs/dc2_crono_soclivj"
SPEC_DIR="/mnt/fusion_tmpfs/spec_post_init"
OLD_DC_DIR="/mnt/fusion_tmpfs/old_dc_post_init"
RESULTS_BASE_DIR="/users/deepmish/ipc_ideal_fusion"
MAX_PARALLEL=3 # Maximum number of parallel jobs

# Create the category subdirectories
mkdir -p "$RESULTS_BASE_DIR/dc1_crono_init"
mkdir -p "$RESULTS_BASE_DIR/dc2_crono_soclivj"
mkdir -p "$RESULTS_BASE_DIR/spec_post_init"
mkdir -p "$RESULTS_BASE_DIR/old_dc_post_init"

# Function to run scarab and store results
run_scarab() {
    local trace_zip="$1"
    local bin_dir="$2"
    local result_file="$3"
    echo "Processing $trace_zip"
    ./scarab --frontend memtrace \
        --cbp_trace_r0="$trace_zip" \
        --memtrace_modules_log="$bin_dir" \
        --do_fusion 1 \
        --inst_limit 100000000 \
        --uop_cache_enable 0 > "$result_file" 2>&1
}

# Function to process directories with a single trace.zip file
process_single_trace_dir() {
    local base_dir=$1
    local result_category=$2
    
    for app_dir in "$base_dir"/*; do
        if [ -d "$app_dir" ]; then
            trace_dir="$app_dir/trace"
            bin_dir="$app_dir/bin"  # Bin directory is at the same level as trace
            app_name=$(basename "$app_dir")
            result_dir="$RESULTS_BASE_DIR/$result_category/$app_name"
            mkdir -p "$result_dir"
            
            # Check if the trace directory exists
            if [ -d "$trace_dir" ]; then
                # Check if trace.zip exists in the trace directory
                if [ -f "$trace_dir/trace.zip" ]; then
                    result_file="$result_dir/${app_name}_ipc_ideal_fusion.txt"
                    run_scarab "$trace_dir/trace.zip" "$bin_dir" "$result_file" &
                    
                    # Limit the number of parallel jobs
                    while (( $(jobs -rp | wc -l) >= MAX_PARALLEL )); do
                        sleep 1
                    done
                else
                    echo "No trace.zip found in $trace_dir"
                fi
            else
                echo "No trace directory found in $app_dir"
            fi
        fi
    done
}

# Process each category with the appropriate result directory
process_single_trace_dir "$CRONO_INIT_DIR" "dc1_crono_init"
process_single_trace_dir "$CRONO_SOCLIVJ_DIR" "dc2_crono_soclivj"
process_single_trace_dir "$SPEC_DIR" "spec_post_init"

# Process OLD_DC_DIR with multiple .zip files in each trace directory
for app_dir in "$OLD_DC_DIR"/*; do
    if [ -d "$app_dir" ]; then
        app_name=$(basename "$app_dir")
        
        # Skip processing apps that start with pt_
        if [[ "$app_name" == pt_* ]]; then
            echo "Skipping $app_name (starts with pt_)"
            continue
        fi
        
        trace_dir="$app_dir/traces_simp/trace"
        bin_dir="$app_dir/traces_simp/bin"  # Updated bin directory path for OLD_DC
        result_dir="$RESULTS_BASE_DIR/old_dc_post_init/$app_name"
        mkdir -p "$result_dir"
        
        # Check if the trace directory exists
        if [ -d "$trace_dir" ]; then
            # Iterate through each .zip file in the trace directory
            for zip_file in "$trace_dir"/*.zip; do
                if [ -f "$zip_file" ]; then
                    zip_name=$(basename "$zip_file" .zip)
                    result_file="$result_dir/${app_name}_${zip_name}_ipc_ideal_fusion.txt"
                    run_scarab "$zip_file" "$bin_dir" "$result_file" &
                    
                    # Limit the number of parallel jobs
                    while (( $(jobs -rp | wc -l) >= MAX_PARALLEL )); do
                        sleep 1
                    done
                else
                    echo "No .zip files found in $trace_dir"
                    break
                fi
            done
        else
            echo "No trace directory found in $app_dir"
        fi
    fi
done

# Wait for all background jobs to finish
wait
echo "All traces processed! Results stored in $RESULTS_BASE_DIR with the following structure:"
echo "  - $RESULTS_BASE_DIR/dc1_crono_init/    (for CRONO initial phase benchmarks)"
echo "  - $RESULTS_BASE_DIR/dc2_crono_soclivj/ (for CRONO social network benchmarks)"
echo "  - $RESULTS_BASE_DIR/spec_post_init/    (for SPEC benchmarks)"
echo "  - $RESULTS_BASE_DIR/old_dc_post_init/  (for DC benchmarks, excluding pt_* apps)"