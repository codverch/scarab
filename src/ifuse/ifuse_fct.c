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
 * Simulator state is maintained in a large open-addressed hash table with
 * 2^IFUSE_IDEAL_FCT_DEFAULT_HASH_BITS entries.
 */

static FCT_Row* fct_rows = NULL; // Software backing rows for the ideal FCT
static size_t   fct_num_hash_table_rows = 0;
static bool     fct_is_initialized = false;
static uint64_t fct_live_row_count = 0;
static uint64_t fct_live_row_peak = 0;

static void fct_note_new_live_row(unsigned int proc_id) {
    fct_live_row_count++;
    if (fct_live_row_count > fct_live_row_peak) {
        INC_STAT_EVENT(proc_id, FCT_PEAK_LIVE,
                       fct_live_row_count - fct_live_row_peak);
        fct_live_row_peak = fct_live_row_count;
    }
}

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

/**
 * Returns the first software hash-table slot to probe for ld1_pc_addr.
 *
 * The ideal FCT is keyed by the full LD1 PC. This hash is only for simulator
 * lookup speed; it is not a modeled hardware index.
 */
static size_t fct_get_probe_start_idx(Addr ld1_pc_addr, size_t row_index_mask) {
    uint64_t h = (uint64_t)ld1_pc_addr;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (size_t)(h & row_index_mask);
}

/**
 * Returns the configured FCT confidence cap.
 *
 * The runtime policy treats the prediction threshold as the maximum useful
 * confidence. A row inserted at confidence 100 should not grow to 500 and keep
 * predicting through many penalties; once it reaches the prediction threshold,
 * it is fully trusted until a misprediction lowers it.
 */
static unsigned int fct_max_confidence_score(void) {
    return IFUSE_FCT_CONF_THRESHOLD;
}

/**
 * Returns the confidence score after applying the configured confidence cap.
 */
static unsigned int fct_saturating_confidence_score(
    unsigned int confidence_score) {
    const unsigned int max_confidence_score = fct_max_confidence_score();
    return (confidence_score > max_confidence_score) ?
        max_confidence_score : confidence_score;
}

/**
 * Reinforces row without exceeding the configured prediction threshold.
 */
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

/**
 * Returns the row for ld1_pc_addr.
 */
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

/**
 * Returns the first empty backing row on ld1_pc_addr's probe chain.
 */
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

/**
 * Returns the FCT row for ld1_pc_addr, allocating a new ideal row if needed.
 */
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

        // First observations create a new row with low confidence; repeated
        // observations or training-table promotion make it predictive.
        row = fct_allocate_row_for_load1_pc(ld1_pc_addr);
        fct_write_row(row, ld1_pc_addr, ld2_pc_addr, ld1_effective_addr,
                      ld2_effective_addr, offset_delta, direction, ld2_mem_size,
                      ld1_micro_op_num, ld2_micro_op_num,
                      IFUSE_FCT_INITIAL_CONF);
        fct_note_new_live_row(proc_id);
        return;
    }

    if (row->ld2_pc_addr == ld2_pc_addr) {
        // Reinforce a stable LD1->LD2 candidate.
        fct_increment_confidence_score(row);
        return;
    }

    // Preserve the one-LD2-per-LD1 rule by replacing the older candidate.
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

    row = fct_allocate_row_for_load1_pc(ld1_pc_addr);
    fct_write_row(row, ld1_pc_addr, ld2_pc_addr, ld1_effective_addr,
                  ld2_effective_addr, offset_delta, direction, ld2_mem_size,
                  ld1_micro_op_num, ld2_micro_op_num,
                  IFUSE_FCT_INITIAL_CONF);
    fct_note_new_live_row(proc_id);
    return TRUE;
}
