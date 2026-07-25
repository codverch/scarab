#ifndef __SHADOW_RFP_H__
#define __SHADOW_RFP_H__

/*
 * Shadow RFP: an oracle copy of the RFP (Register File Prefetch, ISCA'22)
 * stride predictor that runs online during an I-Fuse simulation purely to
 * answer "would RFP have predicted this same dynamic load?"
 *
 * This module NEVER issues a prefetch, NEVER touches the cache/memory model,
 * and NEVER affects timing or I-Fuse. It only observes the same rename /
 * retire / squash lifecycle every load already goes through, and maintains
 * its own private prediction table.
 *
 * The stride-learning, confidence, and prediction algorithm below is ported
 * verbatim from src/prefetcher/rfp.c (branch hpca2027-rfp) -- rfp_train_entry
 * and rfp_predict -- with the PRF/launch/timing machinery removed (nothing
 * here ever launches a request) and two intentional differences, both scoped
 * to removing capacity/replacement limits for an "ideal-capacity" oracle:
 *
 *   1. PT/PAT set/way counts are configured far larger than any realistic
 *      number of unique static load PCs / resident pages, so eviction should
 *      not occur in practice. This required no algorithm change -- table
 *      size was already a parameter.
 *
 *   2. RFP's real PAT entry pointer (rfp_pt[].pat_ptr) is a 1-byte field,
 *      hard-capping the PAT at 256 resident pages system-wide, independent of
 *      any size parameter. Here that field is widened to 32 bits so the
 *      shadow PAT can track arbitrarily many resident pages. This is the one
 *      deliberate divergence from "identical to RFP's hardware model" --
 *      confirmed acceptable since this predictor is not meant to be
 *      hardware-realistic.
 *
 * Everything else -- the 1-bit confidence counter, the probabilistic 1/16
 * confidence increment, the 5-bit signed stride field and its range check,
 * the utility-based PT victim policy, and the inflight-based prediction
 * lookahead (entry_va + stride * (inflight + 1)) -- is unchanged.
 */

#include "globals/global_types.h"
#include "op.h" /* also brings in the Shadow_Rfp_Reason enum */
#include "prefetcher/shadow_rfp.param.h"

void shadow_rfp_init(void);

/* Call at rename, for every on-path op (mem or not -- non-loads are ignored
 * internally). Snapshots the shadow-RFP verdict for this dynamic load onto
 * the op (shadow_rfp_predicted / shadow_rfp_predicted_addr /
 * shadow_rfp_confidence / shadow_rfp_stride / shadow_rfp_reason) and bumps
 * the per-PC inflight counter used for prediction lookahead. Read-only with
 * respect to timing/memory; only touches the shadow table and the op. */
void shadow_rfp_predict_at_rename(Op* op);

/* Call at retire, for every on-path op. Trains the shadow table from the
 * op's oracle address (mirrors rfp_train_retire's unconditional training)
 * and decrements the inflight counter. Must be called after the rename-time
 * snapshot above was already taken for this same dynamic instance, so this
 * instance's own retirement can never influence its own prediction. */
void shadow_rfp_train_retire(Op* op);

/* Call on squash (pipeline recovery), for every on-path op being squashed.
 * Decrements the inflight counter, mirroring rfp_track_squash. */
void shadow_rfp_track_squash(Op* op);

/* Call exactly when I-Fuse has successfully fused this dynamic load (i.e.
 * where the caller already knows op is the retiring, fused-path load).
 * Compares the rename-time shadow-RFP snapshot on op against
 * op->oracle_info.va and fires the IFUSE_*/RFP_* coverage-overlap stats
 * (see shadow_rfp.stat.def). Read-only: fires stats only, never mutates
 * timing or the shadow table itself. */
void shadow_rfp_record_ifuse_outcome(Op* op);

#endif /* __SHADOW_RFP_H__ */
