// STD headers
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
 * The ideal FCT models one LD2 candidate and up to two offset deltas for each
 * LD1 PC. Retire-time training can insert entries either on the first
 * observation of an LD1-LD2 pair or after the training table validates the
 * pair as frequent. If the same LD1 is later observed with a different LD2,
 * the existing entry is updated with the new pairing.
 *
 * Simulator state is maintained in a large open-addressed hash table with
 * 2^IFUSE_IDEAL_FCT_DEFAULT_HASH_BITS entries.
 */

static FCT_Row* fct_rows = NULL;
static size_t   fct_num_hash_table_rows = 0;
static bool     fct_is_initialized = false;

void fct_init(void) {
    if (fct_is_initialized) {
        return;
    }

    fct_num_hash_table_rows = 1U << IFUSE_IDEAL_FCT_DEFAULT_HASH_BITS;
    fct_rows = (FCT_Row*)calloc(fct_num_hash_table_rows, sizeof(FCT_Row));
    if (!fct_rows) {
        fprintf(stderr, "FCT: calloc failed for %zu ideal rows\n",
                fct_num_hash_table_rows);
        exit(1);
    }
    fct_is_initialized = true;

    training_table_init();
}

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

static void fct_clear_delta_slots(FCT_Row* row) {
    memset(row->delta_slots, 0, sizeof(row->delta_slots));
}

static void fct_init_delta_slot(FCT_DeltaSlot* slot,
                                unsigned int offset_delta,
                                bool direction,
                                unsigned int confidence_score) {
    slot->offset_delta       = offset_delta;
    slot->direction          = direction;
    slot->confidence_score   = fct_saturating_confidence_score(confidence_score);
    slot->valid              = true;
}

static void fct_increment_delta_slot_confidence(FCT_DeltaSlot* slot) {
    if (!slot || !slot->valid) {
        return;
    }

    slot->confidence_score =
        fct_saturating_confidence_score(slot->confidence_score + 1U);
}

static int fct_find_delta_slot(const FCT_Row* row,
                               unsigned int offset_delta,
                               bool direction) {
    for (unsigned int slot_idx = 0; slot_idx < FCT_NUM_DELTA_SLOTS; slot_idx++) {
        const FCT_DeltaSlot* slot = &row->delta_slots[slot_idx];
        if (slot->valid &&
            slot->offset_delta == offset_delta &&
            slot->direction == direction) {
            return (int)slot_idx;
        }
    }
    return -1;
}

static int fct_find_empty_delta_slot(const FCT_Row* row) {
    for (unsigned int slot_idx = 0; slot_idx < FCT_NUM_DELTA_SLOTS; slot_idx++) {
        if (!row->delta_slots[slot_idx].valid) {
            return (int)slot_idx;
        }
    }
    return -1;
}

static int fct_find_weakest_delta_slot(const FCT_Row* row) {
    int weakest_slot_idx = -1;
    unsigned int weakest_confidence = 0;

    for (unsigned int slot_idx = 0; slot_idx < FCT_NUM_DELTA_SLOTS; slot_idx++) {
        const FCT_DeltaSlot* slot = &row->delta_slots[slot_idx];
        if (!slot->valid) {
            continue;
        }
        if (weakest_slot_idx < 0 ||
            slot->confidence_score < weakest_confidence) {
            weakest_slot_idx = (int)slot_idx;
            weakest_confidence = slot->confidence_score;
        }
    }

    return weakest_slot_idx;
}

static void fct_write_row_metadata(FCT_Row* row,
                                   Addr ld1_pc_addr,
                                   Addr ld2_pc_addr,
                                   Addr ld1_effective_addr,
                                   Addr ld2_effective_addr,
                                   unsigned int ld2_mem_size,
                                   unsigned int ld1_micro_op_num,
                                   unsigned int ld2_micro_op_num) {
    row->ld1_pc_addr        = ld1_pc_addr;
    row->ld2_pc_addr        = ld2_pc_addr;
    row->ld1_effective_addr = ld1_effective_addr;
    row->ld2_effective_addr = ld2_effective_addr;
    row->ld2_mem_size       = ld2_mem_size;
    row->ld1_micro_op_num   = ld1_micro_op_num;
    row->ld2_micro_op_num   = ld2_micro_op_num;
    row->valid              = true;
}

