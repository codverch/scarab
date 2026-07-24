/* Register File Prefetch (RFP) — stride prefetch table */

#include "prefetcher/rfp.h"
#include "prefetcher/rfp_prf.h"

#include <stdlib.h>
#include <string.h>

#include "globals/assert.h"
#include "globals/global_vars.h"
#include "statistics.h"

#include "cmp_model.h"
#include "core.param.h"
#include "dcache_stage.h"
#include "freq.h"
#include "lsq.h"
#include "memory/memory.h"
#include "memory/memory.param.h"
#include "model.h"

static RFP_PT_Entry rfp_pt[RFP_PT_MAX_SETS][RFP_PT_MAX_WAYS];
static RFP_Pat_Entry rfp_pat[RFP_PAT_MAX_SETS][RFP_PAT_MAX_WAYS];
static uns rfp_set_index_mask;
static uns rfp_pc_tag_shift;
#define RFP_INFLIGHT_MAX 127

static inline uns rfp_log2_u(uns v) {
  uns shift = 0;
  while (v >>= 1)
    shift++;
  return shift;
}

static inline uns rfp_pat_num_entries(void) {
  return RFP_PAT_NUM_SETS * RFP_PAT_NUM_WAYS;
}

static inline Flag rfp_stride_in_range(int64 stride) {
  return stride >= RFP_STRIDE_MIN && stride <= RFP_STRIDE_MAX;
}

static inline Addr rfp_va_pfn(Addr va) {
  return (va >> RFP_PAGE_OFFSET_BITS) & RFP_PFN_MASK;
}

static inline uns16 rfp_va_page_offset(Addr va) {
  return (uns16)(va & RFP_PAGE_OFFSET_MASK);
}

static inline uns rfp_pat_set(Addr pfn) {
  return (uns)(pfn & (RFP_PAT_NUM_SETS - 1));
}

static inline uns8 rfp_pat_ptr_encode(uns set, uns way) {
  ASSERT(0, set < RFP_PAT_NUM_SETS && way < RFP_PAT_NUM_WAYS);
  return (uns8)(set * RFP_PAT_NUM_WAYS + way);
}

static inline void rfp_pat_ptr_decode(uns8 pat_ptr, uns* set, uns* way) {
  ASSERT(0, pat_ptr < rfp_pat_num_entries());
  *set = pat_ptr / RFP_PAT_NUM_WAYS;
  *way = pat_ptr % RFP_PAT_NUM_WAYS;
}

static inline Addr rfp_entry_va(const RFP_PT_Entry* entry) {
  uns set;
  uns way;

  rfp_pat_ptr_decode(entry->pat_ptr, &set, &way);
  ASSERT(0, rfp_pat[set][way].valid);
  return (rfp_pat[set][way].pfn << RFP_PAGE_OFFSET_BITS) | entry->page_offset;
}

static int rfp_pat_lookup_ptr(Addr pfn) {
  uns set = rfp_pat_set(pfn);

  for (uns way = 0; way < RFP_PAT_NUM_WAYS; way++) {
    if (rfp_pat[set][way].valid && rfp_pat[set][way].pfn == pfn)
      return (int)rfp_pat_ptr_encode(set, way);
  }
  return -1;
}

static int rfp_pat_alloc_ptr(uns8 proc_id, Addr pfn) {
  int ptr = rfp_pat_lookup_ptr(pfn);
  if (ptr >= 0)
    return ptr;

  uns set = rfp_pat_set(pfn);

  for (uns way = 0; way < RFP_PAT_NUM_WAYS; way++) {
    if (!rfp_pat[set][way].valid) {
      rfp_pat[set][way].valid = TRUE;
      rfp_pat[set][way].pfn = pfn;
      STAT_EVENT(proc_id, RFP_PAT_ALLOC);
      return (int)rfp_pat_ptr_encode(set, way);
    }
  }

  /* Set is full — evict way 0 (hardware would use replacement policy). */
  STAT_EVENT(proc_id, RFP_PAT_EVICT);
  rfp_pat[set][0].valid = TRUE;
  rfp_pat[set][0].pfn = pfn;
  return (int)rfp_pat_ptr_encode(set, 0);
}

static void rfp_entry_set_va(uns8 proc_id, RFP_PT_Entry* entry, Addr va) {
  int pat_ptr = rfp_pat_alloc_ptr(proc_id, rfp_va_pfn(va));

  ASSERT(proc_id, pat_ptr >= 0 && pat_ptr < (int)rfp_pat_num_entries());
  entry->pat_ptr = (uns8)pat_ptr;
  entry->page_offset = rfp_va_page_offset(va);
}

