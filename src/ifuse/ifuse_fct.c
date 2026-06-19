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
 * The ideal FCT models one LD2 candidate and up to four offset deltas for each
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
static uint64_t fct_delta_update_timestamp = 0;

#define FCT_DELTA_SELECT_HIGHEST_SCORE        0U
#define FCT_DELTA_SELECT_LOWEST_SLOT          1U
#define FCT_DELTA_SELECT_HIGHEST_SLOT         2U
#define FCT_DELTA_SELECT_MOST_RECENT_CORRECT  3U

#define FCT_DELTA_TIE_LOWEST_SLOT             0U
#define FCT_DELTA_TIE_MOST_RECENT_CORRECT     1U
#define FCT_DELTA_TIE_HIGHEST_SLOT            2U

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

static unsigned int fct_min_frontend_corrects_required(void);
static unsigned int fct_retire_bootstrap_confidence_cap(void);
static bool fct_delta_slot_in_probation(const FCT_DeltaSlot* slot);
static bool fct_delta_slot_is_selectable(const FCT_DeltaSlot* slot);

static unsigned int fct_context_bucket(Addr ld1_effective_addr) {
    return (unsigned int)((ld1_effective_addr & 63U) >> 3);
}

static void fct_clear_delta_slots(FCT_Row* row) {
    memset(row->delta_slots, 0, sizeof(row->delta_slots));
}

static void fct_init_delta_slot(FCT_DeltaSlot* slot,
                                unsigned int offset_delta,
                                bool direction,
                                unsigned int confidence_score) {
    memset(slot, 0, sizeof(*slot));
    slot->offset_delta       = offset_delta;
    slot->direction          = direction;
    slot->confidence_score   = fct_saturating_confidence_score(confidence_score);
    slot->valid              = true;
}

static unsigned int fct_delta_slot_selection_score(
    const FCT_DeltaSlot* slot) {
    if (!slot || !slot->valid ||
        slot->confidence_score <= slot->selection_penalty) {
        return 0U;
    }
    return slot->confidence_score - slot->selection_penalty;
}

static unsigned int fct_delta_slot_usefulness_samples(
    const FCT_DeltaSlot* slot) {
    if (!slot || !slot->valid) {
        return 0U;
    }

    if (slot->useful_wake_count >
        0xFFFFFFFFU - slot->useless_wake_count) {
        return 0xFFFFFFFFU;
    }
    return slot->useful_wake_count + slot->useless_wake_count;
}

static bool fct_delta_slot_is_usefulness_gated(
    const FCT_DeltaSlot* slot) {
    if (!IFUSE_FCT_USEFULNESS_GATE_ENABLED || !slot || !slot->valid) {
        return false;
    }

    const unsigned int min_samples =
        IFUSE_FCT_USEFULNESS_GATE_MIN_SAMPLES;
    const unsigned int samples =
        fct_delta_slot_usefulness_samples(slot);
    if (min_samples == 0U || samples < min_samples) {
        return false;
    }

    unsigned int max_useless_pct =
        IFUSE_FCT_USEFULNESS_GATE_MAX_USELESS_PCT;
    if (max_useless_pct > 100U) {
        max_useless_pct = 100U;
    }

    return slot->useless_wake_count * 100U >
           samples * max_useless_pct;
}

static int fct_delta_slot_usefulness_score(
    const FCT_DeltaSlot* slot) {
    if (!IFUSE_FCT_USEFULNESS_GATE_ENABLED || !slot || !slot->valid) {
        return 0;
    }

    int score = 0;
    score += (int)(IFUSE_FCT_USEFULNESS_REWARD_WEIGHT *
                   slot->useful_wake_count);
    score -= (int)(IFUSE_FCT_USEFULNESS_PENALTY_WEIGHT *
                   slot->useless_wake_count);
    return score;
}

static int fct_delta_slot_effective_selection_score(
    const FCT_DeltaSlot* slot) {
    return (int)fct_delta_slot_selection_score(slot) +
           fct_delta_slot_usefulness_score(slot);
}

