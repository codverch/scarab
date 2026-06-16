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
 * The FCT predicts load fusion candidates by mapping each LD1 PC to a
 * corresponding LD2 candidate. The current implementation models an ideal
 * runtime structure and serves as the baseline prior to introducing realistic
 * capacity constraints.
 *
 * Each LD1 row stores up to four cache-line offset deltas for the same LD2 PC.
 * Fetch selects a delta slot according to the configured selection policy.
 * Retire-time observations only bootstrap a slot to the prediction
 * threshold. Frontend outcomes decide which eligible delta is chosen when
 * multiple candidates exist. Learned confidence gates eligibility; selection
 * penalties affect ranking and retire bootstrap blocking. Untracked offset
 * mispredictions install the observed delta into a new slot when possible so
 * the chooser can learn among alternates instead of overwriting the predicted
 * slot in place.
 */

typedef struct FCT_DeltaSlot {
    unsigned int offset_delta;
    bool         direction;
    unsigned int confidence_score;
    unsigned int selection_penalty;
    uint64_t     last_correct_timestamp;
    bool         valid;
} FCT_DeltaSlot;

/**
 * One FCT row contains one LD2 candidate and up to four offset deltas for a
 * single LD1 PC.
 */
typedef struct FCT_Row {
    // Load identification
    Addr         ld1_pc_addr;
    Addr         ld2_pc_addr;

    // Memory access information
    Addr         ld1_effective_addr;
    Addr         ld2_effective_addr;
    unsigned int ld2_mem_size;

    // Execution context
    unsigned int ld1_micro_op_num;
    unsigned int ld2_micro_op_num;

    // Prediction metadata
    bool              valid;
    FCT_DeltaSlot     delta_slots[FCT_NUM_DELTA_SLOTS];
} FCT_Row;

void fct_init(void);

/**
 * Looks up the FCT row for ld1_pc_addr.
 *
 * @param ld1_pc_addr The PC of the candidate first load.
 * @return The matching FCT row, or NULL if no row exists.
 */
FCT_Row* fct_lookup(Addr ld1_pc_addr);

/**
 * Returns the delta slot index selected by the configured policy. Slot
 * eligibility uses learned confidence; slot ranking uses effective selection
 * score, which is learned confidence minus any temporary selection penalty.
 *
 * @return A slot index in [0, FCT_NUM_DELTA_SLOTS), or -1 if none qualify.
 */
int fct_select_delta_slot(const FCT_Row* row);

/**
 * Updates the confidence score for one delta slot after a frontend prediction
 * resolves.
 *
 * @param ld1_pc_addr The PC of the predicted first load.
 * @param slot_idx The delta slot used for the prediction.
 * @param prediction_correct TRUE if the fused prediction was correct.
 */
void fct_update_delta_confidence(Addr ld1_pc_addr, unsigned int slot_idx,
                                 bool prediction_correct);

/**
 * Applies offset-delta-specific confidence feedback after a frontend mispred.
 *
 * The mispredicted slot is penalized. If the FCT already tracks the observed
 * offset delta in another slot, that slot is reinforced and marked as the most
 * recently correct choice for future selection.
 *
 * @param ld1_pc_addr The PC of the predicted first load.
 * @param mispredicted_slot_idx The delta slot used for the failed prediction.
 * @param ld1_effective_addr The effective address produced by LD1.
 * @param ld2_effective_addr The effective address produced by LD2.
 */
void fct_update_delta_confidence_on_offset_misprediction(
    Addr ld1_pc_addr,
    unsigned int mispredicted_slot_idx,
    Addr ld1_effective_addr,
    Addr ld2_effective_addr,
    unsigned int proc_id);

/**
 * TRUE if the FCT already holds a row for this load1 PC.
 *
 * @param ld1_pc_addr The PC of the candidate first load.
 * @return TRUE if a row exists, FALSE otherwise.
 */
Flag fct_has_load1_pc_entry(Addr ld1_pc_addr);

/**
 * Updates the LD2 candidate stored for a retired LD1.
 *
 * @param ld1_pc_addr The PC of the retired first load.
 * @param ld2_pc_addr The PC of the retired second load candidate.
 * @param ld1_effective_addr The effective address produced by LD1.
 * @param ld2_effective_addr The effective address produced by LD2.
 * @param offset_delta The absolute cache-line offset delta between LD1 and LD2.
 * @param direction TRUE if LD2's cache-line offset is greater than or equal to
 *        LD1's offset.
 * @param ld2_mem_size The memory access size of LD2.
 * @param ld1_micro_op_num The dynamic micro-op number of LD1.
 * @param ld2_micro_op_num The dynamic micro-op number of LD2.
 * @param proc_id The core id used for statistics.
 */
void fct_update_ld2_candidate_for_ld1(Addr ld1_pc_addr, Addr ld2_pc_addr,
                                      Addr ld1_effective_addr,
                                      Addr ld2_effective_addr,
                                      unsigned int offset_delta, bool direction,
                                      unsigned int ld2_mem_size,
                                      unsigned int ld1_micro_op_num,
                                      unsigned int ld2_micro_op_num,
                                      unsigned int proc_id);

/**
 * Reinforces an already-installed LD1->LD2 candidate after retire observation.
 *
 * Confidence increases only if the FCT row for LD1 already names the same LD2
 * PC, offset delta, direction, and LD2 memory access size.
 *
 * @param ld1_pc_addr The PC of the retired first load.
 * @param ld2_pc_addr The PC of the retired second load candidate.
 * @param offset_delta The absolute cache-line offset delta between LD1 and LD2.
 * @param direction TRUE if LD2's cache-line offset is greater than or equal to
 *        LD1's offset.
 * @param ld2_mem_size The memory access size of LD2.
 */
void fct_reinforce_ld2_candidate_for_ld1(Addr ld1_pc_addr, Addr ld2_pc_addr,
                                         unsigned int offset_delta,
                                         bool direction,
                                         unsigned int ld2_mem_size);

/**
 * Promotes a training-table validated LD1->LD2 candidate into the FCT.
 *
 * If the FCT already has a row for ld1_pc_addr with the same LD2 PC, a new
 * offset delta is installed in the second slot or replaces the weaker slot.
 * A different LD2 PC replaces the entire row.
 *
 * @param ld1_pc_addr The PC of the retired first load.
 * @param ld2_pc_addr The PC of the retired second load candidate.
 * @param ld1_effective_addr The effective address produced by LD1.
 * @param ld2_effective_addr The effective address produced by LD2.
 * @param offset_delta The absolute cache-line offset delta between LD1 and LD2.
 * @param direction TRUE if LD2's cache-line offset is greater than or equal to
 *        LD1's offset.
 * @param ld2_mem_size The memory access size of LD2.
 * @param ld1_micro_op_num The dynamic micro-op number of LD1.
 * @param ld2_micro_op_num The dynamic micro-op number of LD2.
 * @param proc_id The core id used for statistics.
 */
void fct_promote_ld2_candidate_for_ld1(Addr ld1_pc_addr, Addr ld2_pc_addr,
                                       Addr ld1_effective_addr,
                                       Addr ld2_effective_addr,
                                       unsigned int offset_delta,
                                       bool direction,
                                       unsigned int ld2_mem_size,
                                       unsigned int ld1_micro_op_num,
                                       unsigned int ld2_micro_op_num,
                                       unsigned int proc_id);

#endif /* IFUSE_FCT_H */
