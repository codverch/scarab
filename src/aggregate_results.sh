#!/bin/bash

results_dir="lsq_rob_sensitivity_results"  # Base directory containing workload folders
output_file="agg_results.txt"

# Clear output file and write header
echo -e "workload\tconfig\tTotal Insts\tTotal Cycles\tIPC" > "$output_file"

# Iterate through each workload directory
for workload in "$results_dir"/*; do
    [[ -d "$workload" ]] || continue  # Skip if not a directory
    workload_name=$(basename "$workload")

    # Iterate through each LQ_SQ_ROB configuration
    for config in "$workload"/*; do
        [[ -d "$config" ]] || continue  # Skip if not a directory
        config_name=$(basename "$config")

        total_insts=0
        total_cycles=0

        # Process each .txt file inside the LQ_SQ_ROB folder
        for file in "$config"/*.txt; do
            [[ -f "$file" ]] || continue  # Skip if not a file
            
            insts=$(grep -oP 'insts:\K[0-9]+' "$file")
            cycles=$(grep -oP 'cycles:\K[0-9]+' "$file")

            if [[ -n "$insts" && -n "$cycles" ]]; then
                total_insts=$((total_insts + insts))
                total_cycles=$((total_cycles + cycles))
            fi
        done

        # Calculate IPC
        if [[ $total_cycles -ne 0 ]]; then
            ipc=$(echo "scale=2; $total_insts / $total_cycles" | bc -l)
        else
            ipc="N/A"
        fi

        # Print and write results
        printf "%s\t%s\t%s\t%s\t%s\n" "$workload_name" "$config_name" "$total_insts" "$total_cycles" "$ipc" | tee -a "$output_file"
    done
done

echo "Results saved to $output_file"
