#ifndef IFUSE_EXEC_PAIR_H
#define IFUSE_EXEC_PAIR_H

#include "../op.h"

/**
 * Tracks a mapped LOAD1 or LOAD2 in the execution-side IFuse pair buffer.
 *
 * Register ownership is handled by APT during rename. This separate buffer
 * exists only to coordinate the modeled early availability of LOAD2's result.
 */
void ifuse_exec_pair_track_mapped_load(Op* op,
                                       void (*wake_action)(Op*, Op*, uns));

/**
 * Handles the execution-side IFuse effect of a producer wake-up.
 *
 * When LOAD1 completes, its validated LOAD2 may wake dependents immediately.
 */
void ifuse_exec_pair_handle_producer_wakeup(
    Op* op, Dep_Type type, void (*wake_action)(Op*, Op*, uns));

/**
 * Returns TRUE when LOAD2 already emitted its fused early wake-up signal.
 */
Flag ifuse_exec_pair_skip_duplicate_wakeup(const Op* op, Dep_Type type);

/**
 * Returns TRUE when a validated LOAD2 must bypass LSQ and d-cache access.
 *
 * The op still enters the ROB, issue queue, and load AGU.
 */
Flag ifuse_exec_pair_bypass_ld2_memory_pipeline(const Op* op);

/**
 * Records that a validated LOAD2 finished address generation.
 *
 * LOAD2 can retire only after both LOAD1 data production and LOAD2 address
 * generation have completed.
 */
void ifuse_exec_pair_complete_ld2_agu(Op* op);

/**
 * Removes the execution-side record owned by one LOAD1 prediction.
 *
 * APT calls this when a prediction expires or is discarded before a successful
 * LOAD2 handoff.
 */
void ifuse_exec_pair_forget_ld1_prediction(Counter ld1_op_num);

/**
 * Removes any execution-side pair-buffer entry that refers to op.
 *
 * Called before an Op is returned to Scarab's reusable op pool.
 */
void ifuse_exec_pair_forget_op(const Op* op);

#endif /* IFUSE_EXEC_PAIR_H */
