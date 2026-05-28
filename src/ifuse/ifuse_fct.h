// STD headers
#include <stdbool.h>
#include <stdint.h>

// Custom headers
#include "globals/global_defs.h"
#include "globals/global_types.h"

#ifndef IFUSE_FCT_H
#define IFUSE_FCT_H

/**
 * Fusion Candidate Table (FCT)
 * ============================
 * The FCT predicts load fusion candidates by mapping each LD1 PC to a
 * corresponding LD2 candidate. The current implementation models an ideal
 * runtime structure and serves as the baseline prior to introducing realistic
 * capacity constraints.
 *
 * The current runtime policy inserts an entry on the first retired observation
 * of an LD1-LD2 pair. If the same LD1 is later observed with a different LD2,
 * the existing entry is updated immediately.
 */

 /**
 * One FCT row contains one LD2 candidate and metadata for a single LD1 PC.
 */
typedef struct FCT_Row {
    // Load identification
    Addr         ld1_pc_addr;
    Addr         ld2_pc_addr;

    // Memory access information
    Addr         ld1_effective_addr;
    Addr         ld2_effective_addr;
    unsigned int offset_delta;
    unsigned int ld2_mem_size;

    // Execution context
    unsigned int ld1_micro_op_num;
    unsigned int ld2_micro_op_num;

    // Prediction metadata
    bool         direction;
    bool         valid;
    unsigned int confidence_score;
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
 * Updates the confidence score after a frontend prediction resolves.
 *
 * @param ld1_pc_addr The PC of the predicted first load.
 * @param prediction_correct TRUE if the fused prediction was correct.
 */
void fct_update_confidence(Addr ld1_pc_addr, bool prediction_correct);

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
 * This dynamic-training path is sticky by LD1 PC: if the FCT already has a row
 * for ld1_pc_addr, the promotion is skipped. Later ordinary retire observations
 * can still reinforce the installed exact pair.
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
