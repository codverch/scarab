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

/* 1-bit confidence counter */
#define RFP_CONF_MAX 1

/* Probabilistic confidence increment: P(inc) = 1 / 2^RFP_PROB_SHIFT. = 1/16 */
#define RFP_PROB_SHIFT 4

typedef struct RFP_PT_Entry_struct {
  uns16 tag;        /* Tag for the entry. */
  Addr last_addr;   /* Last address accessed by the load. */
  int64 stride;     /* Stride value of the last address accessed. */
  uns8 confidence;  /* 1-bit confidence counter. Incremented until 1. */
  uns8 utility;     /* 2-bit replacement counter. Incremented until 3. */
  uns8 inflight;    /* Outstanding dynamic instances */
  Flag valid;       /* Valid bit. */
} RFP_PT_Entry;

void rfp_init(void);
Flag rfp_predict(Addr pc, Addr* predicted_addr);
void rfp_predict_at_fetch(Op* op);
void rfp_prefetch_launch(Op* op);
void rfp_train_retire(Op* op);
void rfp_track_squash(Op* op);

#endif 
