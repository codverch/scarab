// STD headers
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Custom headers
#include "ifuse_fct.h"
#include "ifuse_training_table.h"
#include "../general.param.h"
#include "../statistics.h"
#include "ifuse_ideal_limits.h"
#include "ifuse.param.h"

/**
 * Ideal Fusion Candidate Table (FCT) runtime policy.
 *
 * The ideal FCT models a single LD2 candidate for each LD1 PC. 
 * Retire-time training can insert entries either on the first observation of
 * an LD1-LD2 pair or after the training table validates the pair as frequent.
 * If the same LD1 is later observed with a different LD2, the existing entry
 * is updated with the new pairing.
 *
 * Realistic mode uses a set-associative table keyed by LD1 PC, the low bits
 * of the folded LD1 PC select the set, and the remaining LD1 tag bits are
 * compared across the ways, with a 7-bit tree PLRU replacement policy per set.
 */

#define FCT_PC_BITS 48U
#define FCT_PC_MASK 0xFFFFFFFFFFFFULL

static FCT_Row* fct_rows = NULL;
static uint8_t*   fct_plru = NULL;
static size_t     fct_num_hash_table_rows = 0;
static unsigned int fct_num_sets = 0;
static unsigned int fct_num_ways = 0;
static unsigned int fct_num_entries = 0;
static unsigned int fct_set_index_bits = 0;
static bool       fct_is_initialized = false;
static uint64_t   fct_live_row_count = 0;
static uint64_t   fct_live_row_peak = 0;

static void fct_note_new_live_row(unsigned int proc_id) {
    fct_live_row_count++;
    if (fct_live_row_count > fct_live_row_peak) {
        INC_STAT_EVENT(proc_id, FCT_PEAK_LIVE,
                       fct_live_row_count - fct_live_row_peak);
        fct_live_row_peak = fct_live_row_count;
    }
}

static void fct_note_live_remove(void) {
    if (fct_live_row_count > 0) {
        fct_live_row_count--;
    }
}

static unsigned int fct_log2_u32(unsigned int value) {
    unsigned int bits = 0U;

    while (value > 1U) {
        value >>= 1U;
        bits++;
    }

    return bits;
}

static FCT_Row* fct_row_at(unsigned int set_idx, unsigned int way) {
    return &fct_rows[(set_idx * fct_num_ways) + way];
}

/**
 * Marks one way as most-recently used in an 8-way tree PLRU set.
 */
static void fct_plru_touch(unsigned int set_idx, unsigned int way) {
    uint8_t state = fct_plru[set_idx] & 0x7FU;

    switch (way) {
        case 0U:
            state |= 0x0BU;
            break;
        case 1U:
            state |= 0x03U;
            state &= (uint8_t)~0x08U;
            break;
        case 2U:
            state |= 0x11U;
            state &= (uint8_t)~0x02U;
            break;
        case 3U:
            state |= 0x01U;
            state &= (uint8_t)~0x12U;
            break;
        case 4U:
            state |= 0x24U;
            state &= (uint8_t)~0x01U;
            break;
        case 5U:
            state |= 0x04U;
            state &= (uint8_t)~0x21U;
            break;
        case 6U:
            state |= 0x40U;
            state &= (uint8_t)~0x05U;
            break;
        case 7U:
            state &= (uint8_t)~0x45U;
            break;
        default:
            return;
    }

    fct_plru[set_idx] = state;
}

