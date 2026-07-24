#ifndef __RFP_H__
#define __RFP_H__

#include "globals/global_types.h"
#include "op.h"
#include "prefetcher/rfp.param.h"

/* RFP is now PRF-only. Keep helper for readability at call sites. */
static inline Flag rfp_use_prf_model(void) {
  return RFP_ON;
}

/* Register File Prefetch (RFP) stride prefetch table. 1K entries, 8-way set
 * assosciative. */
#define RFP_NUM_SETS 128
#define RFP_NUM_WAYS 8
#define RFP_TAG_BITS 16
#define RFP_TAG_MASK ((1u << RFP_TAG_BITS) - 1)

/* PC bits used to pick a set vs tag (8-way set-assoc, 128 sets, 16-bit tag). */
#define RFP_PC_SET_SHIFT 2
#define RFP_PC_TAG_SHIFT 9
#define RFP_SET_INDEX_MASK (RFP_NUM_SETS - 1)

/* 1-bit confidence counter (ISCA'22 default; Table 1 budgets 3b of storage). */
#define RFP_CONF_MAX 1

/* Probabilistic confidence increment: P(inc) = 1 / 2^RFP_PROB_SHIFT. = 1/16 */
#define RFP_PROB_SHIFT 4

/* ISCA'22 §3.5 Table 1 — compressed PT entry fields. */
#define RFP_STRIDE_BITS 5
#define RFP_STRIDE_MIN (-(1 << (RFP_STRIDE_BITS - 1)))
#define RFP_STRIDE_MAX ((1 << (RFP_STRIDE_BITS - 1)) - 1)

#define RFP_PAT_NUM_SETS 16
#define RFP_PAT_NUM_WAYS 4
#define RFP_PAT_NUM_ENTRIES (RFP_PAT_NUM_SETS * RFP_PAT_NUM_WAYS)
#define RFP_PAT_PTR_BITS 6
#define RFP_PAGE_OFFSET_BITS 12
#define RFP_PAGE_OFFSET_MASK ((1u << RFP_PAGE_OFFSET_BITS) - 1)
#define RFP_PFN_BITS 44
#define RFP_PFN_MASK (((Addr)1 << RFP_PFN_BITS) - 1)

typedef struct RFP_Pat_Entry_struct {
  Flag valid;
  Addr pfn; /* 44-bit page frame number (VA bits [51:12] on 4 KiB pages). */
} RFP_Pat_Entry;

typedef struct RFP_PT_Entry_struct {
  uns16 tag;         /* 16-bit tag. */
  uns8 pat_ptr;      /* 6-bit PAT pointer (flat index into rfp_pat[][]). */
  uns16 page_offset; /* 12-bit in-page byte offset. */
  int8 stride;       /* 5-bit signed byte stride. */
  uns8 confidence;   /* 1-bit confidence (3b in Table 1 storage budget). */
  uns8 utility;      /* 2-bit replacement counter. Incremented until 3. */
  uns8 inflight;     /* 7-bit outstanding-instance counter. */
  Flag valid;
} RFP_PT_Entry;

void rfp_init(void);
Flag rfp_predict(Addr pc, Addr* predicted_addr);
void rfp_predict_at_fetch(Op* op);
void rfp_prefetch_launch(Op* op);
void rfp_train_retire(Op* op);
void rfp_track_squash(Op* op);

#endif