/* Initialize the RFP table */
void rfp_init(void) {
  if (!RFP_ON)
    return;

  ASSERT(0, RFP_PT_NUM_SETS > 0 && RFP_PT_NUM_WAYS > 0);
  ASSERT(0, RFP_PAT_NUM_SETS > 0 && RFP_PAT_NUM_WAYS > 0);
  ASSERT(0, RFP_PT_NUM_SETS <= RFP_PT_MAX_SETS && RFP_PT_NUM_WAYS <= RFP_PT_MAX_WAYS);
  ASSERT(0, RFP_PAT_NUM_SETS <= RFP_PAT_MAX_SETS && RFP_PAT_NUM_WAYS <= RFP_PAT_MAX_WAYS);
  ASSERT(0, (RFP_PT_NUM_SETS & (RFP_PT_NUM_SETS - 1)) == 0);
  ASSERT(0, (RFP_PAT_NUM_SETS & (RFP_PAT_NUM_SETS - 1)) == 0);
  ASSERT(0, rfp_pat_num_entries() <= 256);

  rfp_set_index_mask = RFP_PT_NUM_SETS - 1;
  rfp_pc_tag_shift = RFP_PC_SET_SHIFT + rfp_log2_u(RFP_PT_NUM_SETS);

  if (rfp_use_prf_model())
    rfp_prf_init();

  memset(rfp_pt, 0, sizeof(rfp_pt));
  memset(rfp_pat, 0, sizeof(rfp_pat));
}

/* Calculate the set index from the PC */
static inline uns rfp_set(Addr pc) {
  Addr index = pc >> RFP_PC_SET_SHIFT;
  return (uns)(index & rfp_set_index_mask);
}

/* Calculate the tag from the PC */
static inline uns16 rfp_tag(Addr pc) {
  Addr tag = pc >> rfp_pc_tag_shift;
  return (uns16)(tag & RFP_TAG_MASK);
}

/* Lookup the entry in the RFP table */
static int rfp_lookup(uns set, uns16 tag) {
  for (int way = 0; way < (int)RFP_PT_NUM_WAYS; way++)
    if (rfp_pt[set][way].valid && rfp_pt[set][way].tag == tag)
      return way;

  return -1;
}

/* Get the entry from the RFP table */
static inline RFP_PT_Entry* rfp_get_entry(Addr pc) {
  uns set = rfp_set(pc);
  uns16 tag = rfp_tag(pc);
  int way = rfp_lookup(set, tag);
  if (way < 0)
    return NULL;
  return &rfp_pt[set][way];
}

/* Increment the inflight count for the entry. Done on load allocation
(at rename) for every on-path load, regardless of prediction. */
static inline void rfp_inflight_inc(Addr pc) {
  RFP_PT_Entry* entry = rfp_get_entry(pc);
  if (!entry)
    return;
  if (entry->inflight < RFP_INFLIGHT_MAX)
    entry->inflight++;
}

/* Decrement the inflight count for the entry. Done when a load retires
 or when squashed by a pipeline recovery. */
static inline void rfp_inflight_dec(Addr pc) {
  RFP_PT_Entry* entry = rfp_get_entry(pc);
  if (!entry)
    return;
  if (entry->inflight > 0)
    entry->inflight--;
}

/**
 * Find the victim entry in the RFP table, using utility. First check if there
 * is an empty entry. If not, find the victim is the entry with the lowest
 * utility.
 */
static int rfp_victim(uns set) {
  for (int way = 0; way < (int)RFP_PT_NUM_WAYS; way++)
    if (!rfp_pt[set][way].valid)
      return way;

  int victim = 0;
  for (int way = 1; way < (int)RFP_PT_NUM_WAYS; way++)
    if (rfp_pt[set][way].utility < rfp_pt[set][victim].utility)
      victim = way;
  return victim;
}

/*
 * Train the RFP table. If the entry is not found, allocate a new entry.
 * If the entry is found, update the entry.
 */
