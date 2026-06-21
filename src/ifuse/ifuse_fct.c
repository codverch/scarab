// STD headers
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Custom headers
#include "ifuse_fct.h"
#include "ifuse_plru.h"
#include "ifuse_set_stats.h"
#include "ifuse_training_table.h"
#include "../general.param.h"
#include "../statistics.h"
#include "ifuse_ideal_limits.h"
#include "ifuse.param.h"

/**
 * Fusion Candidate Table (FCT) runtime policy.
 *
 * The FCT models one LD2 candidate and up to four offset deltas for each LD1 PC.
 * Retire-time training can insert entries either on the first observation of an
 * LD1-LD2 pair or after the training table validates the pair as frequent. If
 * the same LD1 is later observed with a different LD2, the existing entry is
 * updated with the new pairing.
 *
 * Ideal mode uses a large open-addressed hash table with
 * 2^IFUSE_IDEAL_FCT_DEFAULT_HASH_BITS entries. Realistic mode uses a
 * set-associative table keyed by LD1 PC with tree PLRU replacement per set.
 */

#define FCT_PC_BITS 48U
#define FCT_PC_MASK 0xFFFFFFFFFFFFULL

static FCT_Row* fct_rows = NULL;
static uint8_t*   fct_plru = NULL;
static size_t     fct_num_hash_table_rows = 0;
static unsigned int fct_num_sets = 0;
static unsigned int fct_num_ways = 0;
static unsigned int fct_num_entries = 0;
static bool       fct_is_initialized = false;
static uint64_t   fct_delta_update_timestamp = 0;
static uint64_t   fct_live_row_count = 0;
static uint64_t   fct_live_row_peak = 0;
static IfuseSetStats fct_set_stats;
static bool       fct_set_stats_registered = false;

#define FCT_DELTA_SELECT_HIGHEST_SCORE        0U
#define FCT_DELTA_SELECT_LOWEST_SLOT          1U
#define FCT_DELTA_SELECT_HIGHEST_SLOT         2U
#define FCT_DELTA_SELECT_MOST_RECENT_CORRECT  3U

#define FCT_DELTA_TIE_LOWEST_SLOT             0U
#define FCT_DELTA_TIE_MOST_RECENT_CORRECT     1U
#define FCT_DELTA_TIE_HIGHEST_SLOT            2U

static FCT_Row* fct_row_at(unsigned int set_idx, unsigned int way) {
    return &fct_rows[(set_idx * fct_num_ways) + way];
}

static unsigned int fct_get_set_live(unsigned int set_idx) {
    unsigned int live = 0U;

    for (unsigned int way = 0; way < fct_num_ways; way++) {
        if (fct_row_at(set_idx, way)->valid) {
            live++;
        }
    }

    return live;
}

static void fct_update_set_stats(unsigned int set_idx) {
    ifuse_set_stats_note_set_live(&fct_set_stats, 0, set_idx,
                                  fct_get_set_live(set_idx));
}

static void fct_note_set_eviction(unsigned int set_idx) {
    ifuse_set_stats_note_eviction(&fct_set_stats, 0, set_idx);
}

static void fct_set_stats_atexit(void) {
    fct_dump_set_stats();
}

static void fct_init_set_stats(void) {
    ifuse_set_stats_init(&fct_set_stats, fct_num_sets, fct_num_ways,
                         FCT_SET_PEAK_LIVE, FCT_HOT_SETS, FCT_CONFLICT_SETS,
                         "FCT");

    if (!fct_set_stats_registered) {
        atexit(fct_set_stats_atexit);
        fct_set_stats_registered = true;
    }
}

void fct_dump_set_stats(void) {
    if (!IFUSE_REALISTIC_FCT) {
        return;
    }

    ifuse_set_stats_dump(&fct_set_stats, "ifuse_fct_set_histo.out", "FCT",
                         fct_num_entries, fct_live_row_peak, "FCT");
}

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
        if (fct_num_ways != 4U && fct_num_ways != 8U) {
            fprintf(stderr,
                    "FCT: realistic set-associative mode requires "
                    "ifuse_fct_ways=4 or 8 (got %u)\n",
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
    } else {
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
        fct_init_set_stats();
    }

    fct_live_row_count = 0;
    fct_live_row_peak  = 0;
    fct_is_initialized = true;
    fct_delta_update_timestamp = 0;

    training_table_init();
}

