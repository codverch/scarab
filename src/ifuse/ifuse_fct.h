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
 * The FCT predicts load fusion candidates by mapping each LD1 PC to a single
 * LD2 candidate. Entries can be preloaded from an offline PGO file or promoted
 * by the retire-stage runtime training table. LD1/LD2 PCs use partial tags
 * (IFUSE_FCT_PC_TAG_BITS, default 32).
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
    Counter      ld1_micro_op_num;
    Counter      ld2_micro_op_num;

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

/* Promote one runtime-trained candidate. Returns TRUE when installed. */
Flag fct_install_runtime_candidate(Addr ld1_pc_addr, Addr ld2_pc_addr,
                                   Addr ld1_effective_addr,
                                   Addr ld2_effective_addr,
                                   unsigned int offset_delta, bool direction,
                                   unsigned int ld2_mem_size,
                                   Counter ld1_micro_op_num,
                                   Counter ld2_micro_op_num,
                                   unsigned int proc_id);

#endif /* IFUSE_FCT_H */