static void fct_write_new_row_with_delta(FCT_Row* row,
                                         Addr ld1_pc_addr,
                                         Addr ld2_pc_addr,
                                         Addr ld1_effective_addr,
                                         Addr ld2_effective_addr,
                                         unsigned int offset_delta,
                                         bool direction,
                                         unsigned int ld2_mem_size,
                                         unsigned int ld1_micro_op_num,
                                         unsigned int ld2_micro_op_num,
                                         unsigned int confidence_score) {
    fct_clear_delta_slots(row);
    fct_write_row_metadata(row, ld1_pc_addr, ld2_pc_addr,
                           ld1_effective_addr, ld2_effective_addr,
                           ld2_mem_size, ld1_micro_op_num, ld2_micro_op_num);
    fct_init_delta_slot(&row->delta_slots[0], offset_delta, direction,
                        confidence_score);
}

static void fct_install_or_reinforce_delta(FCT_Row* row,
                                           unsigned int offset_delta,
                                           bool direction,
                                           unsigned int confidence_score,
                                           unsigned int proc_id,
                                           bool track_second_promotion) {
    int existing_slot_idx =
        fct_find_delta_slot(row, offset_delta, direction);
    if (existing_slot_idx >= 0) {
        fct_increment_delta_slot_confidence(
            &row->delta_slots[existing_slot_idx]);
        return;
    }

    int target_slot_idx = fct_find_empty_delta_slot(row);
    if (target_slot_idx < 0) {
        target_slot_idx = fct_find_weakest_delta_slot(row);
        if (target_slot_idx < 0) {
            return;
        }
        STAT_EVENT(proc_id, FCT_DELTA_SLOT_REPLACEMENTS);
    } else if (target_slot_idx == 1 && track_second_promotion) {
        STAT_EVENT(proc_id, FCT_SECOND_DELTA_PROMOTIONS);
    }

    fct_init_delta_slot(&row->delta_slots[target_slot_idx],
                        offset_delta, direction, confidence_score);
}

int fct_select_delta_slot(const FCT_Row* row) {
    if (!row || !row->valid) {
        return -1;
    }

    int best_slot_idx = -1;
    unsigned int best_confidence = 0;

    for (unsigned int slot_idx = 0; slot_idx < FCT_NUM_DELTA_SLOTS; slot_idx++) {
        const FCT_DeltaSlot* slot = &row->delta_slots[slot_idx];
        if (!slot->valid ||
            slot->confidence_score < IFUSE_FCT_CONF_THRESHOLD) {
            continue;
        }

        if (best_slot_idx < 0 ||
            slot->confidence_score > best_confidence ||
            (slot->confidence_score == best_confidence &&
             slot_idx < (unsigned int)best_slot_idx)) {
            best_slot_idx = (int)slot_idx;
            best_confidence = slot->confidence_score;
        }
    }

    return best_slot_idx;
}

