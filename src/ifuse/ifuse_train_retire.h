#ifndef IFUSE_TRAIN_RETIRE_H
#define IFUSE_TRAIN_RETIRE_H

#include "../op.h"

/**
 * Initializes retire-time IFuse training state.
 *
 * The retired-load history uses -1 bucket heads as empty-list sentinels, so it
 * must be cleared explicitly before the first retiring load probes it.
 */
void ifuse_train_retire_init(void);

/**
 * Applies IFuse training updates for one op retiring from the ROB.
 *
 * This hook must run for every retired op. Decoupled frontend retirement is
 * intentionally sampled for frontend bookkeeping and cannot drive training.
 */
void ifuse_train_retired_op(Op* op);

#endif /* IFUSE_TRAIN_RETIRE_H */
