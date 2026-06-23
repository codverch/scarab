// STD headers
#include <stdbool.h>
#include <stdint.h>

// Custom headers
#include "globals/global_defs.h"
#include "globals/global_types.h"

#ifndef IFUSE_FCT_H
#define IFUSE_FCT_H

#define FCT_NUM_DELTA_SLOTS 4
#define FCT_INVALID_DELTA_SLOT_IDX 0xFFFFFFFFU

/**
 * Fusion Candidate Table (FCT)
 * ============================
 * Each LD1 PC maps to one LD2 candidate and up to four cache-line offset
 * deltas. The row keeps pair-fusion confidence (LD1 fuses with this LD2 PC)
 * capped at IFUSE_FCT_PAIR_MAX_CONF (default 100). Promotion installs pair
 * confidence at that level; mispredictions decrement by
 * IFUSE_FCT_PAIR_MISPRED_CONF_PENALTY without increasing on correct
 * predictions. Offset-delta confidence is tracked separately per slot.
 */

typedef struct FCT_DeltaSlot {
    unsigned int offset_delta;
    bool         direction;
    unsigned int confidence_score;
    uint64_t     last_correct_timestamp;
    bool         valid;
} FCT_DeltaSlot;

/**
 * One FCT row contains one LD2 candidate and up to four offset deltas for a
 * single LD1 PC.
 */
typedef struct FCT_Row {
    Addr         ld1_pc_addr;
    Addr         ld2_pc_addr;
    Addr         ld1_effective_addr;
    Addr         ld2_effective_addr;
    unsigned int ld2_mem_size;
    unsigned int ld1_micro_op_num;
    unsigned int ld2_micro_op_num;
    unsigned int pair_confidence_score;
    bool         valid;
    FCT_DeltaSlot delta_slots[FCT_NUM_DELTA_SLOTS];
} FCT_Row;

void fct_init(void);

FCT_Row* fct_lookup(Addr ld1_pc_addr);

/**
 * Returns the highest-confidence delta slot whose confidence meets the
 * prediction threshold, or -1 if none qualify.
 */
int fct_select_delta_slot(const FCT_Row* row, Addr ld1_effective_addr);

/**
 * Returns TRUE when the LD1-to-LD2 pair confidence meets the fusion threshold.
 */
Flag fct_row_is_fusible(const FCT_Row* row);

void fct_update_pair_confidence(Addr ld1_pc_addr, bool prediction_correct);

void fct_update_delta_confidence(Addr ld1_pc_addr, unsigned int slot_idx,
                                 Addr ld1_effective_addr,
                                 bool prediction_correct);

void fct_update_delta_confidence_on_offset_misprediction(
    Addr ld1_pc_addr,
    unsigned int mispredicted_slot_idx,
    Addr ld1_effective_addr,
    Addr ld2_effective_addr,
    unsigned int proc_id);

Flag fct_has_load1_pc_entry(Addr ld1_pc_addr);

void fct_update_ld2_candidate_for_ld1(Addr ld1_pc_addr, Addr ld2_pc_addr,
                                      Addr ld1_effective_addr,
                                      Addr ld2_effective_addr,
                                      unsigned int offset_delta, bool direction,
                                      unsigned int ld2_mem_size,
                                      unsigned int ld1_micro_op_num,
                                      unsigned int ld2_micro_op_num,
                                      unsigned int proc_id);

void fct_reinforce_ld2_candidate_for_ld1(Addr ld1_pc_addr, Addr ld2_pc_addr,
                                         unsigned int offset_delta,
                                         bool direction,
                                         unsigned int ld2_mem_size);

void fct_promote_ld2_candidate_for_ld1(Addr ld1_pc_addr, Addr ld2_pc_addr,
                                       Addr ld1_effective_addr,
                                       Addr ld2_effective_addr,
                                       unsigned int offset_delta,
                                       bool direction,
                                       unsigned int ld2_mem_size,
                                       unsigned int ld1_micro_op_num,
                                       unsigned int ld2_micro_op_num,
                                       unsigned int proc_id);

/**
 * Returns the training-table observation count required before this pattern
 * is promoted into the FCT. The first offset delta for an LD1-LD2 row uses
 * IFUSE_TRAINING_INSERT_THRESHOLD; additional deltas use
 * IFUSE_TRAINING_SECONDARY_INSERT_THRESHOLD.
 */
unsigned int fct_get_training_insert_threshold(Addr ld1_pc_addr,
                                               Addr ld2_pc_addr,
                                               unsigned int offset_delta,
                                               bool direction,
                                               unsigned int ld2_mem_size);

#endif /* IFUSE_FCT_H */
