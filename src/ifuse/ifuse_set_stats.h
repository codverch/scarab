#ifndef IFUSE_SET_STATS_H
#define IFUSE_SET_STATS_H

#include <stdbool.h>
#include <stdint.h>

#include "../statistics.h"

/**
 * Per-set occupancy / eviction histograms for realistic set-associative IFuse
 * tables (training table, FCT, etc.).
 */
typedef struct IfuseSetStats {
    unsigned int num_sets;
    unsigned int num_ways;
    unsigned int* set_peak_live;
    unsigned int* set_evictions;
    bool* set_ever_full;
    bool* set_had_eviction;
    unsigned int global_set_peak_live;
    unsigned int hot_set_count;
    unsigned int conflict_set_count;
    Stat_Enum stat_set_peak_live;
    Stat_Enum stat_hot_sets;
    Stat_Enum stat_conflict_sets;
    bool initialized;
} IfuseSetStats;

void ifuse_set_stats_init(IfuseSetStats* stats, unsigned int num_sets,
                          unsigned int num_ways,
                          Stat_Enum stat_set_peak_live,
                          Stat_Enum stat_hot_sets,
                          Stat_Enum stat_conflict_sets,
                          const char* error_prefix);

void ifuse_set_stats_reset(IfuseSetStats* stats);

void ifuse_set_stats_note_set_live(IfuseSetStats* stats, unsigned int proc_id,
                                   unsigned int set_idx,
                                   unsigned int set_live);

void ifuse_set_stats_note_eviction(IfuseSetStats* stats, unsigned int proc_id,
                                   unsigned int set_idx);

void ifuse_set_stats_dump(const IfuseSetStats* stats,
                          const char* output_path, const char* table_label,
                          unsigned int num_entries,
                          uint64_t global_peak_live,
                          const char* error_prefix);

#endif /* IFUSE_SET_STATS_H */
