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
 * A validated LOAD2 is truly fused: it does not occupy a ROB slot (doesn't
 * count against node_count), an issue-queue/RS entry, or an LSQ entry. Its
 * result and "address generation" are both completed immediately via
 * ifuse_exec_pair_signal_ld2() once LOAD1's data is available, so it never
 * needs to be scheduled or executed independently. It still goes through
 * ordinary fetch/decode/rename so its dependents can bind to LOAD1's
 * forwarded physical register.
 */
Flag ifuse_exec_pair_bypass_ld2_memory_pipeline(const Op* op);

/**
 * Legacy completion hook for LOAD2 address generation.
 *
 * Under true fusion LOAD2 never reaches the dcache stage (it never gets an
 * issue-queue entry), so this is normally unreachable; AGU completion is
 * instead modeled immediately in ifuse_exec_pair_signal_ld2(). Kept as a
 * defensive no-op fallback if bypass() ever becomes true for an op that
 * somehow still reaches this stage.
 */
void ifuse_exec_pair_complete_ld2_agu(Op* op);

/**
 * Returns TRUE when a fused LOAD2 takes no ROB, LSQ, or RS entry. This holds
 * only in the true-fusion model; a realistic LOAD2 (ifuse_ld2_realistic or
 * either ablation) occupies all three and runs on a load port to validate.
 */
Flag ifuse_exec_pair_ld2_skips_backend(const Op* op);

/**
 * Keeps a fused LOAD2's earlier wake cycle when its own ordinary L1-D access
 * (no-fusion ablation) completes later, so dependents that map afterward still
 * see the fused data's time.
 */
void ifuse_exec_pair_restore_early_wake(Op* op);

/* ifuse_ld2_bank_port: LD2's word on its own L1-D bank read port. */
Flag ifuse_exec_pair_request_ld2_word(const Op* ld1_op);
Flag ifuse_exec_pair_ld2_word_pending(Counter ld1_op_num);
void ifuse_exec_pair_ld2_word_read(Counter ld1_op_num, Flag hit, Counter ready_cycle,
                                   void (*wake_action)(Op*, Op*, uns));

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

void ifuse_exec_pair_observe_live_pairs(uns proc_id);

#endif /* IFUSE_EXEC_PAIR_H */