static uint64_t fct_fold_pc(Addr pc_addr) {
    return (uint64_t)pc_addr & FCT_PC_MASK;
}

static uint64_t fct_xor_rotate_fold_set_key(Addr ld1_pc_addr) {
    uint64_t fold = fct_fold_pc(ld1_pc_addr);
    uint64_t mix  = fold ^ (fold << 17U) ^ (fold >> 15U);

    mix ^= mix >> 32U;
    mix ^= mix >> 16U;
    mix ^= mix >> 8U;

    return mix;
}

static unsigned int fct_get_set_index(Addr ld1_pc_addr) {
    return (unsigned int)(fct_xor_rotate_fold_set_key(ld1_pc_addr) &
                          (fct_num_sets - 1U));
}

static uint64_t fct_get_ld1_tag(Addr ld1_pc_addr) {
    return fct_fold_pc(ld1_pc_addr);
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
    slot->selection_penalty  = 0;
    slot->last_correct_timestamp = 0;
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
        if (slot->selection_penalty > 0U ||
            slot->confidence_score >= IFUSE_FCT_CONF_THRESHOLD) {
            return;
        }

        unsigned int new_confidence_score =
            slot->confidence_score + IFUSE_FCT_CORRECT_CONF_REWARD;
        if (new_confidence_score > IFUSE_FCT_CONF_THRESHOLD) {
            new_confidence_score = IFUSE_FCT_CONF_THRESHOLD;
        }
        slot->confidence_score = new_confidence_score;
        return;
    }

    slot->confidence_score =
        fct_saturating_confidence_score(
            slot->confidence_score + IFUSE_FCT_CORRECT_CONF_REWARD);
    fct_reward_delta_slot_selection(slot);
    slot->last_correct_timestamp = ++fct_delta_update_timestamp;
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

static bool fct_delta_slot_is_selectable(const FCT_DeltaSlot* slot) {
    return slot->valid &&
        slot->confidence_score >= IFUSE_FCT_CONF_THRESHOLD;
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
    row->ld1_tag            = fct_get_ld1_tag(ld1_pc_addr);
    row->ld2_pc_addr        = ld2_pc_addr;
    row->ld1_effective_addr = ld1_effective_addr;
    row->ld2_effective_addr = ld2_effective_addr;
    row->ld2_mem_size       = ld2_mem_size;
    row->ld1_micro_op_num   = ld1_micro_op_num;
    row->ld2_micro_op_num   = ld2_micro_op_num;
    row->valid              = true;
    row->fusion_cooldown_until_load_num = 0;
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
                                             unsigned int proc_id) {
    int existing_slot_idx =
        fct_find_delta_slot(row, offset_delta, direction);
    if (existing_slot_idx >= 0) {
        fct_increment_delta_slot_confidence(
            &row->delta_slots[existing_slot_idx], true);
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
                        IFUSE_FCT_CONF_THRESHOLD);
    fct_penalize_delta_slot_selection_on_offset_mispred(
        &row->delta_slots[target_slot_idx]);
}

