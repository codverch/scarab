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
 * Ideal mode uses a large open-addressed table. Realistic mode uses a
 * set-associative table: the low bits of the folded LD1 PC select the set,
 * and a stored tag (remaining LD1 bits, folded LD2 PC, offset, direction,
 * and LD2 size) is compared across the ways. Each set keeps a 7-bit tree
 * PLRU replacement policy (8-way sets).
 */

#define TRAINING_TABLE_PC_BITS  48U
#define TRAINING_TABLE_PC_MASK  0xFFFFFFFFFFFFULL

typedef struct TrainingTableEntry {
    uint64_t     ld1_tag;
    uint64_t     ld2_tag;
    unsigned int offset_delta;
    unsigned int ld2_memory_access_size;
    unsigned int observation_count;
    bool         direction;
    bool         valid;
} TrainingTableEntry;

// Module state
static TrainingTableEntry* training_table_entries = NULL;
static uint8_t*            training_table_plru    = NULL;
static bool                training_table_initialized = false;
static unsigned int        training_table_num_entries = 0;
static unsigned int        training_table_num_sets    = 0;
static unsigned int        training_table_num_ways    = 0;
static unsigned int        training_table_set_index_bits = 0;
static uint64_t            training_table_live_count  = 0;
static uint64_t            training_table_live_peak   = 0;

static void training_table_note_live_insert(void) {
    training_table_live_count++;
    if (training_table_live_count > training_table_live_peak) {
        INC_STAT_EVENT(0, TRAINING_TABLE_PEAK_LIVE,
                       training_table_live_count - training_table_live_peak);
        training_table_live_peak = training_table_live_count;
    }
}

static void training_table_note_live_remove(void) {
    if (training_table_live_count > 0) {
        training_table_live_count--;
    }
}

static bool training_table_keys_match_ideal(
    const TrainingTableEntry* entry,
    Addr ld1_pc_addr,
    Addr ld2_pc_addr,
    unsigned int offset_delta,
    bool direction,
    unsigned int ld2_memory_access_size) {
    return entry->ld1_tag == (uint64_t)ld1_pc_addr &&
           entry->ld2_tag == (uint64_t)ld2_pc_addr &&
           entry->offset_delta == offset_delta &&
           entry->direction == direction &&
           entry->ld2_memory_access_size == ld2_memory_access_size;
}

static uint64_t training_table_fold_pc(Addr pc_addr) {
    return (uint64_t)pc_addr & TRAINING_TABLE_PC_MASK;
}

static unsigned int training_table_log2_u32(unsigned int value) {
    unsigned int bits = 0U;

    while (value > 1U) {
        value >>= 1U;
        bits++;
    }

    return bits;
}

static unsigned int training_table_get_set_index(Addr ld1_pc_addr) {
    return (unsigned int)(training_table_fold_pc(ld1_pc_addr) &
                          (training_table_num_sets - 1U));
}

static uint64_t training_table_get_ld1_tag(Addr ld1_pc_addr) {
    return training_table_fold_pc(ld1_pc_addr) >> training_table_set_index_bits;
}

static bool training_table_tags_match(
    const TrainingTableEntry* entry,
    uint64_t ld1_tag,
    uint64_t ld2_tag,
    unsigned int offset_delta,
    bool direction,
    unsigned int ld2_memory_access_size) {
    return entry->ld1_tag == ld1_tag &&
           entry->ld2_tag == ld2_tag &&
           entry->offset_delta == offset_delta &&
           entry->direction == direction &&
           entry->ld2_memory_access_size == ld2_memory_access_size;
}

static void training_table_init_entry_fields_ideal(
    TrainingTableEntry* entry,
    Addr ld1_pc_addr,
    Addr ld2_pc_addr,
    unsigned int offset_delta,
    bool direction,
    unsigned int ld2_memory_access_size) {
    entry->ld1_tag                = (uint64_t)ld1_pc_addr;
    entry->ld2_tag                = (uint64_t)ld2_pc_addr;
    entry->offset_delta           = offset_delta;
    entry->direction              = direction;
    entry->ld2_memory_access_size = ld2_memory_access_size;
    entry->observation_count      = 0;
    entry->valid                  = true;
}

static void training_table_init_entry_fields_realistic(
    TrainingTableEntry* entry,
    Addr ld1_pc_addr,
    Addr ld2_pc_addr,
    unsigned int offset_delta,
    bool direction,
    unsigned int ld2_memory_access_size) {
    entry->ld1_tag                = training_table_get_ld1_tag(ld1_pc_addr);
    entry->ld2_tag                = training_table_fold_pc(ld2_pc_addr);
    entry->offset_delta           = offset_delta;
    entry->direction              = direction;
    entry->ld2_memory_access_size = ld2_memory_access_size;
    entry->observation_count      = 0;
    entry->valid                  = true;
}

static TrainingTableEntry* training_table_entry_at(unsigned int set_idx,
                                                     unsigned int way) {
    return &training_table_entries[(set_idx * training_table_num_ways) + way];
}

