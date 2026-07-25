/* Shadow RFP -- oracle-capacity RFP stride predictor, observational only.
 *
 * Ported from src/prefetcher/rfp.c (branch hpca2027-rfp): rfp_train_entry and
 * rfp_predict are reproduced here unchanged in algorithm (same 5-bit signed
 * stride field + range check, same 1-bit confidence with the 1/16
 * probabilistic increment, same utility-based PT victim policy, same
 * inflight-based prediction lookahead). See shadow_rfp.h for the two
 * intentional differences (enlarged PT/PAT capacity, widened PAT pointer).
 */

#include "prefetcher/shadow_rfp.h"

#include <stdlib.h>
#include <string.h>

#include "globals/assert.h"
#include "statistics.h"

/* Why a PT entry's last training update left it in its current state.
 * Recorded at train time; read back at predict time to attribute *why* a
 * load wasn't confidently (and correctly) predicted, without re-deriving it. */
typedef enum Shadow_Rfp_Update_Reason_enum {
  SHADOW_RFP_UPDATE_FRESH_ALLOC,
  SHADOW_RFP_UPDATE_STRIDE_OUT_OF_RANGE,
  SHADOW_RFP_UPDATE_STRIDE_CHANGED,
  SHADOW_RFP_UPDATE_CONFIRM,
} Shadow_Rfp_Update_Reason;

/* Max PT/PAT sizes: enlarged far past any realistic number of unique static
 * load PCs / resident pages so eviction should not occur in practice. Purely
 * a capacity knob -- the lookup/train/predict logic below is agnostic to
 * these sizes. */
#define SHADOW_RFP_PT_MAX_SETS 65536
#define SHADOW_RFP_PT_MAX_WAYS 16
#define SHADOW_RFP_PAT_MAX_SETS 16384
#define SHADOW_RFP_PAT_MAX_WAYS 16

#define SHADOW_RFP_TAG_BITS 16
#define SHADOW_RFP_TAG_MASK ((1u << SHADOW_RFP_TAG_BITS) - 1)
#define SHADOW_RFP_PC_SET_SHIFT 2

/* 1-bit confidence counter, matching RFP's actual functional counter (the
 * ISCA'22 paper's 3-bit figure is a storage-budget accounting detail; the
 * counter itself saturates at 1 in the real implementation this is ported
 * from). */
#define SHADOW_RFP_CONF_MAX 1
/* Probabilistic confidence increment: P(inc) = 1 / 2^SHADOW_RFP_PROB_SHIFT = 1/16. */
#define SHADOW_RFP_PROB_SHIFT 4

#define SHADOW_RFP_STRIDE_BITS 5
#define SHADOW_RFP_STRIDE_MIN (-(1 << (SHADOW_RFP_STRIDE_BITS - 1)))
#define SHADOW_RFP_STRIDE_MAX ((1 << (SHADOW_RFP_STRIDE_BITS - 1)) - 1)

#define SHADOW_RFP_PAGE_OFFSET_BITS 12
#define SHADOW_RFP_PAGE_OFFSET_MASK ((1u << SHADOW_RFP_PAGE_OFFSET_BITS) - 1)
#define SHADOW_RFP_PFN_BITS 44
#define SHADOW_RFP_PFN_MASK (((Addr)1 << SHADOW_RFP_PFN_BITS) - 1)

#define SHADOW_RFP_INFLIGHT_MAX 127

typedef struct Shadow_Rfp_Pat_Entry_struct {
  Flag valid;
  Addr pfn;
} Shadow_Rfp_Pat_Entry;

typedef struct Shadow_Rfp_Pt_Entry_struct {
  uns16 tag;
  /* Widened vs. real RFP's uns8 pat_ptr: that field hard-caps the real PAT at
   * 256 resident pages regardless of any size parameter. This is the one
   * deliberate divergence from the hardware model (see shadow_rfp.h). */
  uns32 pat_ptr;
  uns16 page_offset;
  int8 stride;
  uns8 confidence;
  uns8 utility;
  uns8 inflight;
  Flag valid;
  Shadow_Rfp_Update_Reason last_update_reason;
} Shadow_Rfp_Pt_Entry;

