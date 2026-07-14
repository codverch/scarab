#ifndef IFUSE_RENAME_H
#define IFUSE_RENAME_H

#include "../op.h"

/**
 * Applies IFuse-specific register-renaming behavior after normal renaming.
 *
 * For fused LOAD2, rebinds the architectural destination to the physical
 * register reserved by LOAD1 and frees the destination ordinary rename just
 * allocated, so producers/consumers share one physical register.
 */
void ifuse_rename_op(Op* op);

/**
 * Releases an unbound IFuse-reserved physical register on LOAD2 retire.
 * No-op after a successful fused handoff into dst_reg_id.
 */
void ifuse_retire_op(Op* op);

/**
 * Releases an unbound IFuse-reserved physical register on LOAD2 flush.
 * No-op after a successful fused handoff into dst_reg_id.
 */
void ifuse_recover_op(Op* op);

/**
 * Releases an IFuse-reserved physical register when its APT prediction expires
 * or handoff cannot install it as LOAD2's destination.
 */
void ifuse_free_ld2_physical_reg(unsigned int ld2_physical_reg_id);

#endif /* IFUSE_RENAME_H */
