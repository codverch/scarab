#ifndef __IDEAL_FUSION_H__
#define __IDEAL_FUSION_H__

#include "globals/global_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum Ideal_Fusion_Policy_enum {
  IDEAL_FUSION_OLDEST_FIRST,
  IDEAL_FUSION_MOST_RECENT,
  NUM_IDEAL_FUSION_POLICIES
} Ideal_Fusion_Policy;

void ideal_fusion_on_fetch_op(Op* op);

/*
 * Pass-2 actuation:
 *   - LOAD2 stays on the ROB chain but does not consume node_count / LSQ / RS
 *   - LOAD2 renames normally (own physical dest); src consumer registration skipped
 *   - Load2 buffer coordinates LOAD1 completion with LOAD2 dependent wakeup
 */
#define LOAD2_BUFFER_HT_SIZE 1000003

typedef struct Load2BufferEntry {
  Op* load2;
  Counter load2_unique_num;
  Flag load2_waiting;
  Flag load1_completed;
  Flag pair_completed;
  Counter load1_wake_cycle;
  Counter load1_done_cycle;
  Counter load1_micro_op_num;
  Counter load2_micro_op_num;
} Load2BufferEntry;

typedef struct Load2BufferNode {
  Load2BufferEntry entry;
  struct Load2BufferNode* next;
} Load2BufferNode;

extern Load2BufferNode* load2_buffer_ht[LOAD2_BUFFER_HT_SIZE];

Load2BufferNode* ideal_fusion_find_load2_buffer(Counter load1_micro_op_num);
Load2BufferNode* ideal_fusion_create_load2_buffer(Counter load1_micro_op_num);
void ideal_fusion_remove_load2_buffer(Load2BufferNode* node);

void ideal_fusion_on_map(Op* op, void (*wake_action)(Op*, Op*, uns));
void ideal_fusion_on_load1_wake(Op* load1, void (*wake_action)(Op*, Op*, uns));
Flag ideal_fusion_load2_is_nop(const Op* op);

#ifdef __cplusplus
}
#endif

#endif /* __IDEAL_FUSION_H__ */
