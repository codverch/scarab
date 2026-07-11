#ifndef __RFP_PRF_H__
#define __RFP_PRF_H__

#include "globals/global_types.h"
#include "map_rename.h"
#include "op.h"


/* Per-physical-register prefetch state for the PRF model, indexed by prfid. */
typedef struct RFP_Prf_Entry_struct {
  /* Ready: completed prefetch sitting in the PRF. */
  Flag    valid;
  Addr    prefetched_va;     /* line-aligned VA prefetched */
  Counter ready_cycle;      /* cycle the value arrived and is usable */
  Counter owner_op_unique;

  /* Started: prefetch began (won an L1 port) but may not be ready yet. */
  Flag    started_valid;
  Counter started_owner_op_unique;

  /* Dropped: load beat its prefetch; suppress the late writeback. */
  Flag    dropped_valid;
  Counter dropped_owner_op_unique;
} RFP_Prf_Entry;

void rfp_prf_init(void);
void rfp_prf_clear(uns8 proc_id, uns16 prfid);
void rfp_prf_clear_op(Op* op);
Flag rfp_prf_write(uns8 proc_id, uns16 prfid, Addr va, Counter ready_cycle, Counter owner_op_unique);
void rfp_prf_set_inflight(uns8 proc_id, uns16 prfid, Counter owner_op_unique);
void rfp_prf_mark_dropped(uns8 proc_id, uns16 prfid, Counter owner_op_unique);
/* Returns TRUE if a correctly-owned, line-matching prefetch has won L1 arbitration for this prfid
 * (i.e. it will deliver the value). When TRUE and ready_cycle_out != NULL, *ready_cycle_out is set to
 * the cycle the prefetched data actually becomes usable (which may be slightly in the future for a
 * prefetch that only just won arbitration — this drives partial latency mitigation). */
Flag rfp_prf_ready(uns8 proc_id, uns16 prfid, Addr va, Counter launch_cycle, Counter owner_op_unique,
                   Counter* ready_cycle_out);
Flag rfp_prf_is_inflight(uns8 proc_id, uns16 prfid, Counter owner_op_unique);
Flag rfp_op_load_prfid(const Op* op, uns16* prfid);

#endif 