static FCT_Row* fct_lookup_row(Addr ld1_pc_addr) {
    if (!fct_rows || fct_num_hash_table_rows == 0U) {
        return NULL;
    }

    size_t row_index_mask = fct_num_hash_table_rows - 1U;
    size_t row_idx = fct_get_probe_start_idx(ld1_pc_addr, row_index_mask);

    for (size_t num_probes = 0; num_probes < fct_num_hash_table_rows; num_probes++) {
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

static FCT_Row* fct_find_empty_row(Addr ld1_pc_addr) {
    if (!fct_rows || fct_num_hash_table_rows == 0U) {
        return NULL;
    }

    size_t row_index_mask = fct_num_hash_table_rows - 1U;
    size_t row_idx = fct_get_probe_start_idx(ld1_pc_addr, row_index_mask);

    for (size_t num_probes = 0; num_probes < fct_num_hash_table_rows; num_probes++) {
        FCT_Row* row = &fct_rows[row_idx];
        if (!row->valid) {
            return row;
        }
        row_idx = (row_idx + 1U) & row_index_mask;
    }

    return NULL;
}

static FCT_Row* fct_allocate_row_for_load1_pc(Addr ld1_pc_addr) {
    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (row) {
        return row;
    }

    row = fct_find_empty_row(ld1_pc_addr);
    if (!row) {
        fprintf(stderr,
                "FCT: ideal backing hash table exhausted; increase "
                "IFUSE_IDEAL_FCT_DEFAULT_HASH_BITS\n");
        exit(1);
    }
    return row;
}

FCT_Row* fct_lookup(Addr ld1_pc_addr) {
    if (!fct_is_initialized || ld1_pc_addr == 0) {
        return NULL;
    }
    return fct_lookup_row(ld1_pc_addr);
}

void fct_update_delta_confidence(Addr ld1_pc_addr, unsigned int slot_idx,
                                 bool prediction_correct) {
    if (!fct_is_initialized || slot_idx >= FCT_NUM_DELTA_SLOTS) {
        return;
    }

    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (!row) {
        return;
    }

    FCT_DeltaSlot* slot = &row->delta_slots[slot_idx];
    if (!slot->valid) {
        return;
    }

    if (prediction_correct) {
        fct_increment_delta_slot_confidence(slot);
        return;
    }

    const unsigned int confidence_penalty = IFUSE_FCT_MISPRED_CONF_PENALTY;
    slot->confidence_score =
        (slot->confidence_score <= confidence_penalty) ?
        0U : slot->confidence_score - confidence_penalty;
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

        row = fct_allocate_row_for_load1_pc(ld1_pc_addr);
        fct_write_new_row_with_delta(row, ld1_pc_addr, ld2_pc_addr,
                                     ld1_effective_addr, ld2_effective_addr,
                                     offset_delta, direction, ld2_mem_size,
                                     ld1_micro_op_num, ld2_micro_op_num,
                                     IFUSE_FCT_INITIAL_CONF);
        return;
    }

    if (row->ld2_pc_addr == ld2_pc_addr &&
        row->ld2_mem_size == ld2_mem_size) {
        fct_install_or_reinforce_delta(row, offset_delta, direction,
                                       IFUSE_FCT_INITIAL_CONF, proc_id,
                                       false);
        return;
    }

    STAT_EVENT(proc_id, FCT_LD2_REPLACEMENTS);
    fct_write_new_row_with_delta(row, ld1_pc_addr, ld2_pc_addr,
                                 ld1_effective_addr, ld2_effective_addr,
                                 offset_delta, direction, ld2_mem_size,
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
        row->ld2_mem_size != ld2_mem_size) {
        return;
    }

    int slot_idx = fct_find_delta_slot(row, offset_delta, direction);
    if (slot_idx < 0) {
        return;
    }

    fct_increment_delta_slot_confidence(&row->delta_slots[slot_idx]);
}

void fct_promote_ld2_candidate_for_ld1(Addr ld1_pc_addr, Addr ld2_pc_addr,
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
        return;
    }

    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (!row) {
        row = fct_allocate_row_for_load1_pc(ld1_pc_addr);
        fct_write_new_row_with_delta(row, ld1_pc_addr, ld2_pc_addr,
                                     ld1_effective_addr, ld2_effective_addr,
                                     offset_delta, direction, ld2_mem_size,
                                     ld1_micro_op_num, ld2_micro_op_num,
                                     IFUSE_FCT_INITIAL_CONF);
        return;
    }

    if (row->ld2_pc_addr != ld2_pc_addr ||
        row->ld2_mem_size != ld2_mem_size) {
        STAT_EVENT(proc_id, FCT_LD2_REPLACEMENTS);
        fct_write_new_row_with_delta(row, ld1_pc_addr, ld2_pc_addr,
                                     ld1_effective_addr, ld2_effective_addr,
                                     offset_delta, direction, ld2_mem_size,
                                     ld1_micro_op_num, ld2_micro_op_num,
                                     IFUSE_FCT_INITIAL_CONF);
        return;
    }

    fct_install_or_reinforce_delta(row, offset_delta, direction,
                                   IFUSE_FCT_INITIAL_CONF, proc_id, true);
}
