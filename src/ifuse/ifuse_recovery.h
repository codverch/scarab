#ifndef IFUSE_RECOVERY_H
#define IFUSE_RECOVERY_H

#include "../op.h"

/**
 * Schedules a pipeline recovery for an IFuse LOAD2 prediction failure.
 *
 * LOAD2 itself remains valid. Only younger operations are replayed because
 * they may have consumed LOAD2's incorrectly predicted fused value.
 */
void ifuse_sched_recovery(Op* ld2_op, Counter detection_cycle);

/**
 * Marks the interval in which Scarab is freeing squashed operations.
 */
void ifuse_recovery_begin_flush(Counter recovery_op_num);
void ifuse_recovery_end_flush(void);

/**
 * Repairs fetch-time IFuse table state for one squashed operation.
 */
void ifuse_recover_flushed_op(const Op* op);

#endif /* IFUSE_RECOVERY_H */