/**
 * Marks one way as most-recently used in an 8-way tree PLRU set.
 *
 * Tree layout:
 *           bit0
 *          /    \
 *       bit1    bit2
 *      /  \    /  \
 *   bit3 bit4 bit5 bit6
 *   W0 W1 W2 W3 W4 W5 W6 W7
 */
static void training_table_plru_touch(unsigned int set_idx, unsigned int way) {
    uint8_t state = training_table_plru[set_idx] & 0x7FU;

    switch (way) {
        case 0U:
            state |= 0x0BU; /* bit0=1, bit1=1, bit3=1 */
            break;
        case 1U:
            state |= 0x03U; /* bit0=1, bit1=1 */
            state &= (uint8_t)~0x08U; /* bit3=0 */
            break;
        case 2U:
            state |= 0x11U; /* bit0=1, bit4=1 */
            state &= (uint8_t)~0x02U; /* bit1=0 */
            break;
        case 3U:
            state |= 0x01U; /* bit0=1 */
            state &= (uint8_t)~0x12U; /* bit1=0, bit4=0 */
            break;
        case 4U:
            state |= 0x24U; /* bit2=1, bit5=1 */
            state &= (uint8_t)~0x01U; /* bit0=0 */
            break;
        case 5U:
            state |= 0x04U; /* bit2=1 */
            state &= (uint8_t)~0x21U; /* bit0=0, bit5=0 */
            break;
        case 6U:
            state |= 0x40U; /* bit6=1 */
            state &= (uint8_t)~0x05U; /* bit0=0, bit2=0 */
            break;
        case 7U:
            state &= (uint8_t)~0x45U; /* bit0=0, bit2=0, bit6=0 */
            break;
        default:
            return;
    }

    training_table_plru[set_idx] = state;
}

static unsigned int training_table_plru_victim(unsigned int set_idx) {
    uint8_t state = training_table_plru[set_idx] & 0x7FU;

    if ((state & 0x1U) != 0U) {
        if ((state & 0x2U) != 0U) {
            return ((state & 0x8U) != 0U) ? 0U : 1U;
        }
        return ((state & 0x10U) != 0U) ? 2U : 3U;
    }
    if ((state & 0x4U) != 0U) {
        return ((state & 0x20U) != 0U) ? 4U : 5U;
    }
    return ((state & 0x40U) != 0U) ? 6U : 7U;
}

// Hash-table helpers

/**
 * Returns the software hash for one exact fusible pair pattern.
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
 * Ideal-capacity lookup: probe the full backing table before failing.
 */
static TrainingTableEntry* training_table_find_entry_ideal(
    Addr ld1_pc_addr,
    Addr ld2_pc_addr,
    unsigned int offset_delta,
    bool direction,
    unsigned int ld2_memory_access_size) {
    uint64_t hash_value = training_table_hash(
        ld1_pc_addr, ld2_pc_addr, offset_delta, direction,
        ld2_memory_access_size);
    unsigned int start_idx =
        (unsigned int)(hash_value & (training_table_num_entries - 1U));

    for (unsigned int probe = 0; probe < training_table_num_entries; probe++) {
        unsigned int idx =
            (start_idx + probe) & (training_table_num_entries - 1U);
        TrainingTableEntry* entry = &training_table_entries[idx];

        if (!entry->valid) {
            training_table_init_entry_fields_ideal(
                entry, ld1_pc_addr, ld2_pc_addr, offset_delta, direction,
                ld2_memory_access_size);
            STAT_EVENT(0, TRAINING_TABLE_INSERTS);
            training_table_note_live_insert();
            return entry;
        }

        if (training_table_keys_match_ideal(entry, ld1_pc_addr, ld2_pc_addr,
                                            offset_delta, direction,
                                            ld2_memory_access_size)) {
            return entry;
        }
    }

    STAT_EVENT(0, TRAINING_TABLE_BACKING_ALLOC_FAILURES);
    return NULL;
}

/**
 * Bounded set-associative lookup: folded LD1 PC bits select the set, stored
 * tag fields identify the way, and tree PLRU picks a victim on miss.
 */