static Shadow_Rfp_Pt_Entry shadow_rfp_pt[SHADOW_RFP_PT_MAX_SETS][SHADOW_RFP_PT_MAX_WAYS];
static Shadow_Rfp_Pat_Entry shadow_rfp_pat[SHADOW_RFP_PAT_MAX_SETS][SHADOW_RFP_PAT_MAX_WAYS];
static uns shadow_rfp_set_index_mask;
static uns shadow_rfp_pc_tag_shift;
static Flag shadow_rfp_initialized = FALSE;

/* Lazy self-init, mirroring src/ifuse/ifuse_apt.c's apt_lookup/apt_insert_entry
 * pattern -- avoids wiring a new call into any global startup sequence. */
static inline void shadow_rfp_ensure_init(void) {
  if (!shadow_rfp_initialized) {
    shadow_rfp_init();
    shadow_rfp_initialized = TRUE;
  }
}

static inline uns shadow_rfp_log2_u(uns v) {
  uns shift = 0;
  while (v >>= 1)
    shift++;
  return shift;
}

static inline uns32 shadow_rfp_pat_num_entries(void) {
  return (uns32)SHADOW_RFP_PAT_NUM_SETS * (uns32)SHADOW_RFP_PAT_NUM_WAYS;
}

static inline Flag shadow_rfp_stride_in_range(int64 stride) {
  return stride >= SHADOW_RFP_STRIDE_MIN && stride <= SHADOW_RFP_STRIDE_MAX;
}

static inline Addr shadow_rfp_va_pfn(Addr va) {
  return (va >> SHADOW_RFP_PAGE_OFFSET_BITS) & SHADOW_RFP_PFN_MASK;
}

static inline uns16 shadow_rfp_va_page_offset(Addr va) {
  return (uns16)(va & SHADOW_RFP_PAGE_OFFSET_MASK);
}

static inline uns shadow_rfp_pat_set(Addr pfn) {
  return (uns)(pfn & (SHADOW_RFP_PAT_NUM_SETS - 1));
}

static inline uns32 shadow_rfp_pat_ptr_encode(uns set, uns way) {
  ASSERT(0, set < SHADOW_RFP_PAT_NUM_SETS && way < SHADOW_RFP_PAT_NUM_WAYS);
  return (uns32)set * (uns32)SHADOW_RFP_PAT_NUM_WAYS + (uns32)way;
}

static inline void shadow_rfp_pat_ptr_decode(uns32 pat_ptr, uns* set, uns* way) {
  ASSERT(0, pat_ptr < shadow_rfp_pat_num_entries());
  *set = pat_ptr / SHADOW_RFP_PAT_NUM_WAYS;
  *way = pat_ptr % SHADOW_RFP_PAT_NUM_WAYS;
}

static inline Addr shadow_rfp_entry_va(const Shadow_Rfp_Pt_Entry* entry) {
  uns set;
  uns way;

  shadow_rfp_pat_ptr_decode(entry->pat_ptr, &set, &way);
  ASSERT(0, shadow_rfp_pat[set][way].valid);
  return (shadow_rfp_pat[set][way].pfn << SHADOW_RFP_PAGE_OFFSET_BITS) | entry->page_offset;
}

static int shadow_rfp_pat_lookup_ptr(Addr pfn) {
  uns set = shadow_rfp_pat_set(pfn);

  for (uns way = 0; way < SHADOW_RFP_PAT_NUM_WAYS; way++) {
    if (shadow_rfp_pat[set][way].valid && shadow_rfp_pat[set][way].pfn == pfn)
      return (int)shadow_rfp_pat_ptr_encode(set, way);
  }
  return -1;
}

static uns32 shadow_rfp_pat_alloc_ptr(Addr pfn) {
  int ptr = shadow_rfp_pat_lookup_ptr(pfn);
  if (ptr >= 0)
    return (uns32)ptr;

  uns set = shadow_rfp_pat_set(pfn);

  for (uns way = 0; way < SHADOW_RFP_PAT_NUM_WAYS; way++) {
    if (!shadow_rfp_pat[set][way].valid) {
      shadow_rfp_pat[set][way].valid = TRUE;
      shadow_rfp_pat[set][way].pfn = pfn;
      STAT_EVENT(0, SHADOW_RFP_PAT_ALLOC);
      return shadow_rfp_pat_ptr_encode(set, way);
    }
  }

  /* Should not happen at these sizes for real traces; kept only so an
   * undersized configuration fails loudly instead of silently corrupting a
   * stored address. */
  STAT_EVENT(0, SHADOW_RFP_PAT_EVICT);
  shadow_rfp_pat[set][0].valid = TRUE;
  shadow_rfp_pat[set][0].pfn = pfn;
  return shadow_rfp_pat_ptr_encode(set, 0);
}

