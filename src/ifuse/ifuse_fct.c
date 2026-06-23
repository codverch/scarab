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
 * Ideal Fusion Candidate Table (FCT).
 *
 * Each LD1 PC maps to one LD2 candidate and up to four cache-line offset
 * deltas. The row keeps pair-fusion confidence (LD1 fuses with this LD2 PC)
 * capped at IFUSE_FCT_PAIR_MAX_CONF. Promotion installs pair confidence at
 * that level; mispredictions decrement pair confidence without increasing it
 * on correct predictions. Offset-delta confidence is per delta slot.
 */

static FCT_Row* fct_rows = NULL;
static size_t   fct_num_hash_table_rows = 0;
static bool     fct_is_initialized = false;
static uint64_t fct_delta_update_timestamp = 0;

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
    fct_delta_update_timestamp = 0;

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
    unsigned int max_confidence_score = IFUSE_FCT_MAX_CONF;
    if (max_confidence_score < IFUSE_FCT_CONF_THRESHOLD) {
        max_confidence_score = IFUSE_FCT_CONF_THRESHOLD;
    }
    return max_confidence_score;
}

static unsigned int fct_saturating_confidence_score(
    unsigned int confidence_score) {
    const unsigned int max_confidence_score = fct_max_confidence_score();
    return (confidence_score > max_confidence_score) ?
        max_confidence_score : confidence_score;
}

static unsigned int fct_normalized_training_threshold(unsigned int threshold) {
    return (threshold == 0U) ? 1U : threshold;
}

static unsigned int fct_promoted_delta_confidence(void) {
    return fct_saturating_confidence_score(IFUSE_FCT_CONF_THRESHOLD);
}

static unsigned int fct_max_pair_confidence_score(void) {
    unsigned int max_confidence_score = IFUSE_FCT_PAIR_MAX_CONF;
    if (max_confidence_score < IFUSE_FCT_PAIR_CONF_THRESHOLD) {
        max_confidence_score = IFUSE_FCT_PAIR_CONF_THRESHOLD;
    }
    return max_confidence_score;
}

static unsigned int fct_saturating_pair_confidence_score(
    unsigned int confidence_score) {
    const unsigned int max_confidence_score = fct_max_pair_confidence_score();
    return (confidence_score > max_confidence_score) ?
        max_confidence_score : confidence_score;
}

static unsigned int fct_promoted_pair_confidence(void) {
    return fct_saturating_pair_confidence_score(IFUSE_FCT_PAIR_CONF_THRESHOLD);
}

static bool fct_pair_is_selectable(const FCT_Row* row) {
    if (!row || !row->valid) {
        return false;
    }

    if (IFUSE_FCT_PAIR_CONF_THRESHOLD == 0U) {
        return true;
    }

    return row->pair_confidence_score >= IFUSE_FCT_PAIR_CONF_THRESHOLD;
}

static void fct_penalize_pair_confidence(FCT_Row* row) {
    unsigned int confidence_penalty;

    if (!row || !row->valid) {
        return;
    }

    confidence_penalty = IFUSE_FCT_PAIR_MISPRED_CONF_PENALTY;
    row->pair_confidence_score =
        (row->pair_confidence_score <= confidence_penalty) ?
        0U : row->pair_confidence_score - confidence_penalty;
}

static void fct_clear_delta_slots(FCT_Row* row) {
    memset(row->delta_slots, 0, sizeof(row->delta_slots));
}

static void fct_init_delta_slot(FCT_DeltaSlot* slot,
                                unsigned int offset_delta,
                                bool direction,
                                unsigned int confidence_score) {
    memset(slot, 0, sizeof(*slot));
    slot->offset_delta     = offset_delta;
    slot->direction        = direction;
    slot->confidence_score = fct_saturating_confidence_score(confidence_score);
    slot->valid            = true;
}

static bool fct_delta_slot_is_selectable(const FCT_DeltaSlot* slot) {
    if (!slot || !slot->valid) {
        return false;
    }

    if (IFUSE_FCT_CONF_THRESHOLD == 0U) {
        return true;
    }

    return slot->confidence_score >= IFUSE_FCT_CONF_THRESHOLD;
}

static bool fct_tie_prefers_candidate(const FCT_DeltaSlot* candidate,
                                      unsigned int candidate_idx,
                                      const FCT_DeltaSlot* best,
                                      unsigned int best_idx) {
    if (candidate->last_correct_timestamp != best->last_correct_timestamp) {
        return candidate->last_correct_timestamp > best->last_correct_timestamp;
    }

    return candidate_idx < best_idx;
}

