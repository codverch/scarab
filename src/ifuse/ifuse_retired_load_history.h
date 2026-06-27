// STD headers
#include <stdbool.h>
#include <stdint.h>

// Custom headers
#include "globals/global_types.h"

#ifndef IFUSE_RETIRED_LOAD_HISTORY_H
#define IFUSE_RETIRED_LOAD_HISTORY_H

/**
 * Retired Load History
 * ====================
 * Tracks recently retired loads so the runtime trainer can identify LD1/LD2
 * pairs in the same cache block.
 *
 * The table is a circular buffer sized for the default 512-load fusion
 * distance. Entries are also indexed by cache block, so matching a retiring LD2
 * only scans older LD1 candidates that touched the same block.
 */

typedef struct RetiredLoadHistoryEntry {
    Addr         load_pc_addr;
    Addr         load_effective_addr;
    unsigned int load_memory_access_size;
    unsigned int load_micro_op_num;
    Flag         load_mem_critical;
} RetiredLoadHistoryEntry;

/**
 * Clears all retired-load history entries.
 */
void retired_load_history_clear(void);

/** 
 * Inserts a retired load into the history table.
 *
 * @param load_pc_addr The PC address of the retired load.
 * @param load_effective_addr The effective memory address accessed by the load.
 * @param load_memory_access_size The memory access size of the retired load.
 * @param load_micro_op_num The dynamic micro-op number of the retired load.
 * @param load_num The dynamic retired on-path load number.
 */
void retired_load_history_insert(Addr load_pc_addr,
                                 Addr load_effective_addr,
                                 unsigned int load_memory_access_size,
                                 unsigned int load_micro_op_num,
                                 uint64_t load_num,
                                 Flag load_mem_critical);

/**
 * Finds and removes the most recent older load that can pair with current LD2.
 *
 * @param current_load_effective_addr The effective memory address of LD2.
 * @param current_load_micro_op_num The dynamic micro-op number of LD2.
 * @param current_load_num The dynamic retired on-path load number of LD2.
 * @param matched_load Filled with the matching older load on success.
 * @return TRUE if a matching older load was found, FALSE otherwise.
 */
bool retired_load_history_find_and_remove_match(
    Addr current_load_effective_addr,
    unsigned int current_load_micro_op_num,
    uint64_t current_load_num,
    RetiredLoadHistoryEntry* matched_load);

/**
 * Invalidates earlier loads that touched a cache block overwritten by a store.
 *
 * @param store_cacheblock_addr The cache block address written by the store.
 */
void retired_load_history_invalidate_cacheblock(Addr store_cacheblock_addr);

#endif /* IFUSE_RETIRED_LOAD_HISTORY_H */
