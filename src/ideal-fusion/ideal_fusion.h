#ifndef __IDEAL_FUSION_H__
#define __IDEAL_FUSION_H__

#include "globals/global_types.h"
#include "op_info.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum Ideal_Fusion_Policy_enum {
  IDEAL_FUSION_OLDEST_FIRST,
  IDEAL_FUSION_MOST_RECENT,
  NUM_IDEAL_FUSION_POLICIES
} Ideal_Fusion_Policy;

/*
 * Which memory class a run targets. Loads and stores obey very different
 * correctness rules -- a store pair may not be fused across another store,
 * because that reorders globally visible writes (see FUSION_EXPLAINED.md,
 * Part 3). A single run therefore discovers and fuses exactly one class.
 */
typedef enum Ideal_Fusion_Class_enum {
  IDEAL_FUSION_CLASS_LOADS,
  IDEAL_FUSION_CLASS_STORES,
  NUM_IDEAL_FUSION_CLASSES
} Ideal_Fusion_Class;

/* TRUE when IDEAL_FUSION_CLASS selects store-store fusion. */
Flag ideal_fusion_fusing_stores(void);

void ideal_fusion_on_fetch_op(Op* op);

/*
 * Pass-2 actuation:
 *   - A fused tail nucleus (LOAD2/STORE2) stays on the ROB chain but does not
 *     consume node_count / LSQ / RS
 *   - It renames normally (own physical dest, if any); src consumer
 *     registration is skipped
 *   - The rendezvous table below coordinates head completion with tail
 *     dependent wakeup: LOAD2 forwards LOAD1's REG_DATA_DEP wake; STORE2
 *     forwards STORE1's MEM_ADDR_DEP and MEM_DATA_DEP wakes (a store never
 *     fires REG_DATA_DEP). Without this forwarding a tail nucleus -- which
 *     never itself executes -- would never wake ops waiting on it, so any
 *     dependent load or store would deadlock.
 */
#define IDEAL_FUSION_RENDEZVOUS_HT_SIZE 1000003

typedef struct Ideal_Fusion_Rendezvous_Entry_struct {
  Op* tail;
  Counter tail_unique_num;
  Counter head_wake_cycle;
  Counter head_done_cycle;
  Counter head_micro_op_num;
  Counter tail_micro_op_num;
  /* Per Dep_Type (REG_DATA_DEP / MEM_ADDR_DEP / MEM_DATA_DEP): has the head
   * fired this wake yet, and has it already been forwarded to the tail? A
   * store's head fires two independent types in the same cycle; both must be
   * forwarded before the entry is retired. */
  Flag head_fired[NUM_DEP_TYPES];
  Flag tail_wake_sent[NUM_DEP_TYPES];
  uns tail_wakes_sent;
} Ideal_Fusion_Rendezvous_Entry;

typedef struct Ideal_Fusion_Rendezvous_Node_struct {
  Ideal_Fusion_Rendezvous_Entry entry;
  struct Ideal_Fusion_Rendezvous_Node_struct* next;
} Ideal_Fusion_Rendezvous_Node;

extern Ideal_Fusion_Rendezvous_Node*
  fusion_rendezvous_ht[IDEAL_FUSION_RENDEZVOUS_HT_SIZE];

Ideal_Fusion_Rendezvous_Node* ideal_fusion_find_rendezvous(
  Counter head_micro_op_num);
Ideal_Fusion_Rendezvous_Node* ideal_fusion_create_rendezvous(
  Counter head_micro_op_num);
void ideal_fusion_remove_rendezvous(Ideal_Fusion_Rendezvous_Node* node);

void ideal_fusion_on_map(Op* op, void (*wake_action)(Op*, Op*, uns));

/* Called from wake_up_ops() whenever a LOAD1 or STORE1 fires `type`; forwards
 * that same wake to the paired tail nucleus (see comment above). A no-op for
 * any op that is not a fusion head. */
void ideal_fusion_on_head_wake(Op* head, Dep_Type type,
                               void (*wake_action)(Op*, Op*, uns));
Flag ideal_fusion_load2_is_nop(const Op* op);
Flag ideal_fusion_store2_is_nop(const Op* op);

/*
 * TRUE for any fused tail nucleus (LOAD2 or STORE2). A tail nucleus stays on
 * the ROB chain but consumes no ROB slot, no LSQ entry and no issue-queue
 * entry, and issues no cache access -- it inherits its head nucleus's timing.
 * Pipeline stages should test this rather than the load-only predicate.
 */
Flag ideal_fusion_tail_is_nop(const Op* op);

/* Measurement mode (IDEAL_FUSION_PASS == 3): log real completion cycles of
 * paired loads without applying fusion. */
void ideal_fusion_measure_on_wake(Op* op);

#ifdef __cplusplus
}
#endif

#endif /* __IDEAL_FUSION_H__ */
