// STD headers
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Custom headers
#include "ifuse_fct.h"
#include "ifuse_ideal_limits.h"
#include "ifuse_training_table.h"
#include "../general.param.h"
#include "../statistics.h"
#include "ifuse.param.h"

/**
 * IFuse Training Table
 * ====================
 * The training table counts retired fusible PC-pairs before they are admitted
 * into the Fusion Candidate Table. A pair reaches this table only after the
 * retire-load history has proven that LD1 and LD2 accessed the same cache block
 * and that no intervening store invalidated the pair.
 *
 * This starts the realistic-table path while keeping capacity ideal for now.
 * Storage is a large simulator hash table keyed by the exact runtime pattern:
 * LD1 PC, LD2 PC, offset delta, direction, and LD2 memory access size. Once a
 * pattern reaches IFUSE_TRAINING_INSERT_THRESHOLD observations, it is
 * promoted into the FCT with high confidence.
 */

#define TRAINING_TABLE_NUM_ENTRIES IFUSE_IDEAL_TRAINING_TABLE_MAX_ENTRIES

typedef struct TrainingTableEntry {
    Addr         ld1_pc_addr;
    Addr         ld2_pc_addr;
    unsigned int offset_delta;
    unsigned int ld2_memory_access_size;
    unsigned int observation_count;
    bool         direction;
    bool         valid;
} TrainingTableEntry;

// Module state
static TrainingTableEntry* training_table_entries = NULL;
static bool                training_table_initialized = false;
static uint64_t            training_table_live_count = 0;
static uint64_t            training_table_live_peak = 0;

static void training_table_note_live_insert(void) {
    training_table_live_count++;
    if (training_table_live_count > training_table_live_peak) {
        INC_STAT_EVENT(0, TRAINING_TABLE_PEAK_LIVE,
                       training_table_live_count - training_table_live_peak);
        training_table_live_peak = training_table_live_count;
    }
}

// Hash-table helpers

/**
 * Returns the software hash for one exact fusible pair pattern.
 *
 * @param ld1_pc_addr The PC address of LD1.
 * @param ld2_pc_addr The PC address of LD2.
 * @param offset_delta The absolute cache-line offset delta between LD1 and LD2.
 * @param direction TRUE when LD2's offset is greater than or equal to LD1's.
 * @param ld2_memory_access_size The memory access size of LD2.
 * @return The mixed hash value for the training-table key.
 */
static uint64_t training_table_hash(Addr ld1_pc_addr, Addr ld2_pc_addr,
                                    unsigned int offset_delta, bool direction,
                                    unsigned int ld2_memory_access_size) {
    uint64_t hash_value = (uint64_t)ld1_pc_addr;
    hash_value ^= ((uint64_t)ld2_pc_addr << 1);
    hash_value ^= ((uint64_t)offset_delta << 17);
    hash_value ^= ((uint64_t)ld2_memory_access_size << 25);
    hash_value ^= direction ? 0xA5A5A5A5A5A5A5A5ULL : 0;
    hash_value ^= hash_value >> 33;
    hash_value *= 0xff51afd7ed558ccdULL;
    hash_value ^= hash_value >> 33;
    return hash_value;
}

/**
 * Returns the counter entry for one fusible pair pattern.
 *
 * @param ld1_pc_addr The PC address of LD1.
 * @param ld2_pc_addr The PC address of LD2.
 * @param offset_delta The absolute cache-line offset delta between LD1 and LD2.
 * @param direction TRUE when LD2's offset is greater than or equal to LD1's.
 * @param ld2_memory_access_size The memory access size of LD2.
 * @return The existing or newly allocated training entry, or NULL on overflow.
 */
static TrainingTableEntry* training_table_find_entry(
    Addr ld1_pc_addr,
    Addr ld2_pc_addr,
    unsigned int offset_delta,
    bool direction,
    unsigned int ld2_memory_access_size) {
    uint64_t hash_value = training_table_hash(
        ld1_pc_addr, ld2_pc_addr, offset_delta, direction,
        ld2_memory_access_size);
    unsigned int start_idx =
        (unsigned int)(hash_value & (TRAINING_TABLE_NUM_ENTRIES - 1U));

    /*
     * This is an ideal-capacity baseline. Probe the complete backing table so
     * a collision chain cannot discard a valid observation while free rows
     * remain elsewhere.
     */
    for (unsigned int probe = 0; probe < TRAINING_TABLE_NUM_ENTRIES; probe++) {
        unsigned int idx =
            (start_idx + probe) & (TRAINING_TABLE_NUM_ENTRIES - 1U);
        TrainingTableEntry* entry = &training_table_entries[idx];

        if (!entry->valid) {
            entry->ld1_pc_addr              = ld1_pc_addr;
            entry->ld2_pc_addr              = ld2_pc_addr;
            entry->offset_delta             = offset_delta;
            entry->direction                = direction;
            entry->ld2_memory_access_size   = ld2_memory_access_size;
            entry->observation_count        = 0;
            entry->valid                    = true;
            STAT_EVENT(0, TRAINING_TABLE_INSERTS);
            training_table_note_live_insert();
            return entry;
        }

        if (entry->ld1_pc_addr == ld1_pc_addr &&
            entry->ld2_pc_addr == ld2_pc_addr &&
            entry->offset_delta == offset_delta &&
            entry->direction == direction &&
            entry->ld2_memory_access_size == ld2_memory_access_size) {
            return entry;
        }
    }

    STAT_EVENT(0, TRAINING_TABLE_BACKING_ALLOC_FAILURES);
    return NULL;
}

/**
 * Returns the LD2 cache-line offset prediction for a retired pair.
 *
 * @param ld1_effective_addr The effective memory address accessed by LD1.
 * @param ld2_effective_addr The effective memory address accessed by LD2.
 * @param offset_delta Filled with the absolute cache-line offset delta.
 * @param direction Filled with TRUE when LD2's offset is greater than LD1's.
 */
