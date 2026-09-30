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
 * by the retire-stage runtime training table.
 */

 /**
 * One FCT row contains one LD2 candidate and metadata for a single LD1 PC.
 *
 * Hardware-design storage model (the C fields below intentionally use normal
 * host types): a modeled row is a 32-bit LD1 PC tag (IFUSE_FCT_PC_TAG_BITS,
 * truncated the same way as the training table's tags -- see
 * ifuse_training_table.c), a 48-bit LD2 PC (kept full width: this is a
 * predicted value that must exact-match a real future op's PC in
 * apt_lookup(), not a disambiguation tag, so truncating it would raise the
 * false-fusion-match rate directly instead of just aliasing a hash bucket),
 * a 6-bit offset magnitude, a 1-bit direction, a 3-bit log2(LD2 access
 * size), a 10-bit confidence score (IFUSE_FCT_CONFIDENCE_MAX), and 1 valid
 * bit: 101 bits/row. A 512-row FCT (ifuse_fct_hash_bits = 9) costs
 * 512 * 101 = 51,712 bits = 6.3125 KiB, plus tree-PLRU state: 3 bits per
 * 4-way set, 128 * 3 = 384 bits.
 *
 * ld1_effective_addr, ld2_effective_addr, ld1_micro_op_num, and
 * ld2_micro_op_num are simulator-side bookkeeping, not part of the modeled
 * hardware row: ft.cc computes the actual predicted LD2 address from the
 * live dynamic op's oracle_info plus this row's offset_delta/direction, and
 * ACI validates the result against the real cache block at execution time --
 * neither ever reads these four fields back off a looked-up row. They are
 * kept here for simulator-side address bookkeeping and are excluded from the
 * storage estimate above.
 */
typedef struct FCT_Row {
    // Load identification
    Addr         ld1_pc_addr;
    Addr         ld2_pc_addr;

    // Memory access information -- effective addrs are simulator bookkeeping
    // only; see the storage-model comment above.
    Addr         ld1_effective_addr;
    Addr         ld2_effective_addr;
    unsigned int offset_delta;
    unsigned int ld2_mem_size;

    // Execution context -- simulator bookkeeping only; see the storage-model
    // comment above.
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
