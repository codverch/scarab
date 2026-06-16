#include <stdio.h>
#include <stdlib.h>

#include "ifuse_set_stats.h"

void ifuse_set_stats_init(IfuseSetStats* stats, unsigned int num_sets,
                          unsigned int num_ways,
                          Stat_Enum stat_set_peak_live,
                          Stat_Enum stat_hot_sets,
                          Stat_Enum stat_conflict_sets,
                          const char* error_prefix) {
    stats->num_sets              = num_sets;
    stats->num_ways              = num_ways;
    stats->stat_set_peak_live    = stat_set_peak_live;
    stats->stat_hot_sets         = stat_hot_sets;
    stats->stat_conflict_sets    = stat_conflict_sets;
    stats->set_peak_live         = (unsigned int*)calloc(num_sets,
                                                         sizeof(unsigned int));
    stats->set_evictions         = (unsigned int*)calloc(num_sets,
                                                         sizeof(unsigned int));
    stats->set_ever_full         = (bool*)calloc(num_sets, sizeof(bool));
    stats->set_had_eviction    = (bool*)calloc(num_sets, sizeof(bool));

    if (!stats->set_peak_live || !stats->set_evictions ||
        !stats->set_ever_full || !stats->set_had_eviction) {
        fprintf(stderr, "%s: calloc failed for %u set stat vectors\n",
                error_prefix, num_sets);
        exit(1);
    }

    ifuse_set_stats_reset(stats);
    stats->initialized = true;
}

void ifuse_set_stats_reset(IfuseSetStats* stats) {
    stats->global_set_peak_live  = 0U;
    stats->hot_set_count         = 0U;
    stats->conflict_set_count    = 0U;
}

void ifuse_set_stats_note_set_live(IfuseSetStats* stats, unsigned int proc_id,
                                   unsigned int set_idx,
                                   unsigned int set_live) {
    if (!stats->initialized || !stats->set_peak_live ||
        set_idx >= stats->num_sets) {
        return;
    }

    if (set_live > stats->set_peak_live[set_idx]) {
        stats->set_peak_live[set_idx] = set_live;
    }

    if (set_live > stats->global_set_peak_live) {
        INC_STAT_EVENT(proc_id, stats->stat_set_peak_live,
                       set_live - stats->global_set_peak_live);
        stats->global_set_peak_live = set_live;
    }

    if (set_live >= stats->num_ways && !stats->set_ever_full[set_idx]) {
        stats->set_ever_full[set_idx] = true;
        stats->hot_set_count++;
        STAT_EVENT(proc_id, stats->stat_hot_sets);
    }
}

void ifuse_set_stats_note_eviction(IfuseSetStats* stats, unsigned int proc_id,
                                   unsigned int set_idx) {
    if (!stats->initialized || !stats->set_evictions ||
        set_idx >= stats->num_sets) {
        return;
    }

    stats->set_evictions[set_idx]++;

    if (!stats->set_had_eviction[set_idx]) {
        stats->set_had_eviction[set_idx] = true;
        stats->conflict_set_count++;
        STAT_EVENT(proc_id, stats->stat_conflict_sets);
    }
}

void ifuse_set_stats_dump(const IfuseSetStats* stats, const char* output_path,
                          const char* table_label, unsigned int num_entries,
                          uint64_t global_peak_live,
                          const char* error_prefix) {
    if (!stats->initialized || !stats->set_peak_live ||
        stats->num_sets == 0U) {
        return;
    }

    FILE* fp = fopen(output_path, "w");
    if (!fp) {
        fprintf(stderr, "%s: could not write %s\n", error_prefix,
                output_path);
        return;
    }

    unsigned int sets_with_evictions = 0U;
    unsigned int top_peak            = 0U;
    unsigned int top_evictions       = 0U;
    unsigned int top_evict_set       = 0U;

    for (unsigned int set_idx = 0; set_idx < stats->num_sets; set_idx++) {
        if (stats->set_evictions[set_idx] > 0U) {
            sets_with_evictions++;
        }
        if (stats->set_peak_live[set_idx] > top_peak) {
            top_peak = stats->set_peak_live[set_idx];
        }
        if (stats->set_evictions[set_idx] > top_evictions) {
            top_evictions = stats->set_evictions[set_idx];
            top_evict_set = set_idx;
        }
    }

    fprintf(fp, "# IFuse %s per-set stats (realistic mode)\n", table_label);
    fprintf(fp, "# sets=%u ways=%u entries=%u global_peak_live=%llu\n",
            stats->num_sets, stats->num_ways, num_entries,
            (unsigned long long)global_peak_live);
    fprintf(fp, "# set_peak_live=%u hot_sets=%u conflict_sets=%u\n",
            stats->global_set_peak_live, stats->hot_set_count,
            stats->conflict_set_count);
    fprintf(fp, "# sets_with_evictions=%u top_evict_set=%u top_evictions=%u\n",
            sets_with_evictions, top_evict_set, top_evictions);
    fprintf(fp, "# columns: set_idx peak_live evictions ever_full\n");

    for (unsigned int set_idx = 0; set_idx < stats->num_sets; set_idx++) {
        if (stats->set_peak_live[set_idx] == 0U &&
            stats->set_evictions[set_idx] == 0U) {
            continue;
        }

        fprintf(fp, "%u %u %u %u\n", set_idx,
                stats->set_peak_live[set_idx],
                stats->set_evictions[set_idx],
                stats->set_ever_full[set_idx] ? 1U : 0U);
    }

    fclose(fp);
}