static int fct_delta_slot_contextual_selection_score(
    const FCT_DeltaSlot* slot,
    unsigned int context_bucket) {
    int score = fct_delta_slot_effective_selection_score(slot);
    score += (int)(IFUSE_FCT_CONTEXTUAL_DELTA_CORRECT_WEIGHT *
                   slot->context_correct[context_bucket]);
    score -= (int)(IFUSE_FCT_CONTEXTUAL_DELTA_WRONG_WEIGHT *
                   slot->context_wrong[context_bucket]);
    return score;
}

static void fct_penalize_delta_slot_selection(FCT_DeltaSlot* slot) {
    if (!slot || !slot->valid) {
        return;
    }

    unsigned int penalty = IFUSE_FCT_DELTA_SELECT_MISPRED_PENALTY;
    if (penalty == 0U) {
        return;
    }

    const unsigned int max_penalty = fct_max_confidence_score();
    slot->selection_penalty =
        (penalty >= max_penalty ||
         slot->selection_penalty > max_penalty - penalty) ?
        max_penalty : slot->selection_penalty + penalty;
}

static void fct_penalize_delta_slot_selection_on_offset_mispred(
    FCT_DeltaSlot* slot) {
    if (!slot || !slot->valid) {
        return;
    }

    unsigned int penalty = IFUSE_FCT_DELTA_SELECT_MISPRED_PENALTY;
    if (penalty == 0U) {
        penalty = IFUSE_FCT_MISPRED_CONF_PENALTY;
    }
    if (penalty == 0U) {
        return;
    }

    const unsigned int max_penalty = fct_max_confidence_score();
    slot->selection_penalty =
        (penalty >= max_penalty ||
         slot->selection_penalty > max_penalty - penalty) ?
        max_penalty : slot->selection_penalty + penalty;
}

static void fct_reward_delta_slot_selection(FCT_DeltaSlot* slot) {
    if (!slot || !slot->valid ||
        IFUSE_FCT_DELTA_SELECT_CORRECT_REWARD == 0U) {
        return;
    }

    const unsigned int reward = IFUSE_FCT_DELTA_SELECT_CORRECT_REWARD;
    slot->selection_penalty =
        (slot->selection_penalty <= reward) ?
        0U : slot->selection_penalty - reward;
}

static void fct_increment_delta_slot_confidence(FCT_DeltaSlot* slot,
                                                bool mark_correct) {
    if (!slot || !slot->valid) {
        return;
    }

    if (!mark_correct) {
        /*
         * Retire-time observations only bootstrap a slot to the prediction
         * threshold. They must not keep inflating ranking confidence once the
         * slot is already eligible, otherwise the first-installed delta (slot
         * 0) dominates selection even when another delta is more accurate.
         * Also skip retire bootstrap while a frontend selection penalty is
         * active so training does not immediately override a misprediction.
         */
        const unsigned int bootstrap_cap =
            fct_retire_bootstrap_confidence_cap();
        if (slot->selection_penalty > 0U ||
            slot->confidence_score >= bootstrap_cap) {
            return;
        }

        unsigned int new_confidence_score =
            slot->confidence_score + IFUSE_FCT_CORRECT_CONF_REWARD;
        if (new_confidence_score > bootstrap_cap) {
            new_confidence_score = bootstrap_cap;
        }
        slot->confidence_score = new_confidence_score;
        return;
    }

    slot->confidence_score =
        fct_saturating_confidence_score(
            slot->confidence_score + IFUSE_FCT_CORRECT_CONF_REWARD);
    fct_reward_delta_slot_selection(slot);
    if (slot->correct_streak < 0xFFFFFFFFU) {
        slot->correct_streak++;
    }
    slot->last_correct_timestamp = ++fct_delta_update_timestamp;
}