static void shadow_rfp_entry_set_va(Shadow_Rfp_Pt_Entry* entry, Addr va) {
  uns32 pat_ptr = shadow_rfp_pat_alloc_ptr(shadow_rfp_va_pfn(va));

  entry->pat_ptr = pat_ptr;
  entry->page_offset = shadow_rfp_va_page_offset(va);
}

void shadow_rfp_init(void) {
  ASSERT(0, SHADOW_RFP_PT_NUM_SETS > 0 && SHADOW_RFP_PT_NUM_WAYS > 0);
  ASSERT(0, SHADOW_RFP_PAT_NUM_SETS > 0 && SHADOW_RFP_PAT_NUM_WAYS > 0);
  ASSERT(0, SHADOW_RFP_PT_NUM_SETS <= SHADOW_RFP_PT_MAX_SETS && SHADOW_RFP_PT_NUM_WAYS <= SHADOW_RFP_PT_MAX_WAYS);
  ASSERT(0, SHADOW_RFP_PAT_NUM_SETS <= SHADOW_RFP_PAT_MAX_SETS && SHADOW_RFP_PAT_NUM_WAYS <= SHADOW_RFP_PAT_MAX_WAYS);
  ASSERT(0, (SHADOW_RFP_PT_NUM_SETS & (SHADOW_RFP_PT_NUM_SETS - 1)) == 0);
  ASSERT(0, (SHADOW_RFP_PAT_NUM_SETS & (SHADOW_RFP_PAT_NUM_SETS - 1)) == 0);

  shadow_rfp_set_index_mask = SHADOW_RFP_PT_NUM_SETS - 1;
  shadow_rfp_pc_tag_shift = SHADOW_RFP_PC_SET_SHIFT + shadow_rfp_log2_u(SHADOW_RFP_PT_NUM_SETS);

  memset(shadow_rfp_pt, 0, sizeof(shadow_rfp_pt));
  memset(shadow_rfp_pat, 0, sizeof(shadow_rfp_pat));
}

static inline uns shadow_rfp_set(Addr pc) {
  Addr index = pc >> SHADOW_RFP_PC_SET_SHIFT;
  return (uns)(index & shadow_rfp_set_index_mask);
}

static inline uns16 shadow_rfp_tag(Addr pc) {
  Addr tag = pc >> shadow_rfp_pc_tag_shift;
  return (uns16)(tag & SHADOW_RFP_TAG_MASK);
}

static int shadow_rfp_lookup(uns set, uns16 tag) {
  for (int way = 0; way < (int)SHADOW_RFP_PT_NUM_WAYS; way++)
    if (shadow_rfp_pt[set][way].valid && shadow_rfp_pt[set][way].tag == tag)
      return way;
  return -1;
}

static inline Shadow_Rfp_Pt_Entry* shadow_rfp_get_entry(Addr pc) {
  uns set = shadow_rfp_set(pc);
  uns16 tag = shadow_rfp_tag(pc);
  int way = shadow_rfp_lookup(set, tag);
  if (way < 0)
    return NULL;
  return &shadow_rfp_pt[set][way];
}

static inline void shadow_rfp_inflight_inc(Addr pc) {
  Shadow_Rfp_Pt_Entry* entry = shadow_rfp_get_entry(pc);
  if (!entry)
    return;
  if (entry->inflight < SHADOW_RFP_INFLIGHT_MAX)
    entry->inflight++;
}

static inline void shadow_rfp_inflight_dec(Addr pc) {
  Shadow_Rfp_Pt_Entry* entry = shadow_rfp_get_entry(pc);
  if (!entry)
    return;
  if (entry->inflight > 0)
    entry->inflight--;
}

/* Utility-based victim, identical policy to real RFP's rfp_victim. At these
 * enlarged sizes this should only ever hit the "empty entry" branch for our
 * traces; the utility fallback is kept so an undersized configuration
 * degrades the same way RFP's real PT does, rather than silently. */