static TrainingTableEntry* training_table_find_entry_realistic(
    Addr ld1_pc_addr,
    Addr ld2_pc_addr,
    unsigned int offset_delta,
    bool direction,
    unsigned int ld2_memory_access_size) {
    unsigned int set_idx = training_table_get_set_index(ld1_pc_addr);
    uint64_t     ld1_tag = training_table_get_ld1_tag(ld1_pc_addr);
    uint64_t     ld2_tag = training_table_fold_pc(ld2_pc_addr);

    unsigned int invalid_way = training_table_num_ways;

    for (unsigned int way = 0; way < training_table_num_ways; way++) {
        TrainingTableEntry* entry = training_table_entry_at(set_idx, way);

        if (entry->valid &&
            training_table_tags_match(entry, ld1_tag, ld2_tag, offset_delta,
                                      direction, ld2_memory_access_size)) {
            training_table_plru_touch(set_idx, way);
            return entry;
        }

        if (!entry->valid && invalid_way == training_table_num_ways) {
            invalid_way = way;
        }
    }

    if (invalid_way < training_table_num_ways) {
        TrainingTableEntry* invalid_entry =
            training_table_entry_at(set_idx, invalid_way);
        training_table_init_entry_fields_realistic(
            invalid_entry, ld1_pc_addr, ld2_pc_addr, offset_delta, direction,
            ld2_memory_access_size);
        training_table_plru_touch(set_idx, invalid_way);
        STAT_EVENT(0, TRAINING_TABLE_INSERTS);
        training_table_note_live_insert();
        return invalid_entry;
    }

    unsigned int victim_way = training_table_plru_victim(set_idx);
    TrainingTableEntry* victim = training_table_entry_at(set_idx, victim_way);
    victim->valid = false;
    victim->observation_count = 0;
    training_table_note_live_remove();
    STAT_EVENT(0, TRAINING_TABLE_EVICTED);

    training_table_init_entry_fields_realistic(
        victim, ld1_pc_addr, ld2_pc_addr, offset_delta, direction,
        ld2_memory_access_size);
    training_table_plru_touch(set_idx, victim_way);
    STAT_EVENT(0, TRAINING_TABLE_INSERTS);
    return victim;
}

static TrainingTableEntry* training_table_find_entry(
    Addr ld1_pc_addr,
    Addr ld2_pc_addr,
    unsigned int offset_delta,
    bool direction,
    unsigned int ld2_memory_access_size) {
    if (IFUSE_REALISTIC_TRAINING_TABLE) {
        return training_table_find_entry_realistic(
            ld1_pc_addr, ld2_pc_addr, offset_delta, direction,
            ld2_memory_access_size);
    }

    return training_table_find_entry_ideal(
        ld1_pc_addr, ld2_pc_addr, offset_delta, direction,
        ld2_memory_access_size);
}

/**
 * Returns the LD2 cache-line offset prediction for a retired pair.
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

    if (IFUSE_REALISTIC_TRAINING_TABLE) {
        training_table_num_sets = IFUSE_TRAINING_TABLE_SETS;
        training_table_num_ways = IFUSE_TRAINING_TABLE_WAYS;
        training_table_num_entries =
            training_table_num_sets * training_table_num_ways;

        if (training_table_num_sets == 0U ||
            (training_table_num_sets &
             (training_table_num_sets - 1U)) != 0U) {
            fprintf(stderr,
                    "TRAINING_TABLE: ifuse_training_table_sets must be "
                    "a power of two (got %u)\n",
                    training_table_num_sets);
            exit(1);
        }
        if (training_table_num_ways != 8U) {
            fprintf(stderr,
                    "TRAINING_TABLE: realistic set-associative mode requires "
                    "ifuse_training_table_ways=8 (got %u)\n",
                    training_table_num_ways);
            exit(1);
        }
        if (training_table_num_entries != IFUSE_TRAINING_TABLE_ENTRIES) {
            fprintf(stderr,
                    "TRAINING_TABLE: ifuse_training_table_entries (%u) must "
                    "equal sets*ways (%u*%u=%u)\n",
                    IFUSE_TRAINING_TABLE_ENTRIES,
                    training_table_num_sets, training_table_num_ways,
                    training_table_num_entries);
            exit(1);
        }
        if (IFUSE_TRAINING_TABLE_EVICT_POLICY != 0U) {
            fprintf(stderr,
                    "TRAINING_TABLE: only evict_policy=0 (tree PLRU) is "
                    "supported for realistic mode (got %u)\n",
                    IFUSE_TRAINING_TABLE_EVICT_POLICY);
            exit(1);
        }

        training_table_set_index_bits =
            training_table_log2_u32(training_table_num_sets);
    } else {
        training_table_set_index_bits = 0U;
        training_table_num_entries = IFUSE_IDEAL_TRAINING_TABLE_MAX_ENTRIES;
        training_table_num_sets    = 0U;
        training_table_num_ways    = 0U;
    }

    training_table_entries =
        (TrainingTableEntry*)calloc(training_table_num_entries,
                                    sizeof(TrainingTableEntry));
    if (!training_table_entries) {
        fprintf(stderr, "TRAINING_TABLE: calloc failed for %u entries\n",
                training_table_num_entries);
        exit(1);
    }

    if (IFUSE_REALISTIC_TRAINING_TABLE) {
        training_table_plru =
            (uint8_t*)calloc(training_table_num_sets, sizeof(uint8_t));
        if (!training_table_plru) {
            fprintf(stderr,
                    "TRAINING_TABLE: calloc failed for %u PLRU sets\n",
                    training_table_num_sets);
            exit(1);
        }
    }

    training_table_live_count = 0;
    training_table_live_peak  = 0;
    training_table_initialized = true;
}

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