static void fct_record_delta_slot_context_outcome(FCT_Row* row,
                                                  unsigned int slot_idx,
                                                  Addr ld1_effective_addr,
                                                  bool prediction_correct) {
    if (!row || slot_idx >= FCT_NUM_DELTA_SLOTS ||
        !row->delta_slots[slot_idx].valid) {
        return;
    }

    FCT_DeltaSlot* slot = &row->delta_slots[slot_idx];
    unsigned int bucket = fct_context_bucket(ld1_effective_addr);
    unsigned int max_counter = fct_max_confidence_score();

    if (prediction_correct) {
        if (slot->context_correct[bucket] < max_counter) {
            slot->context_correct[bucket]++;
        }
        if (slot->context_wrong[bucket] > 0U) {
            slot->context_wrong[bucket]--;
        }
        return;
    }

    if (slot->context_wrong[bucket] < max_counter) {
        slot->context_wrong[bucket]++;
    }
    if (slot->context_correct[bucket] > 0U) {
        slot->context_correct[bucket]--;
    }
}

static void fct_record_correct_delta_slot(FCT_Row* row,
                                          unsigned int slot_idx) {
    if (!row || slot_idx >= FCT_NUM_DELTA_SLOTS ||
        !row->delta_slots[slot_idx].valid) {
        return;
    }

    if (!IFUSE_FCT_DOMINANT_DELTA_ENABLED) {
        return;
    }

    if (row->primary_delta_slot_idx >= FCT_NUM_DELTA_SLOTS ||
        !row->delta_slots[row->primary_delta_slot_idx].valid) {
        row->primary_delta_slot_idx = slot_idx;
        row->dominant_delta_mode = true;
        row->recent_primary_offset_mispreds = 0;
        return;
    }

    if (slot_idx == row->primary_delta_slot_idx) {
        row->dominant_delta_mode = true;
        row->recent_primary_offset_mispreds = 0;
        return;
    }

    unsigned int promote_corrects =
        IFUSE_FCT_DOMINANT_DELTA_ALT_PROMOTE_CORRECTS;
    if (promote_corrects == 0U) {
        promote_corrects = 1U;
    }

    if (row->delta_slots[slot_idx].correct_streak >= promote_corrects) {
        row->primary_delta_slot_idx = slot_idx;
        row->dominant_delta_mode = true;
        row->recent_primary_offset_mispreds = 0;
    }
}

