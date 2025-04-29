#!/bin/bash
# Directory containing the traces
TRACES_DIR="/users/deepmish/completed_traces"
# Output directory for results
OUTPUT_DIR="ideal_fusion"
mkdir -p $OUTPUT_DIR

# Loop through each application directory
for app in $(ls $TRACES_DIR); do
  echo "Running Scarab for $app..."
  
  # Create app-specific directory to store all results
  APP_OUTPUT_DIR="${OUTPUT_DIR}/${app}"
  mkdir -p $APP_OUTPUT_DIR
  
  # Run Scarab with the appropriate parameters
  # We capture both stdout and stderr to the terminal output log
  # while also saving the regular output to the ipc file
  (./scarab --frontend memtrace \
    --fetch_off_path_ops 0 \
    --cbp_trace_r0=${TRACES_DIR}/${app}/trace/trace.zip \
    --memtrace_modules_log=${TRACES_DIR}/${app}/bin \
    --inst_limit 100000000 \
    | tee ${APP_OUTPUT_DIR}/${app}_terminal_output.log) \
    > ${OUTPUT_DIR}/ideal_fusion_${app}_ipc.txt 2> ${APP_OUTPUT_DIR}/${app}_error.log
  
  echo "Completed $app."
  echo "Results saved to ${OUTPUT_DIR}/ideal_fusion_${app}_ipc.txt"
  echo "Terminal output saved to ${APP_OUTPUT_DIR}/${app}_terminal_output.log"
  echo "Error output saved to ${APP_OUTPUT_DIR}/${app}_error.log"
  
  # Copy all generated CSV and OUT files to the app-specific directory
  echo "Copying result files for $app..."
  cp core.stat.0.csv ${APP_OUTPUT_DIR}/${app}_core.stat.0.csv
  cp core.stat.0.out ${APP_OUTPUT_DIR}/${app}_core.stat.0.out
  cp fetch.stat.0.csv ${APP_OUTPUT_DIR}/${app}_fetch.stat.0.csv
  cp fetch.stat.0.out ${APP_OUTPUT_DIR}/${app}_fetch.stat.0.out
  cp inst.stat.0.csv ${APP_OUTPUT_DIR}/${app}_inst.stat.0.csv
  cp inst.stat.0.out ${APP_OUTPUT_DIR}/${app}_inst.stat.0.out
  cp l2l1pref.stat.0.csv ${APP_OUTPUT_DIR}/${app}_l2l1pref.stat.0.csv
  cp l2l1pref.stat.0.out ${APP_OUTPUT_DIR}/${app}_l2l1pref.stat.0.out
  cp memory.stat.0.csv ${APP_OUTPUT_DIR}/${app}_memory.stat.0.csv
  cp memory.stat.0.out ${APP_OUTPUT_DIR}/${app}_memory.stat.0.out
  cp ramulator.stat.out ${APP_OUTPUT_DIR}/${app}_ramulator.stat.out
  cp pref.stat.0.out ${APP_OUTPUT_DIR}/${app}_pref.stat.0.out
  cp power.stat.0.out ${APP_OUTPUT_DIR}/${app}_power.stat.0.out
  
  echo "Result files for $app copied successfully!"
done

echo "All applications processed successfully!"

# Optional: Create a summary file listing all applications and their results
echo "Creating summary file..."
echo "Application,IPC" > ${OUTPUT_DIR}/summary.csv
for app in $(ls $TRACES_DIR); do
  # Extract IPC from the output file (assuming it can be found with grep)
  IPC=$(grep "IPC" ${OUTPUT_DIR}/ideal_fusion_${app}_ipc.txt | awk '{print $NF}')
  echo "${app},${IPC}" >> ${OUTPUT_DIR}/summary.csv
done

echo "Summary file created at ${OU