static int shadow_rfp_victim(uns set) {
  for (int way = 0; way < (int)SHADOW_RFP_PT_NUM_WAYS; way++)
    if (!shadow_rfp_pt[set][way].valid)
      return way;

  int victim = 0;
  for (int way = 1; way < (int)SHADOW_RFP_PT_NUM_WAYS; way++)
    if (shadow_rfp_pt[set][way].utility < shadow_rfp_pt[set][victim].utility)
      victim = way;
  return victim;
}

/* Ported from rfp_train_entry, unchanged algorithm. */
static void shadow_rfp_train_entry(uns8 proc_id, Addr pc, Addr va) {
  uns set = shadow_rfp_set(pc);
  uns16 tag = shadow_rfp_tag(pc);
  int way = shadow_rfp_lookup(set, tag);

  if (way < 0) {
    way = shadow_rfp_victim(set);
    Shadow_Rfp_Pt_Entry* entry = &shadow_rfp_pt[set][way];
    entry->tag = tag;
    shadow_rfp_entry_set_va(entry, va);
    entry->stride = 0;
    entry->confidence = 0;
    entry->utility = 0;
    entry->inflight = 0;
    entry->last_update_reason = SHADOW_RFP_UPDATE_FRESH_ALLOC;
    entry->valid = TRUE;
    STAT_EVENT(proc_id, SHADOW_RFP_PT_ALLOC);
    return;
  }

  Shadow_Rfp_Pt_Entry* entry = &shadow_rfp_pt[set][way];
  int64 observed_stride = (int64)((int64)va - (int64)shadow_rfp_entry_va(entry));

  if (!shadow_rfp_stride_in_range(observed_stride)) {
    entry->last_update_reason = SHADOW_RFP_UPDATE_STRIDE_OUT_OF_RANGE;
    entry->confidence = 0;
    entry->utility = 0;
    shadow_rfp_entry_set_va(entry, va);
    STAT_EVENT(proc_id, SHADOW_RFP_STRIDE_OUT_OF_RANGE_EVENTS);
    return;
  }

  if (observed_stride == entry->stride) {
    entry->last_update_reason = SHADOW_RFP_UPDATE_CONFIRM;
    if (entry->confidence < SHADOW_RFP_CONF_MAX) {
      if ((rand() & ((1u << SHADOW_RFP_PROB_SHIFT) - 1)) == 0) {
        entry->confidence++;
        STAT_EVENT(proc_id, SHADOW_RFP_CONF_INC);
      }
    }
    if (entry->utility < 3)
      entry->utility++;
  } else {
    entry->last_update_reason = SHADOW_RFP_UPDATE_STRIDE_CHANGED;
    entry->stride = (int8)observed_stride;
    entry->confidence = 0;
    entry->utility = 0;
  }

  shadow_rfp_entry_set_va(entry, va);
}

/* Ported from rfp_predict, extended to also report *why* not (reason) and
 * the raw confidence/stride, so a caller can attribute a non-prediction
 * without re-deriving it from the table. */
static Flag shadow_rfp_lookup_and_classify(Addr pc, Addr* predicted_addr,
                                           uns8* out_confidence, int8* out_stride,
                                           Shadow_Rfp_Reason* out_reason) {
  uns set = shadow_rfp_set(pc);
  uns16 tag = shadow_rfp_tag(pc);
  int way = shadow_rfp_lookup(set, tag);

  if (way < 0) {
    *out_confidence = 0;
    *out_stride = 0;
    *out_reason = SHADOW_RFP_REASON_NO_HISTORY;
    return FALSE;
  }

  Shadow_Rfp_Pt_Entry* entry = &shadow_rfp_pt[set][way];
  *out_confidence = entry->confidence;
  *out_stride = entry->stride;

  if (entry->confidence < SHADOW_RFP_CONF_MAX) {
    switch (entry->last_update_reason) {
      case SHADOW_RFP_UPDATE_STRIDE_OUT_OF_RANGE:
        *out_reason = SHADOW_RFP_REASON_STRIDE_OUT_OF_RANGE;
        break;
      case SHADOW_RFP_UPDATE_STRIDE_CHANGED:
        *out_reason = SHADOW_RFP_REASON_STRIDE_CHANGED;
        break;
      case SHADOW_RFP_UPDATE_FRESH_ALLOC:
        *out_reason = SHADOW_RFP_REASON_NO_HISTORY;
        break;
      case SHADOW_RFP_UPDATE_CONFIRM:
      default:
        *out_reason = SHADOW_RFP_REASON_NOT_CONFIDENT;
        break;
    }
    return FALSE;
  }

  int64 lookahead = (int64)entry->inflight + 1;
  *predicted_addr = (Addr)((int64)shadow_rfp_entry_va(entry) + (int64)entry->stride * lookahead);
  *out_reason = SHADOW_RFP_REASON_NONE;
  return TRUE;
}