static unsigned int fct_plru_victim(unsigned int set_idx) {
    uint8_t state = fct_plru[set_idx] & 0x7FU;

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

void fct_init(void) {
    if (fct_is_initialized) {
        return;
    }

    if (IFUSE_REALISTIC_FCT) {
        fct_num_sets = IFUSE_FCT_SETS;
        fct_num_ways = IFUSE_FCT_WAYS;
        fct_num_entries = fct_num_sets * fct_num_ways;
        fct_num_hash_table_rows = fct_num_entries;

        if (fct_num_sets == 0U ||
            (fct_num_sets & (fct_num_sets - 1U)) != 0U) {
            fprintf(stderr,
                    "FCT: ifuse_fct_sets must be a power of two (got %u)\n",
                    fct_num_sets);
            exit(1);
        }
        if (fct_num_ways != 8U) {
            fprintf(stderr,
                    "FCT: realistic set-associative mode requires "
                    "ifuse_fct_ways=8 (got %u)\n",
                    fct_num_ways);
            exit(1);
        }
        if (fct_num_entries != IFUSE_FCT_ENTRIES) {
            fprintf(stderr,
                    "FCT: ifuse_fct_entries (%u) must equal sets*ways "
                    "(%u*%u=%u)\n",
                    IFUSE_FCT_ENTRIES, fct_num_sets, fct_num_ways,
                    fct_num_entries);
            exit(1);
        }
        if (IFUSE_FCT_EVICT_POLICY != 0U) {
            fprintf(stderr,
                    "FCT: only evict_policy=0 (tree PLRU) is supported for "
                    "realistic mode (got %u)\n",
                    IFUSE_FCT_EVICT_POLICY);
            exit(1);
        }

        fct_set_index_bits = fct_log2_u32(fct_num_sets);
    } else {
        fct_set_index_bits = 0U;
        fct_num_hash_table_rows = 1U << IFUSE_IDEAL_FCT_DEFAULT_HASH_BITS;
        fct_num_sets = 0U;
        fct_num_ways = 0U;
        fct_num_entries = 0U;
    }

    fct_rows = (FCT_Row*)calloc(fct_num_hash_table_rows, sizeof(FCT_Row));
    if (!fct_rows) {
        fprintf(stderr, "FCT: calloc failed for %zu rows\n",
                fct_num_hash_table_rows);
        exit(1);
    }

    if (IFUSE_REALISTIC_FCT) {
        fct_plru = (uint8_t*)calloc(fct_num_sets, sizeof(uint8_t));
        if (!fct_plru) {
            fprintf(stderr, "FCT: calloc failed for %u PLRU sets\n",
                    fct_num_sets);
            exit(1);
        }
    }

    fct_live_row_count = 0;
    fct_live_row_peak  = 0;
    fct_is_initialized = true;

    training_table_init();
}

static uint64_t fct_fold_pc(Addr pc_addr) {
    return (uint64_t)pc_addr & FCT_PC_MASK;
}

static unsigned int fct_get_set_index(Addr ld1_pc_addr) {
    return (unsigned int)(fct_fold_pc(ld1_pc_addr) & (fct_num_sets - 1U));
}

static uint64_t fct_get_ld1_tag(Addr ld1_pc_addr) {
    return fct_fold_pc(ld1_pc_addr) >> fct_set_index_bits;
}

/**
 * Returns the first software hash-table slot to probe for ld1_pc_addr.
 */
static size_t fct_get_probe_start_idx(Addr ld1_pc_addr, size_t row_index_mask) {
    uint64_t h = (uint64_t)ld1_pc_addr;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (size_t)(h & row_index_mask);
}

static unsigned int fct_max_confidence_score(void) {
    return IFUSE_FCT_CONF_THRESHOLD;
}

static unsigned int fct_saturating_confidence_score(
    unsigned int confidence_score) {
    const unsigned int max_confidence_score = fct_max_confidence_score();
    return (confidence_score > max_confidence_score) ?
        max_confidence_score : confidence_score;
}

static void fct_increment_confidence_score(FCT_Row* row) {
    if (!row) {
        return;
    }

    row->confidence_score =
        fct_saturating_confidence_score(row->confidence_score + 1U);
}

static void fct_write_row(FCT_Row* row, Addr ld1_pc_addr, Addr ld2_pc_addr,
                          Addr ld1_effective_addr, Addr ld2_effective_addr,
                          unsigned int offset_delta,
                          bool direction, unsigned int ld2_mem_size,
                          unsigned int ld1_micro_op_num,
                          unsigned int ld2_micro_op_num,
                          unsigned int confidence_score) {
    row->ld1_pc_addr        = ld1_pc_addr;
    row->ld1_tag            = fct_get_ld1_tag(ld1_pc_addr);
    row->ld2_pc_addr        = ld2_pc_addr;
    row->ld1_effective_addr = ld1_effective_addr;
    row->ld2_effective_addr = ld2_effective_addr;
    row->offset_delta       = offset_delta;
    row->direction          = direction;
    row->ld2_mem_size       = ld2_mem_size;
    row->ld1_micro_op_num   = ld1_micro_op_num;
    row->ld2_micro_op_num   = ld2_micro_op_num;
    row->valid              = true;
    row->confidence_score   = fct_saturating_confidence_score(confidence_score);
}

static FCT_Row* fct_lookup_row_ideal(Addr ld1_pc_addr) {
    if (!fct_rows || fct_num_hash_table_rows == 0U) {
        return NULL;
    }

    size_t row_index_mask = fct_num_hash_table_rows - 1U;
    size_t row_idx = fct_get_probe_start_idx(ld1_pc_addr, row_index_mask);

    for (size_t num_probes = 0; num_probes < fct_num_hash_table_rows;
         num_probes++) {
        FCT_Row* row = &fct_rows[row_idx];
        if (!row->valid) {
            return NULL;
        }
        if (row->ld1_pc_addr == ld1_pc_addr) {
            return row;
        }
        row_idx = (row_idx + 1U) & row_index_mask;
    }

    return NULL;
}

static FCT_Row* fct_lookup_row_realistic(Addr ld1_pc_addr) {
    if (!fct_rows || fct_num_sets == 0U) {
        return NULL;
    }

    unsigned int set_idx = fct_get_set_index(ld1_pc_addr);
    uint64_t     ld1_tag = fct_get_ld1_tag(ld1_pc_addr);

    for (unsigned int way = 0; way < fct_num_ways; way++) {
        FCT_Row* row = fct_row_at(set_idx, way);
        if (row->valid && row->ld1_tag == ld1_tag) {
            fct_plru_touch(set_idx, way);
            return row;
        }
    }

    return NULL;
}

static FCT_Row* fct_lookup_row(Addr ld1_pc_addr) {
    if (IFUSE_REALISTIC_FCT) {
        return fct_lookup_row_realistic(ld1_pc_addr);
    }
    return fct_lookup_row_ideal(ld1_pc_addr);
}

static FCT_Row* fct_find_empty_row_ideal(Addr ld1_pc_addr) {
    if (!fct_rows || fct_num_hash_table_rows == 0U) {
        return NULL;
    }

    size_t row_index_mask = fct_num_hash_table_rows - 1U;
    size_t row_idx = fct_get_probe_start_idx(ld1_pc_addr, row_index_mask);

    for (size_t num_probes = 0; num_probes < fct_num_hash_table_rows;
         num_probes++) {
        FCT_Row* row = &fct_rows[row_idx];
        if (!row->valid) {
            return row;
        }
        row_idx = (row_idx + 1U) & row_index_mask;
    }

    return NULL;
}

/**
 * Returns a free or evicted row in the set indexed by ld1_pc_addr.
 */
static FCT_Row* fct_allocate_row_realistic(Addr ld1_pc_addr,
                                           unsigned int proc_id) {
    unsigned int set_idx = fct_get_set_index(ld1_pc_addr);
    unsigned int invalid_way = fct_num_ways;

    for (unsigned int way = 0; way < fct_num_ways; way++) {
        FCT_Row* row = fct_row_at(set_idx, way);
        if (!row->valid && invalid_way == fct_num_ways) {
            invalid_way = way;
        }
    }

    if (invalid_way < fct_num_ways) {
        FCT_Row* row = fct_row_at(set_idx, invalid_way);
        fct_plru_touch(set_idx, invalid_way);
        fct_note_new_live_row(proc_id);
        return row;
    }

    unsigned int victim_way = fct_plru_victim(set_idx);
    FCT_Row* victim = fct_row_at(set_idx, victim_way);
    victim->valid = false;
    fct_note_live_remove();
    STAT_EVENT(proc_id, FCT_EVICTED);
    fct_plru_touch(set_idx, victim_way);
    return victim;
}

static FCT_Row* fct_allocate_row_for_load1_pc(Addr ld1_pc_addr,
                                              unsigned int proc_id) {
    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (row) {
        return row;
    }

    if (IFUSE_REALISTIC_FCT) {
        return fct_allocate_row_realistic(ld1_pc_addr, proc_id);
    }

    row = fct_find_empty_row_ideal(ld1_pc_addr);
    if (!row) {
        fprintf(stderr,
                "FCT: ideal backing hash table exhausted; increase "
                "IFUSE_IDEAL_FCT_DEFAULT_HASH_BITS\n");
        exit(1);
    }
    fct_note_new_live_row(proc_id);
    return row;
}

FCT_Row* fct_lookup(Addr ld1_pc_addr) {
    if (!fct_is_initialized || ld1_pc_addr == 0) {
        return NULL;
    }
    return fct_lookup_row(ld1_pc_addr);
}

void fct_update_confidence(Addr ld1_pc_addr, bool prediction_correct) {
    if (!fct_is_initialized) {
        return;
    }

    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (!row) {
        return;
    }

    if (prediction_correct) {
        fct_increment_confidence_score(row);
        return;
    }

    const unsigned int confidence_penalty =
        IFUSE_FCT_MISPRED_CONF_PENALTY;
    row->confidence_score = (row->confidence_score <= confidence_penalty) ?
        0 : row->confidence_score - confidence_penalty;
}

Flag fct_has_load1_pc_entry(Addr ld1_pc_addr) {
    if (!fct_is_initialized) {
        return FALSE;
    }
    return fct_lookup_row(ld1_pc_addr) ? TRUE : FALSE;
}

void fct_update_ld2_candidate_for_ld1(Addr ld1_pc_addr, Addr ld2_pc_addr,
                                      Addr ld1_effective_addr,
                                      Addr ld2_effective_addr,
                                      unsigned int offset_delta, bool direction,
                                      unsigned int ld2_mem_size,
                                      unsigned int ld1_micro_op_num,
                                      unsigned int ld2_micro_op_num,
                                      unsigned int proc_id) {
    if (!fct_is_initialized) {
        fct_init();
    }
    if (ld1_pc_addr == 0 || ld2_pc_addr == 0 || offset_delta > 63U) {
        return;
    }

    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (!row) {
        if (!IFUSE_FCT_INSERT_ON_FIRST_OBS) {
            return;
        }

        row = fct_allocate_row_for_load1_pc(ld1_pc_addr, proc_id);
        fct_write_row(row, ld1_pc_addr, ld2_pc_addr, ld1_effective_addr,
                      ld2_effective_addr, offset_delta, direction, ld2_mem_size,
                      ld1_micro_op_num, ld2_micro_op_num,
                      IFUSE_FCT_INITIAL_CONF);
        return;
    }

    if (row->ld2_pc_addr == ld2_pc_addr) {
        fct_increment_confidence_score(row);
        return;
    }

    STAT_EVENT(proc_id, FCT_LD2_REPLACEMENTS);
    fct_write_row(row, ld1_pc_addr, ld2_pc_addr, ld1_effective_addr,
                  ld2_effective_addr, offset_delta, direction, ld2_mem_size,
                  ld1_micro_op_num, ld2_micro_op_num,
                  IFUSE_FCT_INITIAL_CONF);
}

void fct_reinforce_ld2_candidate_for_ld1(Addr ld1_pc_addr, Addr ld2_pc_addr,
                                         unsigned int offset_delta,
                                         bool direction,
                                         unsigned int ld2_mem_size) {
    if (!fct_is_initialized || ld1_pc_addr == 0 || ld2_pc_addr == 0) {
        return;
    }

    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (!row || row->ld2_pc_addr != ld2_pc_addr ||
        row->offset_delta != offset_delta ||
        row->direction != direction ||
        row->ld2_mem_size != ld2_mem_size) {
        return;
    }

    fct_increment_confidence_score(row);
}

Flag fct_promote_ld2_candidate_for_ld1(Addr ld1_pc_addr, Addr ld2_pc_addr,
                                       Addr ld1_effective_addr,
                                       Addr ld2_effective_addr,
                                       unsigned int offset_delta,
                                       bool direction,
                                       unsigned int ld2_mem_size,
                                       unsigned int ld1_micro_op_num,
                                       unsigned int ld2_micro_op_num,
                                       unsigned int proc_id) {
    if (!fct_is_initialized) {
        fct_init();
    }
    if (ld1_pc_addr == 0 || ld2_pc_addr == 0 || offset_delta > 63U) {
        return FALSE;
    }

    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (row) {
        STAT_EVENT(proc_id, TRAINING_TABLE_PROMOTION_SKIPPED_FCT_EXISTS);
        return FALSE;
    }

    row = fct_allocate_row_for_load1_pc(ld1_pc_addr, proc_id);
    fct_write_row(row, ld1_pc_addr, ld2_pc_addr, ld1_effective_addr,
                  ld2_effective_addr, offset_delta, direction, ld2_mem_size,
                  ld1_micro_op_num, ld2_micro_op_num,
                  IFUSE_FCT_INITIAL_CONF);
    return TRUE;
}
