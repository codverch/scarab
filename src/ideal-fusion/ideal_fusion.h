#ifndef __IDEAL_FUSION_H__
#define __IDEAL_FUSION_H__

#include "globals/global_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Called once for each fetched op after its ideal-fusion metadata is
 * initialized. Pass-specific sequencing and classification will live here.
 */
void ideal_fusion_on_fetch_op(Op* op);

/*
 * Pass-2 hooks. LOAD2 remains in the ROB for accounting, but its value becomes
 * available when the corresponding LOAD1 completes.
 */
void ideal_fusion_on_rename(Op* op, void (*wake_action)(Op*, Op*, uns));
void ideal_fusion_on_load_complete(Op* op,
                                   void (*wake_action)(Op*, Op*, uns));
Flag ideal_fusion_load2_is_nop(const Op* op);

#ifdef __cplusplus
}
#endif

#endif /* __IDEAL_FUSION_H__ */