static void fct_record_wrong_delta_slot(FCT_Row* row,
                                        unsigned int slot_idx,
                                        bool offset_specific) {
    if (!row || slot_idx >= FCT_NUM_DELTA_SLOTS) {
        return;
    }

    row->delta_slots[slot_idx].correct_streak = 0;

    if (!IFUSE_FCT_DOMINANT_DELTA_ENABLED ||
        !offset_specific ||
        slot_idx != row->primary_delta_slot_idx) {
        return;
    }

    if (row->recent_primary_offset_mispreds < 0xFFFFFFFFU) {
        row->recent_primary_offset_mispreds++;
    }

    unsigned int mispred_threshold =
        IFUSE_FCT_DOMINANT_DELTA_MISPRED_THRESHOLD;
    if (mispred_threshold == 0U) {
        mispred_threshold = 1U;
    }

    if (row->recent_primary_offset_mispreds >= mispred_threshold) {
        row->dominant_delta_mode = false;
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

static void fct_penalize_delta_slot_confidence(FCT_DeltaSlot* slot) {
    if (!slot || !slot->valid) {
        return;
    }

    const unsigned int confidence_penalty = IFUSE_FCT_MISPRED_CONF_PENALTY;
    slot->confidence_score =
        (slot->confidence_score <= confidence_penalty) ?
        0U : slot->confidence_score - confidence_penalty;
    slot->correct_streak = 0;
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

static int fct_find_weakest_delta_slot_excluding(const FCT_Row* row,
                                                 int exclude_slot_idx) {
    int weakest_slot_idx = -1;
    unsigned int weakest_confidence = 0;

    for (unsigned int slot_idx = 0; slot_idx < FCT_NUM_DELTA_SLOTS; slot_idx++) {
        if ((int)slot_idx == exclude_slot_idx) {
            continue;
        }

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

static bool fct_delta_tie_prefers_candidate(const FCT_DeltaSlot* candidate,
                                            unsigned int candidate_idx,
                                            const FCT_DeltaSlot* best,
                                            unsigned int best_idx) {
    switch (IFUSE_FCT_DELTA_TIE_POLICY) {
    case FCT_DELTA_TIE_LOWEST_SLOT:
        return candidate_idx < best_idx;
    case FCT_DELTA_TIE_HIGHEST_SLOT:
        return candidate_idx > best_idx;
    case FCT_DELTA_TIE_MOST_RECENT_CORRECT:
    default:
        if (candidate->last_correct_timestamp !=
            best->last_correct_timestamp) {
            return candidate->last_correct_timestamp >
                best->last_correct_timestamp;
        }
        return candidate_idx < best_idx;
    }
}

static unsigned int fct_min_frontend_corrects_required(void) {
    unsigned int min_corrects = IFUSE_FCT_MIN_FRONTEND_CORRECTS;
    return (min_corrects == 0U) ? 1U : min_corrects;
}

static unsigned int fct_retire_bootstrap_confidence_cap(void) {
    if (IFUSE_FCT_CONF_THRESHOLD == 0U) {
        return 0U;
    }

    unsigned int cap = IFUSE_FCT_CONF_THRESHOLD;
    if (cap > IFUSE_FCT_CORRECT_CONF_REWARD) {
        cap -= IFUSE_FCT_CORRECT_CONF_REWARD;
    } else if (cap > 0U) {
        cap -= 1U;
    }
    if (cap < IFUSE_FCT_INITIAL_CONF) {
        cap = IFUSE_FCT_INITIAL_CONF;
    }
    return cap;
}

static bool fct_delta_slot_in_probation(const FCT_DeltaSlot* slot) {
    if (!slot || !slot->valid || slot->selection_penalty > 0U ||
        slot->correct_streak > 0U || slot->last_correct_timestamp > 0U) {
        return false;
    }

    const unsigned int bootstrap_cap =
        fct_retire_bootstrap_confidence_cap();
    return slot->confidence_score >= bootstrap_cap &&
        slot->confidence_score < IFUSE_FCT_CONF_THRESHOLD;
}

static bool fct_delta_slot_is_selectable(const FCT_DeltaSlot* slot) {
    if (!slot || !slot->valid) {
        return false;
    }

    if (slot->confidence_score <= slot->selection_penalty) {
        return false;
    }

    if (fct_delta_slot_is_usefulness_gated(slot)) {
        return false;
    }

    const unsigned int min_corrects = fct_min_frontend_corrects_required();
    if (slot->confidence_score >= IFUSE_FCT_CONF_THRESHOLD &&
        slot->correct_streak >= min_corrects) {
        return true;
    }

    /*
     * After the first frontend correct, keep fusing while penalty-free so
     * min_frontend_corrects can be reached without a deadlock.
     */
    if (slot->confidence_score >= IFUSE_FCT_CONF_THRESHOLD &&
        slot->correct_streak > 0U &&
        slot->selection_penalty == 0U) {
        return true;
    }

    return fct_delta_slot_in_probation(slot);
}

static unsigned int fct_count_selectable_delta_slots(const FCT_Row* row) {
    unsigned int num_selectable_slots = 0;

    if (!row || !row->valid) {
        return 0U;
    }

    for (unsigned int slot_idx = 0; slot_idx < FCT_NUM_DELTA_SLOTS; slot_idx++) {
        if (fct_delta_slot_is_selectable(&row->delta_slots[slot_idx])) {
            num_selectable_slots++;
        }
    }

    return num_selectable_slots;
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

static void fct_reset_row_dominance(FCT_Row* row,
                                    unsigned int primary_delta_slot_idx) {
    if (!row) {
        return;
    }

    row->dominant_delta_mode = IFUSE_FCT_DOMINANT_DELTA_ENABLED ? true : false;
    row->primary_delta_slot_idx = primary_delta_slot_idx;
    row->recent_primary_offset_mispreds = 0;
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
    fct_reset_row_dominance(row, 0);
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

static void fct_install_validated_delta_slot(FCT_Row* row,
                                             unsigned int offset_delta,
                                             bool direction,
                                             Addr ld1_effective_addr,
                                             unsigned int proc_id) {
    int existing_slot_idx =
        fct_find_delta_slot(row, offset_delta, direction);
    if (existing_slot_idx >= 0) {
        fct_increment_delta_slot_confidence(
            &row->delta_slots[existing_slot_idx], true);
        fct_record_delta_slot_context_outcome(row,
                                              (unsigned int)existing_slot_idx,
                                              ld1_effective_addr, true);
        fct_record_correct_delta_slot(row, (unsigned int)existing_slot_idx);
        return;
    }

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

    fct_init_delta_slot(&row->delta_slots[target_slot_idx],
                        offset_delta, direction,
                        IFUSE_FCT_INITIAL_CONF);
    fct_record_delta_slot_context_outcome(row, (unsigned int)target_slot_idx,
                                          ld1_effective_addr, true);
    fct_penalize_delta_slot_selection_on_offset_mispred(
        &row->delta_slots[target_slot_idx]);
}

int fct_select_delta_slot(const FCT_Row* row, Addr ld1_effective_addr) {
    if (!row || !row->valid) {
        return -1;
    }

    if (IFUSE_FCT_CONTEXTUAL_DELTA_ENABLED) {
        int best_slot_idx = -1;
        int best_score = 0;
        const unsigned int context_bucket =
            fct_context_bucket(ld1_effective_addr);

        for (unsigned int slot_idx = 0;
             slot_idx < FCT_NUM_DELTA_SLOTS;
             slot_idx++) {
            const FCT_DeltaSlot* slot = &row->delta_slots[slot_idx];
            if (!fct_delta_slot_is_selectable(slot)) {
                continue;
            }

            const int slot_score =
                fct_delta_slot_contextual_selection_score(slot,
                                                          context_bucket);
            if (best_slot_idx < 0) {
                best_slot_idx = (int)slot_idx;
                best_score = slot_score;
                continue;
            }

            const FCT_DeltaSlot* best_slot =
                &row->delta_slots[best_slot_idx];
            if (slot_score > best_score ||
                (slot_score == best_score &&
                 fct_delta_tie_prefers_candidate(
                     slot, slot_idx, best_slot,
                     (unsigned int)best_slot_idx))) {
                best_slot_idx = (int)slot_idx;
                best_score = slot_score;
            }
        }

        return best_slot_idx;
    }

    if (IFUSE_FCT_DOMINANT_DELTA_ENABLED &&
        row->dominant_delta_mode &&
        row->primary_delta_slot_idx < FCT_NUM_DELTA_SLOTS) {
        const FCT_DeltaSlot* primary_slot =
            &row->delta_slots[row->primary_delta_slot_idx];

        if (fct_delta_slot_is_selectable(primary_slot)) {
            const int primary_score =
                fct_delta_slot_effective_selection_score(primary_slot);
            int best_alt_score = 0;
            bool has_selectable_alt = false;

            for (unsigned int slot_idx = 0;
                 slot_idx < FCT_NUM_DELTA_SLOTS;
                 slot_idx++) {
                if (slot_idx == row->primary_delta_slot_idx) {
                    continue;
                }

                const FCT_DeltaSlot* slot = &row->delta_slots[slot_idx];
                if (!fct_delta_slot_is_selectable(slot)) {
                    continue;
                }

                has_selectable_alt = true;
                int slot_score =
                    fct_delta_slot_effective_selection_score(slot);
                if (slot_score > best_alt_score) {
                    best_alt_score = slot_score;
                }
            }

            if (!has_selectable_alt ||
                primary_score +
                    (int)IFUSE_FCT_DOMINANT_DELTA_SWITCH_MARGIN >=
                    best_alt_score) {
                return (int)row->primary_delta_slot_idx;
            }
        }
    }

    int best_slot_idx = -1;
    int best_score = 0;

    for (unsigned int slot_idx = 0; slot_idx < FCT_NUM_DELTA_SLOTS; slot_idx++) {
        const FCT_DeltaSlot* slot = &row->delta_slots[slot_idx];
        if (!fct_delta_slot_is_selectable(slot)) {
            continue;
        }

        if (IFUSE_FCT_DELTA_SELECT_POLICY == FCT_DELTA_SELECT_LOWEST_SLOT) {
            return (int)slot_idx;
        }

        const int slot_score =
            fct_delta_slot_effective_selection_score(slot);
        if (best_slot_idx < 0) {
            best_slot_idx = (int)slot_idx;
            best_score = slot_score;
            continue;
        }

        const FCT_DeltaSlot* best_slot = &row->delta_slots[best_slot_idx];
        bool choose_candidate = false;

        switch (IFUSE_FCT_DELTA_SELECT_POLICY) {
        case FCT_DELTA_SELECT_HIGHEST_SLOT:
            choose_candidate = slot_idx > (unsigned int)best_slot_idx;
            break;
        case FCT_DELTA_SELECT_MOST_RECENT_CORRECT:
            choose_candidate =
                slot->last_correct_timestamp >
                    best_slot->last_correct_timestamp ||
                (slot->last_correct_timestamp ==
                    best_slot->last_correct_timestamp &&
                 (slot_score > best_score ||
                  (slot_score == best_score &&
                   fct_delta_tie_prefers_candidate(
                       slot, slot_idx, best_slot,
                       (unsigned int)best_slot_idx))));
            break;
        case FCT_DELTA_SELECT_HIGHEST_SCORE:
        default:
            choose_candidate =
                slot_score > best_score ||
                (slot_score == best_score &&
                 fct_delta_tie_prefers_candidate(
                     slot, slot_idx, best_slot,
                     (unsigned int)best_slot_idx));
            break;
        }

        if (choose_candidate) {
            best_slot_idx = (int)slot_idx;
            best_score = slot_score;
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
                                 Addr ld1_effective_addr,
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
        fct_increment_delta_slot_confidence(slot, true);
        fct_record_delta_slot_context_outcome(row, slot_idx,
                                              ld1_effective_addr, true);
        fct_record_correct_delta_slot(row, slot_idx);
        return;
    }

    fct_record_delta_slot_context_outcome(row, slot_idx,
                                          ld1_effective_addr, false);
    fct_record_wrong_delta_slot(row, slot_idx, false);
    fct_penalize_delta_slot_selection(slot);
    fct_penalize_delta_slot_confidence(slot);
}

void fct_update_delta_usefulness(Addr ld1_pc_addr, unsigned int slot_idx,
                                 unsigned int consumer_wakeups) {
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

    const unsigned int max_counter = 255U;
    if (consumer_wakeups > 0U) {
        if (slot->useful_wake_count < max_counter) {
            slot->useful_wake_count++;
        }
        if (slot->useless_wake_count > 0U) {
            slot->useless_wake_count--;
        }
        return;
    }

    if (slot->useless_wake_count < max_counter) {
        slot->useless_wake_count++;
    }
    if (slot->useful_wake_count > 0U) {
        slot->useful_wake_count--;
    }
}

void fct_update_delta_confidence_on_offset_misprediction(
    Addr ld1_pc_addr,
    unsigned int mispredicted_slot_idx,
    Addr ld1_effective_addr,
    Addr ld2_effective_addr,
    unsigned int proc_id) {
    if (!fct_is_initialized ||
        mispredicted_slot_idx >= FCT_NUM_DELTA_SLOTS) {
        return;
    }

    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (!row) {
        return;
    }

    unsigned int observed_offset_delta = 0;
    bool         observed_direction    = false;
    fct_offset_delta_from_addresses(ld1_effective_addr, ld2_effective_addr,
                                    &observed_offset_delta,
                                    &observed_direction);
    if (observed_offset_delta > 63U) {
        return;
    }

    FCT_DeltaSlot* mispredicted_slot =
        &row->delta_slots[mispredicted_slot_idx];
    bool wrong_delta_in_mispredicted_slot =
        mispredicted_slot->valid &&
        (mispredicted_slot->offset_delta != observed_offset_delta ||
         mispredicted_slot->direction != observed_direction);

    unsigned int num_selectable_slots =
        fct_count_selectable_delta_slots(row);
    if (num_selectable_slots > 1U) {
        STAT_EVENT(proc_id, FCT_OFFSET_MISPRED_MULTI_DELTA_ROW);
    } else {
        STAT_EVENT(proc_id, FCT_OFFSET_MISPRED_SINGLE_DELTA_ROW);
    }

    int observed_slot_idx = fct_find_delta_slot(
        row, observed_offset_delta, observed_direction);
    if (observed_slot_idx >= 0 &&
        (unsigned int)observed_slot_idx != mispredicted_slot_idx) {
        STAT_EVENT(proc_id, FCT_OFFSET_MISPRED_KNOWN_ALT_DELTA);
        fct_record_delta_slot_context_outcome(row, mispredicted_slot_idx,
                                              ld1_effective_addr, false);
        fct_record_wrong_delta_slot(row, mispredicted_slot_idx, true);
        fct_penalize_delta_slot_selection_on_offset_mispred(
            mispredicted_slot);
        if (wrong_delta_in_mispredicted_slot) {
            mispredicted_slot->confidence_score = 0U;
            mispredicted_slot->selection_penalty =
                fct_max_confidence_score();
            mispredicted_slot->correct_streak = 0;
        } else {
            fct_penalize_delta_slot_confidence(mispredicted_slot);
        }
        fct_increment_delta_slot_confidence(
            &row->delta_slots[observed_slot_idx], true);
        fct_record_delta_slot_context_outcome(row,
                                              (unsigned int)observed_slot_idx,
                                              ld1_effective_addr, true);
        fct_record_correct_delta_slot(row, (unsigned int)observed_slot_idx);
        return;
    }

    if (observed_slot_idx < 0) {
        STAT_EVENT(proc_id, FCT_OFFSET_MISPRED_UNTRACKED_DELTA);

        if (mispredicted_slot->valid) {
            fct_record_delta_slot_context_outcome(row, mispredicted_slot_idx,
                                                  ld1_effective_addr, false);
            fct_record_wrong_delta_slot(row, mispredicted_slot_idx, true);
            fct_penalize_delta_slot_selection_on_offset_mispred(
                mispredicted_slot);
            if (wrong_delta_in_mispredicted_slot) {
                mispredicted_slot->confidence_score = 0U;
                mispredicted_slot->selection_penalty =
                    fct_max_confidence_score();
                mispredicted_slot->correct_streak = 0;
            } else {
                fct_penalize_delta_slot_confidence(mispredicted_slot);
            }
        }

        int target_slot_idx = fct_find_empty_delta_slot(row);
        if (target_slot_idx < 0) {
            target_slot_idx =
                fct_find_weakest_delta_slot_excluding(row,
                                                      (int)mispredicted_slot_idx);
            if (target_slot_idx < 0) {
                if (wrong_delta_in_mispredicted_slot) {
                    fct_init_delta_slot(mispredicted_slot,
                                        observed_offset_delta,
                                        observed_direction,
                                        IFUSE_FCT_INITIAL_CONF);
                    fct_reset_row_dominance(row, mispredicted_slot_idx);
                }
                return;
            }
            STAT_EVENT(proc_id, FCT_DELTA_SLOT_REPLACEMENTS);
        } else if (target_slot_idx > 0) {
            fct_stat_delta_slot_promotion(proc_id, target_slot_idx);
        }

        FCT_DeltaSlot* observed_slot =
            &row->delta_slots[target_slot_idx];
        fct_init_delta_slot(observed_slot, observed_offset_delta,
                            observed_direction, IFUSE_FCT_INITIAL_CONF);
        fct_record_delta_slot_context_outcome(row, (unsigned int)target_slot_idx,
                                              ld1_effective_addr, true);
        fct_record_correct_delta_slot(row, (unsigned int)target_slot_idx);
        return;
    }

    STAT_EVENT(proc_id, FCT_OFFSET_MISPRED_SAME_SLOT_DELTA);

    if (mispredicted_slot->valid) {
        fct_record_delta_slot_context_outcome(row, mispredicted_slot_idx,
                                              ld1_effective_addr, false);
        fct_record_wrong_delta_slot(row, mispredicted_slot_idx, true);
        fct_penalize_delta_slot_selection_on_offset_mispred(
            mispredicted_slot);
        fct_penalize_delta_slot_confidence(mispredicted_slot);
    }

    fct_install_validated_delta_slot(row, observed_offset_delta,
                                     observed_direction, ld1_effective_addr,
                                     proc_id);
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