static void fct_reward_delta_slot_confidence(FCT_DeltaSlot* slot) {
    if (!slot || !slot->valid) {
        return;
    }

    slot->confidence_score =
        fct_saturating_confidence_score(
            slot->confidence_score + IFUSE_FCT_CORRECT_CONF_REWARD);
    slot->last_correct_timestamp = ++fct_delta_update_timestamp;
}

static void fct_bootstrap_delta_slot_confidence(FCT_DeltaSlot* slot) {
    unsigned int bootstrap_cap;
    unsigned int new_confidence_score;

    if (!slot || !slot->valid) {
        return;
    }

    bootstrap_cap = IFUSE_FCT_CONF_THRESHOLD;
    if (bootstrap_cap > 0U) {
        bootstrap_cap--;
    }

    if (slot->confidence_score >= bootstrap_cap) {
        return;
    }

    new_confidence_score =
        slot->confidence_score + IFUSE_FCT_CORRECT_CONF_REWARD;
    if (new_confidence_score > bootstrap_cap) {
        new_confidence_score = bootstrap_cap;
    }
    slot->confidence_score = new_confidence_score;
}

static void fct_penalize_delta_slot_confidence(FCT_DeltaSlot* slot) {
    unsigned int confidence_penalty;

    if (!slot || !slot->valid) {
        return;
    }

    confidence_penalty = IFUSE_FCT_MISPRED_CONF_PENALTY;
    slot->confidence_score =
        (slot->confidence_score <= confidence_penalty) ?
        0U : slot->confidence_score - confidence_penalty;
}

static void fct_increment_delta_slot_confidence(FCT_DeltaSlot* slot,
                                                bool mark_correct) {
    if (!slot || !slot->valid) {
        return;
    }

    if (mark_correct) {
        fct_reward_delta_slot_confidence(slot);
    } else {
        fct_bootstrap_delta_slot_confidence(slot);
    }
}

static void fct_offset_delta_from_addresses(Addr ld1_effective_addr,
                                            Addr ld2_effective_addr,
                                            unsigned int* offset_delta,
                                            bool* direction) {
    unsigned int ld1_offset =
        (unsigned int)(ld1_effective_addr & 63U);
    unsigned int ld2_offset =
        (unsigned int)(ld2_effective_addr & 63U);

    if (ld2_offset >= ld1_offset) {
        *offset_delta = ld2_offset - ld1_offset;
        *direction    = true;
    } else {
        *offset_delta = ld1_offset - ld2_offset;
        *direction    = false;
    }
}

static void fct_stat_delta_slot_promotion(unsigned int proc_id,
                                          int target_slot_idx) {
    switch (target_slot_idx) {
    case 1:
        STAT_EVENT(proc_id, FCT_SECOND_DELTA_PROMOTIONS);
        break;
    case 2:
        STAT_EVENT(proc_id, FCT_THIRD_DELTA_PROMOTIONS);
        break;
    case 3:
        STAT_EVENT(proc_id, FCT_FOURTH_DELTA_PROMOTIONS);
        break;
    default:
        break;
    }
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
            slot->confidence_score < weakest_confidence ||
            (slot->confidence_score == weakest_confidence &&
             slot->last_correct_timestamp <
                 row->delta_slots[weakest_slot_idx].last_correct_timestamp)) {
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

static void fct_set_pair_confidence(FCT_Row* row,
                                    unsigned int confidence_score) {
    row->pair_confidence_score =
        fct_saturating_pair_confidence_score(confidence_score);
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
                                         unsigned int confidence_score,
                                         unsigned int pair_confidence_score) {
    fct_clear_delta_slots(row);
    fct_write_row_metadata(row, ld1_pc_addr, ld2_pc_addr,
                           ld1_effective_addr, ld2_effective_addr,
                           ld2_mem_size, ld1_micro_op_num, ld2_micro_op_num);
    fct_set_pair_confidence(row, pair_confidence_score);
    fct_init_delta_slot(&row->delta_slots[0], offset_delta, direction,
                        confidence_score);
}

static void fct_install_observed_delta_slot(FCT_Row* row,
                                            unsigned int offset_delta,
                                            bool direction,
                                            unsigned int proc_id) {
    int target_slot_idx = fct_find_empty_delta_slot(row);

    if (target_slot_idx < 0) {
        target_slot_idx = fct_find_weakest_delta_slot(row);
        if (target_slot_idx < 0) {
            return;
        }
        STAT_EVENT(proc_id, FCT_DELTA_SLOT_REPLACEMENTS);
    } else if (target_slot_idx > 0) {
        fct_stat_delta_slot_promotion(proc_id, target_slot_idx);
    }

    fct_init_delta_slot(&row->delta_slots[target_slot_idx], offset_delta,
                        direction, IFUSE_FCT_INITIAL_CONF);
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
            &row->delta_slots[existing_slot_idx], false);
        return;
    }

    int target_slot_idx = fct_find_empty_delta_slot(row);
    if (target_slot_idx < 0) {
        target_slot_idx = fct_find_weakest_delta_slot(row);
        if (target_slot_idx < 0) {
            return;
        }
        STAT_EVENT(proc_id, FCT_DELTA_SLOT_REPLACEMENTS);
    } else if (track_second_promotion && target_slot_idx > 0) {
        fct_stat_delta_slot_promotion(proc_id, target_slot_idx);
    }

    fct_init_delta_slot(&row->delta_slots[target_slot_idx],
                        offset_delta, direction, confidence_score);
}