static void training_table_get_offset_prediction(Addr ld1_effective_addr,
                                                 Addr ld2_effective_addr,
                                                 unsigned int* offset_delta,
                                                 bool* direction) {
    unsigned int ld1_offset = (unsigned int)(ld1_effective_addr & 63U);
    unsigned int ld2_offset = (unsigned int)(ld2_effective_addr & 63U);

    if (ld2_offset >= ld1_offset) {
        *offset_delta = ld2_offset - ld1_offset;
        *direction    = true;
    } else {
        *offset_delta = ld1_offset - ld2_offset;
        *direction    = false;
    }
}

// Public API

void training_table_init(void) {
    if (training_table_initialized) {
        return;
    }

    training_table_entries =
        (TrainingTableEntry*)calloc(TRAINING_TABLE_NUM_ENTRIES,
                                    sizeof(TrainingTableEntry));
    if (!training_table_entries) {
        fprintf(stderr, "TRAINING_TABLE: calloc failed for %u entries\n",
                TRAINING_TABLE_NUM_ENTRIES);
        exit(1);
    }
    training_table_initialized = true;
}

/**
 * Records one fusible pair observation and optionally promotes trained rows.
 *
 * @param ld1_pc_addr The PC address of LD1.
 * @param ld2_pc_addr The PC address of LD2.
 * @param ld1_effective_addr The effective memory address accessed by LD1.
 * @param ld2_effective_addr The effective memory address accessed by LD2.
 * @param ld1_memory_access_size The memory access size of LD1.
 * @param ld2_memory_access_size The memory access size of LD2.
 * @param ld1_micro_op_num The dynamic micro-op number of LD1.
 * @param ld2_micro_op_num The dynamic micro-op number of LD2.
 * @param proc_id The core id used for statistics.
 * @param promote_existing_row TRUE when an already-trained row can retry promotion.
 */
static void training_table_observe_pair(
    Addr ld1_pc_addr,
    Addr ld2_pc_addr,
    Addr ld1_effective_addr,
    Addr ld2_effective_addr,
    unsigned int ld1_memory_access_size,
    unsigned int ld2_memory_access_size,
    unsigned int ld1_micro_op_num,
    unsigned int ld2_micro_op_num,
    unsigned int proc_id,
    bool promote_existing_row) {
    (void)ld1_memory_access_size;

    if (!IFUSE_TRAINING_ENABLED) {
        return;
    }
    if (!training_table_initialized) {
        training_table_init();
    }
    if (ld1_pc_addr == 0 || ld2_pc_addr == 0 ||
        ld2_memory_access_size == 0) {
        return;
    }

    unsigned int offset_delta;
    bool         direction;
    training_table_get_offset_prediction(ld1_effective_addr, ld2_effective_addr,
                                         &offset_delta, &direction);
    if (offset_delta > 63U) {
        return;
    }

    TrainingTableEntry* entry = training_table_find_entry(
        ld1_pc_addr, ld2_pc_addr, offset_delta, direction,
        ld2_memory_access_size);
    if (!entry) {
        return;
    }

    if (entry->observation_count < 0xFFFFFFFFU) {
        entry->observation_count++;
    }
    STAT_EVENT(proc_id, TRAINING_TABLE_OBSERVATIONS);

    unsigned int insert_threshold = IFUSE_TRAINING_INSERT_THRESHOLD;
    if (insert_threshold == 0U) {
        insert_threshold = 1U;
    }

    if (entry->observation_count < insert_threshold) {
        return;
    }
    if (entry->observation_count > insert_threshold && !promote_existing_row) {
        fct_reinforce_ld2_candidate_for_ld1(ld1_pc_addr, ld2_pc_addr,
                                            offset_delta, direction,
                                            ld2_memory_access_size);
        return;
    }

    if (fct_promote_ld2_candidate_for_ld1(ld1_pc_addr, ld2_pc_addr,
                                          ld1_effective_addr, ld2_effective_addr,
                                          offset_delta, direction,
                                          ld2_memory_access_size,
                                          ld1_micro_op_num, ld2_micro_op_num,
                                          proc_id)) {
        STAT_EVENT(proc_id, TRAINING_TABLE_PROMOTIONS);
    }
}

void training_table_observe_fusible_pair(
    Addr ld1_pc_addr,
    Addr ld2_pc_addr,
    Addr ld1_effective_addr,
    Addr ld2_effective_addr,
    unsigned int ld1_memory_access_size,
    unsigned int ld2_memory_access_size,
    unsigned int ld1_micro_op_num,
    unsigned int ld2_micro_op_num,
    unsigned int proc_id) {
    training_table_observe_pair(ld1_pc_addr, ld2_pc_addr,
                                ld1_effective_addr, ld2_effective_addr,
                                ld1_memory_access_size,
                                ld2_memory_access_size,
                                ld1_micro_op_num, ld2_micro_op_num,
                                proc_id, false);
}

void training_table_retrain_fusible_pair(
    Addr ld1_pc_addr,
    Addr ld2_pc_addr,
    Addr ld1_effective_addr,
    Addr ld2_effective_addr,
    unsigned int ld1_memory_access_size,
    unsigned int ld2_memory_access_size,
    unsigned int ld1_micro_op_num,
    unsigned int ld2_micro_op_num,
    unsigned int proc_id) {
    training_table_observe_pair(ld1_pc_addr, ld2_pc_addr,
                                ld1_effective_addr, ld2_effective_addr,
                                ld1_memory_access_size,
                                ld2_memory_access_size,
                                ld1_micro_op_num, ld2_micro_op_num,
                                proc_id, true);
}