int fct_select_delta_slot(const FCT_Row* row) {
    if (!row || !row->valid) {
        return -1;
    }

    int best_slot_idx = -1;
    unsigned int best_score = 0;

    for (unsigned int slot_idx = 0; slot_idx < FCT_NUM_DELTA_SLOTS; slot_idx++) {
        const FCT_DeltaSlot* slot = &row->delta_slots[slot_idx];
        if (!fct_delta_slot_is_selectable(slot)) {
            continue;
        }

        if (IFUSE_FCT_DELTA_SELECT_POLICY == FCT_DELTA_SELECT_LOWEST_SLOT) {
            return (int)slot_idx;
        }

        const unsigned int slot_score =
            fct_delta_slot_selection_score(slot);
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

static FCT_Row* fct_lookup_row_ideal(Addr ld1_pc_addr) {
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

static FCT_Row* fct_lookup_row_realistic(Addr ld1_pc_addr) {
    if (!fct_rows || fct_num_sets == 0U) {
        return NULL;
    }

    unsigned int set_idx = fct_get_set_index(ld1_pc_addr);
    uint64_t     ld1_tag = fct_get_ld1_tag(ld1_pc_addr);

    for (unsigned int way = 0; way < fct_num_ways; way++) {
        FCT_Row* row = fct_row_at(set_idx, way);
        if (row->valid && row->ld1_tag == ld1_tag) {
            ifuse_plru_touch(fct_plru, set_idx, way, fct_num_ways);
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

    for (size_t num_probes = 0; num_probes < fct_num_hash_table_rows; num_probes++) {
        FCT_Row* row = &fct_rows[row_idx];
        if (!row->valid) {
            return row;
        }
        row_idx = (row_idx + 1U) & row_index_mask;
    }

    return NULL;
}

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
        ifuse_plru_touch(fct_plru, set_idx, invalid_way, fct_num_ways);
        fct_note_new_live_row(proc_id);
        fct_update_set_stats(set_idx);
        return row;
    }

    unsigned int victim_way =
        ifuse_plru_victim(fct_plru, set_idx, fct_num_ways);
    FCT_Row* victim = fct_row_at(set_idx, victim_way);
    fct_clear_delta_slots(victim);
    victim->fusion_cooldown_until_load_num = 0;
    victim->valid = false;
    fct_note_live_remove();
    fct_note_set_eviction(set_idx);
    STAT_EVENT(proc_id, FCT_EVICTED);
    ifuse_plru_touch(fct_plru, set_idx, victim_way, fct_num_ways);
    fct_update_set_stats(set_idx);
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

void fct_note_mispred_fusion_cooldown(Addr ld1_pc_addr,
                                      uint64_t current_load_num,
                                      unsigned int proc_id) {
    unsigned int cooldown_loads = IFUSE_FUSION_MISPRED_COOLDOWN_LOADS;

    if (!fct_is_initialized || ld1_pc_addr == 0 || cooldown_loads == 0U) {
        return;
    }

    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (!row) {
        return;
    }

    uint64_t cooldown_until = current_load_num + cooldown_loads;
    if (cooldown_until > row->fusion_cooldown_until_load_num) {
        row->fusion_cooldown_until_load_num = cooldown_until;
    }
    (void)proc_id;
}

Flag fct_is_fusion_gated_by_cooldown(Addr ld1_pc_addr,
                                     uint64_t current_load_num,
                                     unsigned int proc_id) {
    if (!fct_is_initialized || ld1_pc_addr == 0 ||
        IFUSE_FUSION_MISPRED_COOLDOWN_LOADS == 0U) {
        return FALSE;
    }

    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (!row || current_load_num >= row->fusion_cooldown_until_load_num) {
        return FALSE;
    }

    STAT_EVENT(proc_id, IFUSE_FUSION_GATED_MISPRED_COOLDOWN);
    return TRUE;
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
        fct_increment_delta_slot_confidence(slot, true);
        return;
    }

    fct_penalize_delta_slot_selection(slot);
    fct_penalize_delta_slot_confidence(slot);
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
        fct_penalize_delta_slot_selection_on_offset_mispred(
            mispredicted_slot);
        if (wrong_delta_in_mispredicted_slot) {
            mispredicted_slot->confidence_score = 0U;
            mispredicted_slot->selection_penalty =
                fct_max_confidence_score();
        } else {
            fct_penalize_delta_slot_confidence(mispredicted_slot);
        }
        fct_increment_delta_slot_confidence(
            &row->delta_slots[observed_slot_idx], true);
        return;
    }

    if (observed_slot_idx < 0) {
        STAT_EVENT(proc_id, FCT_OFFSET_MISPRED_UNTRACKED_DELTA);

        if (mispredicted_slot->valid) {
            fct_penalize_delta_slot_selection_on_offset_mispred(
                mispredicted_slot);
            if (wrong_delta_in_mispredicted_slot) {
                mispredicted_slot->confidence_score = 0U;
                mispredicted_slot->selection_penalty =
                    fct_max_confidence_score();
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
                                        IFUSE_FCT_CONF_THRESHOLD);
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
                            observed_direction, IFUSE_FCT_CONF_THRESHOLD);
        fct_increment_delta_slot_confidence(observed_slot, true);
        return;
    }

    STAT_EVENT(proc_id, FCT_OFFSET_MISPRED_SAME_SLOT_DELTA);

    if (mispredicted_slot->valid) {
        fct_penalize_delta_slot_selection_on_offset_mispred(
            mispredicted_slot);
        fct_penalize_delta_slot_confidence(mispredicted_slot);
    }

    fct_install_validated_delta_slot(row, observed_offset_delta,
                                     observed_direction, proc_id);
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
        row = fct_allocate_row_for_load1_pc(ld1_pc_addr, proc_id);
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