static void rfp_train_entry(uns8 proc_id, Addr pc, Addr va) {
  uns set = rfp_set(pc);
  uns16 tag = rfp_tag(pc);
  int way = rfp_lookup(set, tag);

  if (way < 0) {
    /* No entry found, allocate a new entry */
    way = rfp_victim(set);
    rfp_pt[set][way].tag = tag;
    rfp_entry_set_va(proc_id, &rfp_pt[set][way], va);
    rfp_pt[set][way].stride = 0;
    rfp_pt[set][way].confidence = 0;
    rfp_pt[set][way].utility = 0;
    rfp_pt[set][way].inflight = 0;
    rfp_pt[set][way].valid = TRUE;
    STAT_EVENT(proc_id, RFP_PT_ALLOC);
    return;
  }

  STAT_EVENT(proc_id, RFP_PT_HIT);

  RFP_PT_Entry *entry = &rfp_pt[set][way];
  int64 observed_stride = (int64)((int64)va - (int64)rfp_entry_va(entry));

  if (!rfp_stride_in_range(observed_stride)) {
    /* Table 1 stores only a 5-bit signed byte stride. */
    STAT_EVENT(proc_id, RFP_STRIDE_OUT_OF_RANGE);
    entry->confidence = 0;
    entry->utility = 0;
    rfp_entry_set_va(proc_id, entry, va);
    return;
  }

  if (observed_stride == entry->stride) {
    STAT_EVENT(proc_id, RFP_STRIDE_CONFIRM);
    if (entry->confidence < RFP_CONF_MAX) {
      /* Increment confidence with probability of 1/16 (2^4) */
      if ((rand() & ((1u << RFP_PROB_SHIFT) - 1)) == 0) {
        entry->confidence++;
        STAT_EVENT(proc_id, RFP_CONF_INC);
      }
    }
    /* Increment utility until 3 */
    if (entry->utility < 3)
      entry->utility++;
  } else {
    /* Stride changed, reset confidence and utility */
    STAT_EVENT(proc_id, RFP_STRIDE_CHANGE);
    entry->stride = (int8)observed_stride;
    entry->confidence = 0;
    entry->utility = 0;
  }

  rfp_entry_set_va(proc_id, entry, va);
}

/* Predict the next address based on the RFP table */
Flag rfp_predict(Addr pc, Addr *predicted_addr) {
  uns set = rfp_set(pc);
  uns16 tag = rfp_tag(pc);
  int way = rfp_lookup(set, tag);

  /* RFP prediction is made only if confidence is maximum */
  if (way < 0 || rfp_pt[set][way].confidence < RFP_CONF_MAX)
    return FALSE;

  RFP_PT_Entry *entry = &rfp_pt[set][way];
  /* Use outstanding instance depth from PT. */
  int64 lookahead = (int64)entry->inflight + 1;
  *predicted_addr =
      (Addr)((int64)rfp_entry_va(entry) + (int64)entry->stride * lookahead);
  return TRUE;
}

/* Clear PT confidence after an execute-time address mismatch (ISCA'22 / README). */
void rfp_reset_confidence_on_mispredict(uns8 proc_id, Addr pc) {
  RFP_PT_Entry* entry;

  if (!RFP_ON)
    return;

  entry = rfp_get_entry(pc);
  if (!entry || entry->confidence == 0)
    return;

  entry->confidence = 0;
  STAT_EVENT(proc_id, RFP_CONF_RESET_ON_MISPRED);
}

/* ISCA'22 §3.2.1 — older in-flight stores can supply the prefetched data; if any
   overlap the predicted load footprint, wait (LSQ scan + store-forwarding buffer). */
Flag rfp_blocked_by_older_stores(uns8 proc_id, Counter load_op_num, Addr pred_addr, uns load_size) {
  if (!RFP_ON || !pred_addr || !load_size)
    return FALSE;

  if (lsq_older_store_overlaps(proc_id, load_op_num, pred_addr, load_size))
    return TRUE;
  if (scan_stores(pred_addr, load_size))
    return TRUE;
  return FALSE;
}

/* Per-op RFP fetch-time reset (icache). Ved: F5 — the PT lookup/prediction itself moved to rename
   (rfp_prefetch_launch) so its lookahead reads the same inflight it increments; only the field resets
   remain here at fetch (ISCA'22 §3.4). */
void rfp_predict_at_fetch(Op *op) {
  ASSERT(op->proc_id, op);

  if (!RFP_ON)
    return;

  op->rfp_predicted = FALSE;
  op->rfp_predicted_addr = 0;
  op->rfp_dropped_load_first = FALSE;
  op->rfp_served = FALSE;
  op->rfp_partial_mitigated = FALSE;
  op->rfp_pred_resolved = FALSE;
}

/* Launch RFP prefetch for PRF model: enqueue into LSQ-side RFP queue. */
static void rfp_prefetch_launch_prf(Op* op, Addr line_addr) {
  uns16 prfid;

  /* Get the PRF ID for the load */
  if (!rfp_op_load_prfid(op, &prfid)) {
    STAT_EVENT(op->proc_id, RFP_PRF_LAUNCH_NO_DEST);
    STAT_EVENT(op->proc_id, RFP_ELIGIBLE_NOT_ISSUED);
    return;
  }

  op->rfp_prfid = prfid;
  STAT_EVENT(op->proc_id, RFP_PRF_LAUNCH);

  /* Enqueue the load into the LSQ-side RFP FIFO */
  if (lsq_rfp_enqueue(op->proc_id, op, line_addr, prfid, op->rfp_launch_cycle)) {
    STAT_EVENT(op->proc_id, RFP_PREFETCH_INJECTED);
    STAT_EVENT(op->proc_id, RFP_ISSUED);
  } else {
    STAT_EVENT(op->proc_id, RFP_ELIGIBLE_NOT_ISSUED);
  }
}