int fct_select_delta_slot(const FCT_Row* row, Addr ld1_effective_addr) {
    int best_slot_idx = -1;

    (void)ld1_effective_addr;

    if (!row || !row->valid) {
        return -1;
    }

    for (unsigned int slot_idx = 0; slot_idx < FCT_NUM_DELTA_SLOTS; slot_idx++) {
        const FCT_DeltaSlot* slot = &row->delta_slots[slot_idx];
        if (!fct_delta_slot_is_selectable(slot)) {
            continue;
        }

        if (best_slot_idx < 0) {
            best_slot_idx = (int)slot_idx;
            continue;
        }

        const FCT_DeltaSlot* best_slot =
            &row->delta_slots[best_slot_idx];
        if (slot->confidence_score > best_slot->confidence_score ||
            (slot->confidence_score == best_slot->confidence_score &&
             fct_tie_prefers_candidate(
                 slot, slot_idx, best_slot,
                 (unsigned int)best_slot_idx))) {
            best_slot_idx = (int)slot_idx;
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

Flag fct_row_is_fusible(const FCT_Row* row) {
    return fct_pair_is_selectable(row) ? TRUE : FALSE;
}

void fct_update_pair_confidence(Addr ld1_pc_addr, bool prediction_correct) {
    FCT_Row* row;

    if (!fct_is_initialized || prediction_correct) {
        return;
    }

    row = fct_lookup_row(ld1_pc_addr);
    if (!row) {
        return;
    }

    fct_penalize_pair_confidence(row);
}

unsigned int fct_get_training_insert_threshold(Addr ld1_pc_addr,
                                               Addr ld2_pc_addr,
                                               unsigned int offset_delta,
                                               bool direction,
                                               unsigned int ld2_mem_size) {
    const unsigned int primary =
        fct_normalized_training_threshold(IFUSE_TRAINING_INSERT_THRESHOLD);
    const unsigned int secondary =
        fct_normalized_training_threshold(
            IFUSE_TRAINING_SECONDARY_INSERT_THRESHOLD);
    FCT_Row* row;

    if (!fct_is_initialized || ld1_pc_addr == 0 || ld2_pc_addr == 0) {
        return primary;
    }

    row = fct_lookup_row(ld1_pc_addr);
    if (!row || !row->valid) {
        return primary;
    }
    if (row->ld2_pc_addr != ld2_pc_addr ||
        row->ld2_mem_size != ld2_mem_size) {
        return primary;
    }
    if (fct_find_delta_slot(row, offset_delta, direction) >= 0) {
        return secondary;
    }

    for (unsigned int slot_idx = 0; slot_idx < FCT_NUM_DELTA_SLOTS; slot_idx++) {
        if (row->delta_slots[slot_idx].valid) {
            return secondary;
        }
    }

    return primary;
}

void fct_update_delta_confidence(Addr ld1_pc_addr, unsigned int slot_idx,
                                 Addr ld1_effective_addr,
                                 bool prediction_correct) {
    FCT_DeltaSlot* slot;
    FCT_Row* row;

    (void)ld1_effective_addr;

    if (!fct_is_initialized || slot_idx >= FCT_NUM_DELTA_SLOTS) {
        return;
    }

    row = fct_lookup_row(ld1_pc_addr);
    if (!row) {
        return;
    }

    slot = &row->delta_slots[slot_idx];
    if (!slot->valid) {
        return;
    }

    if (prediction_correct) {
        fct_reward_delta_slot_confidence(slot);
    } else {
        fct_penalize_delta_slot_confidence(slot);
    }
}

void fct_update_delta_confidence_on_offset_misprediction(
    Addr ld1_pc_addr,
    unsigned int mispredicted_slot_idx,
    Addr ld1_effective_addr,
    Addr ld2_effective_addr,
    unsigned int proc_id) {
    unsigned int observed_offset_delta = 0;
    bool         observed_direction    = false;
    int          observed_slot_idx;
    FCT_Row*     row;

    if (!fct_is_initialized ||
        mispredicted_slot_idx >= FCT_NUM_DELTA_SLOTS) {
        return;
    }

    row = fct_lookup_row(ld1_pc_addr);
    if (!row) {
        return;
    }

    fct_offset_delta_from_addresses(ld1_effective_addr, ld2_effective_addr,
                                    &observed_offset_delta,
                                    &observed_direction);
    if (observed_offset_delta > 63U) {
        return;
    }

    if (row->delta_slots[mispredicted_slot_idx].valid) {
        fct_penalize_delta_slot_confidence(
            &row->delta_slots[mispredicted_slot_idx]);
    }

    observed_slot_idx = fct_find_delta_slot(
        row, observed_offset_delta, observed_direction);
    if (observed_slot_idx >= 0 &&
        (unsigned int)observed_slot_idx != mispredicted_slot_idx) {
        STAT_EVENT(proc_id, FCT_OFFSET_MISPRED_KNOWN_ALT_DELTA);
        fct_reward_delta_slot_confidence(
            &row->delta_slots[observed_slot_idx]);
        return;
    }

    if (observed_slot_idx < 0) {
        STAT_EVENT(proc_id, FCT_OFFSET_MISPRED_UNTRACKED_DELTA);
        fct_install_observed_delta_slot(row, observed_offset_delta,
                                        observed_direction, proc_id);
    }
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
    FCT_Row* row;

    if (!fct_is_initialized) {
        fct_init();
    }
    if (ld1_pc_addr == 0 || ld2_pc_addr == 0 || offset_delta > 63U) {
        return;
    }

    row = fct_lookup_row(ld1_pc_addr);
    if (!row) {
        if (!IFUSE_FCT_INSERT_ON_FIRST_OBS) {
            return;
        }

        row = fct_allocate_row_for_load1_pc(ld1_pc_addr);
        fct_write_new_row_with_delta(row, ld1_pc_addr, ld2_pc_addr,
                                     ld1_effective_addr, ld2_effective_addr,
                                     offset_delta, direction, ld2_mem_size,
                                     ld1_micro_op_num, ld2_micro_op_num,
                                     IFUSE_FCT_INITIAL_CONF,
                                     IFUSE_FCT_PAIR_INITIAL_CONF);
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
                                 IFUSE_FCT_INITIAL_CONF,
                                 IFUSE_FCT_PAIR_INITIAL_CONF);
}

void fct_reinforce_ld2_candidate_for_ld1(Addr ld1_pc_addr, Addr ld2_pc_addr,
                                         unsigned int offset_delta,
                                         bool direction,
                                         unsigned int ld2_mem_size) {
    FCT_Row* row;
    int      slot_idx;

    if (!fct_is_initialized || ld1_pc_addr == 0 || ld2_pc_addr == 0) {
        return;
    }

    row = fct_lookup_row(ld1_pc_addr);
    if (!row || row->ld2_pc_addr != ld2_pc_addr ||
        row->ld2_mem_size != ld2_mem_size) {
        return;
    }

    slot_idx = fct_find_delta_slot(row, offset_delta, direction);
    if (slot_idx < 0) {
        return;
    }

    fct_increment_delta_slot_confidence(&row->delta_slots[slot_idx], false);
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
    FCT_Row* row;

    if (!fct_is_initialized) {
        fct_init();
    }
    if (ld1_pc_addr == 0 || ld2_pc_addr == 0 || offset_delta > 63U) {
        return;
    }

    row = fct_lookup_row(ld1_pc_addr);
    if (!row) {
        row = fct_allocate_row_for_load1_pc(ld1_pc_addr);
        fct_write_new_row_with_delta(row, ld1_pc_addr, ld2_pc_addr,
                                     ld1_effective_addr, ld2_effective_addr,
                                     offset_delta, direction, ld2_mem_size,
                                     ld1_micro_op_num, ld2_micro_op_num,
                                     fct_promoted_delta_confidence(),
                                     fct_promoted_pair_confidence());
        return;
    }

    if (row->ld2_pc_addr != ld2_pc_addr ||
        row->ld2_mem_size != ld2_mem_size) {
        STAT_EVENT(proc_id, FCT_LD2_REPLACEMENTS);
        fct_write_new_row_with_delta(row, ld1_pc_addr, ld2_pc_addr,
                                     ld1_effective_addr, ld2_effective_addr,
                                     offset_delta, direction, ld2_mem_size,
                                     ld1_micro_op_num, ld2_micro_op_num,
                                     fct_promoted_delta_confidence(),
                                     fct_promoted_pair_confidence());
        return;
    }

    fct_install_or_reinforce_delta(row, offset_delta, direction,
                                   fct_promoted_delta_confidence(), proc_id,
                                   true);
}
