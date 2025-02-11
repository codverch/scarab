#include "load_history.h"

void insert_into_load_history(uint64_t pc, uint64_t ea, bool is_load, uint64_t commit_num, LoadHistory* load_history) {
    // Check if the entry already exists in the load history
    for (int i = 0; i < LOAD_HISTORY_SIZE; i++) {
        if (load_history->entries[i].prog_ctr == pc && load_history->entries[i].effective_addr == ea) {
            // If the entry already exists that's probably the head nuclues
            // check whether it has been fused or not
            if (!load_history->entries[i].is_fused) {
                // If it has not been fused then this is the second time we are seeing this entry
                // so we can mark it as fused: the current commit instr is the tail nucleus and we should 
                // look for this in the predictor
                load_history->entries[i].is_fused = true;
                // call function to train the predictor with the fused entry
            }
            return; 
        }
    }

    // Create a new LoadEntry
    LoadEntry entry;
    entry.prog_ctr = pc;
    entry.effective_addr = ea;
    entry.micro_op_commit_num = commit_num; // Store the commit number directly
    entry.is_mem_load = is_load;
    entry.is_fused = false; // New entry is not fused

    // Insert into the circular buffer
    load_history->entries[load_history->head] = entry;

    // Update head and tail pointers for circular buffer behavior
    load_history->head = (load_history->head + 1) % LOAD_HISTORY_SIZE;
    if (load_history->size < LOAD_HISTORY_SIZE) {
        load_history->size++;
    } else {
        // Overwrite the oldest entry
        load_history->tail = (load_history->tail + 1) % LOAD_HISTORY_SIZE;
    }
}

bool is_load_cacheline_in_history(uint64_t ea, const LoadHistory* load_history) {
    for (int i = 0; i < LOAD_HISTORY_SIZE; i++) {
        if (load_history->entries[i].effective_addr == ea) {
            return true;
        }
    }
    return false;
}

