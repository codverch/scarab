#!/bin/bash

# Directory containing the traces
TRACES_DIR="/users/deepmish/traces"

# Output directory for results
OUTPUT_DIR="ideal_fusion_with_store_dependence_results"
mkdir -p $OUTPUT_DIR

# Loop through each application directory
for app in $(ls $TRACES_DIR); do
    echo "Running Scarab for $app..."
    
    # Run Scarab with the appropriate parameters
    ./scarab --frontend memtrace \
             --fetch_off_path_ops 0 \
             --cbp_trace_r0=${TRACES_DIR}/${app}/trace/trace.zip \
             --memtrace_modules_log=${TRACES_DIR}/${app}/bin \
             --inst_limit 100000000 \
             > ${OUTPUT_DIR}/ideal_fusion_${app}_ipc.txt
             
    echo "Completed $app. Results saved to ${OUTPUT_DIR}/ideal_fusion_${app}_ipc.txt"
done

echo "All applications processed successfully!"