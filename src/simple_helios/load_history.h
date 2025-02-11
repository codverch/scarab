#ifndef LOAD_HISTORY_H
#define LOAD_HISTORY_H

#include <stdint.h>
#include <stdbool.h>

#define LOAD_HISTORY_SIZE 6

typedef struct LoadEntry {
    uint64_t prog_ctr;
    uint64_t effective_addr;
    uint64_t micro_op_commit_num;
    bool is_mem_load;
    bool is_fused;
} LoadEntry;

typedef struct LoadHistory{
    LoadEntry entries[LOAD_HISTORY_SIZE];
    uint8_t head;
    uint8_t tail;
    uint8_t size;  // Tracks the number of valid entries in the circular buffer
} LoadHistory;

// Inserts a new load micro-op into the load history.
void insert_into_load_history(uint64_t prog_ctr, uint64_t effective_addr, 
                              bool is_mem_load, bool is_fused, 
                              uint64_t micro_op_commit_num, LoadHistory* load_history);

// Checks if a load with the same cacheline access as the committed micro-op exists in history.
bool is_load_cacheline_in_history(uint64_t effective_addr, const LoadHistory* load_history);

#endif // LOAD_HISTORY_H
