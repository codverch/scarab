#ifndef IFUSE_FUSION_LOG_H
#define IFUSE_FUSION_LOG_H

#include "../op.h"

/**
 * Logs a successfully retired LOAD1/LOAD2 fusion pair when --ifuse_fusion_log
 * is set. Output uses the same CSV schema as ideal-fusion candidate logs.
 */
void ifuse_fusion_log_retired_pair(Op* op);

#endif  // IFUSE_FUSION_LOG_H
