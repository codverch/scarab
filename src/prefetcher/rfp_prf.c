/* Register File Prefetch PRF-side state */

#include "prefetcher/rfp_prf.h"

#include <stdlib.h>
#include <string.h>

#include "globals/assert.h"

#include "core.param.h"
#include "map_rename.h"   // Ved: F3 — reg_file_get_reg_type + REG_FILE_REG_TYPE_*
#include "memory/memory.param.h"
#include "prefetcher/rfp.param.h"
#include "statistics.h"

/* Ved: F3 — the PRF tracker spans BOTH physical register files; their ids overlap, so
   rfp_op_load_prfid() remaps vector ids to INT_SIZE + phys to keep the two namespaces disjoint. */
#define RFP_PRF_NUM_ENTRIES (REG_TABLE_INTEGER_PHYSICAL_SIZE + REG_TABLE_VECTOR_PHYSICAL_SIZE)

/* PRF Table which tracks the prefetch-ready state for each physical register. */
static RFP_Prf_Entry* rfp_prf_table;

/* Get the PRF entry metadatafor a given physical register */
static RFP_Prf_Entry* rfp_prf_entry(uns8 proc_id, uns16 prfid) {
  ASSERT(proc_id, proc_id < NUM_CORES);
  if (!rfp_prf_table)
    return NULL;
  if (prfid >= RFP_PRF_NUM_ENTRIES)   // Ved: F3 — was REG_TABLE_INTEGER_PHYSICAL_SIZE (integer-only)
    return NULL;
  return &rfp_prf_table[proc_id * RFP_PRF_NUM_ENTRIES + prfid];   // Ved: F3
}

/* Initialize the PRF table */
void rfp_prf_init(void) {
  uns num_entries = NUM_CORES * RFP_PRF_NUM_ENTRIES;   // Ved: F3 — span both register files
  rfp_prf_table = calloc(num_entries, sizeof(RFP_Prf_Entry));
  ASSERT(0, rfp_prf_table);
}

/* Clear the PRF entry for a given physical register */
void rfp_prf_clear(uns8 proc_id, uns16 prfid) {
  RFP_Prf_Entry* entry = rfp_prf_entry(proc_id, prfid);
  if (!entry)
    return;

  entry->valid = FALSE;
  entry->prefetched_va = 0;
  entry->ready_cycle = 0;
  entry->owner_op_unique = 0;
  /* Ved: also clear the started/dropped markers. Leaving started_valid set after a
     mispredict/recovery clear strands any load waiting in rfp_wait (started=TRUE,
     ready=FALSE forever) and deadlocks the pipeline. */
  entry->started_valid = FALSE;
  entry->started_owner_op_unique = 0;
  entry->dropped_valid = FALSE;
  entry->dropped_owner_op_unique = 0;
}

/* Clear the PRF entry for a given op */
void rfp_prf_clear_op(Op* op) {
  uns16 prfid;

  if (!rfp_op_load_prfid(op, &prfid))
    return;
  rfp_prf_clear(op->proc_id, prfid);
}

/* Write to the PRF entry for a given physical register */
Flag rfp_prf_write(uns8 proc_id, uns16 prfid, Addr va, Counter ready_cycle, Counter owner_op_unique) {
  RFP_Prf_Entry* entry = rfp_prf_entry(proc_id, prfid);
  if (!entry)
    return FALSE;

  if (entry->dropped_valid && entry->dropped_owner_op_unique == owner_op_unique) {
    /* Explicit drop: suppress late PRF writeback for this load. */
    return FALSE;
  }

  entry->valid = TRUE;
  entry->prefetched_va = va;
  entry->ready_cycle = ready_cycle;
  entry->owner_op_unique = owner_op_unique;
  entry->dropped_valid = FALSE;
  return TRUE;
}

/* Set the started marker for the PRF entry */
void rfp_prf_set_inflight(uns8 proc_id, uns16 prfid, Counter owner_op_unique) {
  RFP_Prf_Entry* entry = rfp_prf_entry(proc_id, prfid);
  if (!entry)
    return;

  entry->started_valid = TRUE;
  entry->started_owner_op_unique = owner_op_unique;
}

/* Set the dropped marker for the PRF entry */
void rfp_prf_mark_dropped(uns8 proc_id, uns16 prfid, Counter owner_op_unique) {
  RFP_Prf_Entry* entry = rfp_prf_entry(proc_id, prfid);
  if (!entry)
    return;

  entry->dropped_valid = TRUE;
  entry->dropped_owner_op_unique = owner_op_unique;
}

/* Check if the PRF entry is ready for a given physical register and owner op:
 * the entry must be valid, the prefetched (line-aligned) VA must match the load,
 * the value must have become ready no earlier than the load launched (timeliness),
 * and the owner identity must match.
 */
Flag rfp_prf_ready(uns8 proc_id, uns16 prfid, Addr va, Counter launch_cycle, Counter owner_op_unique,
                   Counter* ready_cycle_out) {
  RFP_Prf_Entry* entry = rfp_prf_entry(proc_id, prfid);

  if (!entry || !entry->valid)
    return FALSE;
  /* prefetched_va is line-aligned, so compare the load address at line granularity. */
  if (entry->prefetched_va != (va & ~(Addr)(DCACHE_LINE_SIZE - 1)))
    return FALSE;
  if (entry->ready_cycle < launch_cycle)
    return FALSE;
  if (entry->owner_op_unique != owner_op_unique) {
    /* Stale PRF state from a different dynamic op; invalidate to avoid reuse. */
    STAT_EVENT(proc_id, RFP_PRF_OWNER_MISMATCH);
    entry->valid = FALSE;
    entry->prefetched_va = 0;
    entry->ready_cycle = 0;
    entry->owner_op_unique = 0;
    return FALSE;
  }
  if (ready_cycle_out)
    *ready_cycle_out = entry->ready_cycle;
  return TRUE;
}

/* Check if the PRF entry is inflight for a given physical register and owner op */
Flag rfp_prf_is_inflight(uns8 proc_id, uns16 prfid, Counter owner_op_unique) {
  RFP_Prf_Entry* entry = rfp_prf_entry(proc_id, prfid);

  if (!entry || !entry->started_valid)
    return FALSE;
  if (entry->started_owner_op_unique != owner_op_unique)
    return FALSE;
  return TRUE;
}

/* Get the PRF ID and register file index for a given op */
Flag rfp_op_load_prfid(const Op* op, uns16* prfid) {
  ASSERT(op->proc_id, op && prfid);

  if (op->inst_info->table_info.num_dest_regs == 0)
    return FALSE;

  uns16 phys = op->dst_reg_id[0][REG_TABLE_TYPE_PHYSICAL];
  if (phys == REG_TABLE_REG_ID_INVALID)
    return FALSE;

  /* Ved: F3 — integer and vector physical ids overlap between the two register files, so remap to a
     unified key that never aliases: integer dest -> phys; vector dest -> INT_SIZE + phys. dests[0].id
     is the flattened logical id reg_file_get_reg_type() expects, matching dst_reg_id[0] above. */
  int rtype = reg_file_get_reg_type(op->inst_info->dests[0].id);
  if (rtype == REG_FILE_REG_TYPE_GENERAL_PURPOSE) {
    *prfid = phys;
  } else if (rtype == REG_FILE_REG_TYPE_VECTOR) {
    *prfid = (uns16)(REG_TABLE_INTEGER_PHYSICAL_SIZE + phys);
  } else {
    return FALSE;   /* non-int/vector dest (flags/mask): no RFP */
  }

  return TRUE;
}