/* Launch the RFP prefetch request at rename */
void rfp_prefetch_launch(Op* op) {
  Addr line_addr;
  Counter launch_cycle;

  ASSERT(op->proc_id, op);

  if (!RFP_ON)
    return;

  if (op->off_path)
    return;
  if (op->inst_info->table_info.mem_type != MEM_LD)
    return;

  /* Count all on-path loads seen at rename. */
  STAT_EVENT(op->proc_id, RFP_ALL_LOADS);

  /* Ved: F5 — do the PT lookup here at rename, BEFORE rfp_inflight_inc, so the prediction's lookahead
     (= inflight + 1) reads the same inflight this load is about to add to. Predicting at fetch (the old
     site) under-counted same-PC instances still between fetch and rename, shortening the lookahead in
     tight loops (ISCA'22 §3.4). */
  Addr predicted_addr = 0;
  if (rfp_predict(op->inst_info->addr, &predicted_addr)) {
    op->rfp_predicted = TRUE;
    op->rfp_predicted_addr = predicted_addr;
    STAT_EVENT(op->proc_id, RFP_PREDICTION_MADE);
    STAT_EVENT(op->proc_id, RFP_ELIGIBLE);
  }

  rfp_inflight_inc(op->inst_info->addr);

  if (!op->rfp_predicted)
    return;
  if (model->mem != MODEL_MEM)
    return;

  /* Predicted, on-path load eligible to launch an RFP at rename. */
  STAT_EVENT(op->proc_id, RFP_PREDICTED_AT_RENAME);

  /* Get the line address for the predicted address */
  line_addr = op->rfp_predicted_addr & ~(Addr)(DCACHE_LINE_SIZE - 1);
  launch_cycle = freq_cycle_count(FREQ_DOMAIN_CORES[op->proc_id]);
  op->rfp_launch_cycle = launch_cycle;
  rfp_prefetch_launch_prf(op, line_addr);
}

/* Train the RFP table on a load instruction.
 * Training is done when the load is retired.
 * Also accumulate retire-time coverage / latency for RFP analysis.
 */
void rfp_train_retire(Op *op) {
  ASSERT(op->proc_id, op);

  if (!RFP_ON)
    return;

  if (op->inst_info->table_info.mem_type != MEM_LD)
    return;
  /* PT inflight tracks outstanding instances of this load-PC. */
  rfp_inflight_dec(op->inst_info->addr);
  if (!op->oracle_info.va)
    return;

  STAT_EVENT(op->proc_id, RFP_RETIRE_LOAD);
  rfp_train_entry(op->proc_id, op->inst_info->addr, op->oracle_info.va);

  /* Retire-time prediction coverage (committed loads only). */
  if (op->rfp_predicted) {
    STAT_EVENT(op->proc_id, RFP_RETIRE_PRED);
    if (op->rfp_served) {
      STAT_EVENT(op->proc_id, RFP_RETIRE_COVERED);
      STAT_EVENT(op->proc_id, RFP_RETIRE_PRED_CORRECT);
    } else if (op->rfp_mispred_accounted) {
      STAT_EVENT(op->proc_id, RFP_RETIRE_PRED_WRONG);
    } else if (op->rfp_dropped_load_first) {
      /* Correct address, but prefetch lost the race — still a correct prediction. */
      STAT_EVENT(op->proc_id, RFP_RETIRE_PRED_CORRECT);
    }
  }
}

/* Track the squashed loads and decrement the inflight count for the load.
Loads are squashed due to pipeline recovery events caused by branch mispredictions or redirections. */
void rfp_track_squash(Op* op) {
  ASSERT(op->proc_id, op);
  if (!RFP_ON)
    return;
  if (op->inst_info->table_info.mem_type != MEM_LD)
    return;
  /* Ved: F5 — inflight is incremented only for on-path loads (rfp_prefetch_launch returns early on
     off_path before rfp_inflight_inc), so decrementing off-path squashes underflows the counter and
     shortens the lookahead (ISCA'22 §3.4). recover_seq_op_list (thread.c) is the sole rollback path
     that squashes renamed on-path loads, so this keeps the inc/dec pair balanced. */
  if (op->off_path)
    return;
  /* Decrement inflight for squashed loads. */
  rfp_inflight_dec(op->inst_info->addr);
}
