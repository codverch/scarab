#ifndef IFUSE_RENAME_H
#define IFUSE_RENAME_H

#include "../op.h"

/**
 * Applies IFuse-specific register-renaming behavior after normal renaming.
 */
void ifuse_rename_op(Op* op);

/**
 * Releases the IFuse-only physical register carried by a retired LOAD2.
 */
void ifuse_retire_op(Op* op);

/**
 * Releases the IFuse-only physical register carried by a flushed LOAD2.
 */
void ifuse_recover_op(Op* op);

/**
 * Releases an IFuse-only physical register when its APT prediction expires.
 */
void ifuse_free_ld2_physical_reg(unsigned int ld2_physical_reg_id);

#endif /* IFUSE_RENAME_H */
