#ifndef IFUSE_STATS_H
#define IFUSE_STATS_H

#include "../globals/global_types.h"

/**
 * Samples GP physical-register occupancy and utilization for I-Fuse studies.
 * Average occupied regs: IFUSE_GP_REG_OCCUPIED_TOTAL / IFUSE_GP_REG_OCCUPIED_OBSERVATIONS.
 * Average utilization %: IFUSE_GP_REG_UTIL_PCT_TOTAL / IFUSE_GP_REG_OCCUPIED_OBSERVATIONS.
 */
void ifuse_observe_gp_reg_pressure(uns proc_id);

/**
 * Samples the number of extra physical registers held for predicted/fused LOAD2.
 * Average: IFUSE_EXTRA_REG_IN_USE_TOTAL / IFUSE_EXTRA_REG_IN_USE_OBSERVATIONS.
 */
void ifuse_observe_extra_reg_pressure(uns proc_id);

void ifuse_extra_reg_note_alloc(void);
void ifuse_extra_reg_note_free(void);

/* Physical registers currently reserved for a LOAD2 that has not renamed. */
uns64 ifuse_extra_reg_in_use_now(void);

#endif /* IFUSE_STATS_H */