void shadow_rfp_predict_at_rename(Op* op) {
  ASSERT(op->proc_id, op);

  shadow_rfp_ensure_init();

  op->shadow_rfp_predicted = FALSE;
  op->shadow_rfp_predicted_addr = 0;
  op->shadow_rfp_confidence = 0;
  op->shadow_rfp_stride = 0;
  op->shadow_rfp_reason = SHADOW_RFP_REASON_NO_HISTORY;

  if (op->off_path || op->inst_info->table_info.mem_type != MEM_LD)
    return;

  Addr pc = op->inst_info->addr;
  Addr predicted_addr = 0;
  uns8 confidence = 0;
  int8 stride = 0;
  Shadow_Rfp_Reason reason = SHADOW_RFP_REASON_NO_HISTORY;

  Flag predicted = shadow_rfp_lookup_and_classify(pc, &predicted_addr, &confidence, &stride, &reason);

  op->shadow_rfp_predicted = predicted;
  op->shadow_rfp_predicted_addr = predicted_addr;
  op->shadow_rfp_confidence = confidence;
  op->shadow_rfp_stride = stride;
  op->shadow_rfp_reason = reason;

  /* Same ordering as rfp_prefetch_launch: the prediction check reads
   * inflight BEFORE this instance adds itself to it. */
  shadow_rfp_inflight_inc(pc);
}

void shadow_rfp_train_retire(Op* op) {
  ASSERT(op->proc_id, op);

  if (op->inst_info->table_info.mem_type != MEM_LD)
    return;

  shadow_rfp_inflight_dec(op->inst_info->addr);

  if (!op->oracle_info.va)
    return;

  shadow_rfp_train_entry(op->proc_id, op->inst_info->addr, op->oracle_info.va);
}

void shadow_rfp_track_squash(Op* op) {
  ASSERT(op->proc_id, op);

  if (op->off_path || op->inst_info->table_info.mem_type != MEM_LD)
    return;

  shadow_rfp_inflight_dec(op->inst_info->addr);
}

void shadow_rfp_record_ifuse_outcome(Op* op) {
  ASSERT(op->proc_id, op);

  STAT_EVENT(op->proc_id, IFUSE_TOTAL_PREDICTED_LOADS);

  if (op->shadow_rfp_predicted) {
    if (op->shadow_rfp_predicted_addr == op->oracle_info.va) {
      STAT_EVENT(op->proc_id, IFUSE_AND_RFP_PREDICTED);
    } else {
      STAT_EVENT(op->proc_id, RFP_CONFIDENT_BUT_WRONG);
      STAT_EVENT(op->proc_id, IFUSE_ONLY_PREDICTED);
    }
    return;
  }

  STAT_EVENT(op->proc_id, IFUSE_ONLY_PREDICTED);
  switch (op->shadow_rfp_reason) {
    case SHADOW_RFP_REASON_NO_HISTORY:
      STAT_EVENT(op->proc_id, RFP_NO_HISTORY);
      break;
    case SHADOW_RFP_REASON_STRIDE_OUT_OF_RANGE:
      STAT_EVENT(op->proc_id, RFP_STRIDE_OUT_OF_RANGE);
      break;
    case SHADOW_RFP_REASON_STRIDE_CHANGED:
      STAT_EVENT(op->proc_id, RFP_STRIDE_CHANGED);
      break;
    case SHADOW_RFP_REASON_NOT_CONFIDENT:
    default:
      STAT_EVENT(op->proc_id, RFP_NOT_CONFIDENT);
      break;
  }
}
