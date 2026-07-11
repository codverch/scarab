/**
 * @file heliosFusion.c
 *
 * @brief File that provides the hooks injected at the FETCH and COMMIT stages. Provides
 * the necessary functionality to implement the Helios Fusion algorithm, given the FP and UCH
 * data structures created elsewhere. 
 *
 * @date 05/28/2026
 * @author Ved Lakshminarayanan <vlakshmi@andrew.cmu.edu>
 */

#include "heliosFusion.h"
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "statistics.h" 

#include "general.param.h"
#include "isa/isa_macros.h"
#include "globals/global_vars.h"  // cycle_count (head-driven tail completion timing)
#include "map.h"                   // wake_up_ops (tail self-produces + wakes its consumers)
#include "map_rename.h"            // reg_file_consume (balance finite-RF src accounting for the non-issuing tail)
#include "model.h"                 // model->wake_hook
#include "issue_queue.h"           // issue_queue_wakeup (release a held fused head onto the ready list)

LoadHeadTableEntry loadHeadTable[LOAD_HEAD_TABLE_SIZE];
StoreHeadTableEntry storeHeadTable[STORE_HEAD_TABLE_SIZE];
RegTrackEntry regTrackTable[REG_TRACK_TABLE_SIZE];
FusionIntervalEntry activeFusions[NCSF_RING_SIZE];

int regTrackTableHead = 0;
int activeFusionsHead = 0;

/**
 * @brief Running op_num of the LAST micro-instruction of the macro-instruction currently being fetched. Updated at
 * each bom micro-instruction and attached to every micro-instruction of the macro in heliosFetchHook. This allows a 
 * mis-fusion flush to keep the flushing op's whole macro regardless of which micro-instruction within the macro 
 * is the mis-fused memory op.
 */
static Counter heliosMacroLastOpNum[MAX_NUM_PROCS] = {0};

/**
 * @brief Initializes the fusion data structures. This includes the predictor
 * tables, the load and store UCHs, the load and store head tables, the register
 * tracking table, and the active fusions ring.
 * 
 * @param void
 * 
 * @return void
 */
void heliosFusionInit(void) {
    predictorInit();
    initLSUCH();

    memset(loadHeadTable, 0, sizeof(loadHeadTable));
    memset(storeHeadTable, 0, sizeof(storeHeadTable));
    memset(regTrackTable, 0, sizeof(regTrackTable));
    memset(activeFusions, 0, sizeof(activeFusions));
    regTrackTableHead = 0;
    activeFusionsHead = 0;
}

/**
 * @brief Find an invalid/outdated entry in the load head table and
 * populate it with a new load. 
 * 
 * @param op The operation to add to the load head table.
 * 
 * @return void
 */
void addLoadHead(Op *op) {

    // If the operation is not a load, return.
    if (op->inst_info->table_info.mem_type != MEM_LD) {
        return;
    }
    
    // If a valid version of the operation already exists in the load head table, return.
    for (int i = 0; i < LOAD_HEAD_TABLE_SIZE; i++) {
        if (loadHeadTable[i].valid && (loadHeadTable[i].globalMicroOpNumber == op->globalMicroOpNumber)) {
            return;
        }
    }

    uint64_t cacheBlockAddress = getCacheBlockAddress(op->oracle_info.va);
    uint8_t loadSize = op->oracle_info.mem_size;

    int slotToPopulate = 0;
    uint64_t oldestMicroOpNumber = UINT64_MAX;

    // Find an invalid/outdated entry to populate.
    for (int i = 0; i < LOAD_HEAD_TABLE_SIZE; i++) {
        if (!loadHeadTable[i].valid) {
            slotToPopulate = i;
            break;
        }
        if (loadHeadTable[i].globalMicroOpNumber < oldestMicroOpNumber) {
            oldestMicroOpNumber = loadHeadTable[i].globalMicroOpNumber;
            slotToPopulate = i;
        }
    }

    LoadHeadTableEntry *entry = &loadHeadTable[slotToPopulate];
    entry->globalMicroOpNumber = op->globalMicroOpNumber;
    entry->cacheBlockAddress = cacheBlockAddress;
    entry->memoryAddress = op->oracle_info.va;
    entry->accessSize = loadSize;
    entry->valid = true;
    entry->headConsumed = false;
    entry->fused = false;
    entry->headOp = op;
}

/**
 * @brief Find an invalid/outdated entry in the store head table and
 * populate it with a new store. 
 * 
 * @param op The operation to add to the store head table.
 * 
 * @return void
 */
void addStoreHead(Op *op) {

    // If the operation is not a store, return.
    if (op->inst_info->table_info.mem_type != MEM_ST) {
        return;
    }

    // If a valid version of the operation already exists in the store head table, return.
    for (int i = 0; i < STORE_HEAD_TABLE_SIZE; i++) {
        if (storeHeadTable[i].valid && (storeHeadTable[i].globalMicroOpNumber == op->globalMicroOpNumber)) {
            return;
        }
    }

    uint64_t cacheBlockAddress = getCacheBlockAddress(op->oracle_info.va);

    int slotToPopulate = 0;
    uint64_t oldestMicroOpNumber = UINT64_MAX;

    // Find an invalid/outdated entry to populate.
    for (int i = 0; i < STORE_HEAD_TABLE_SIZE; i++) {
        if (!storeHeadTable[i].valid) {
            slotToPopulate = i;
            break;
        }
        if (storeHeadTable[i].globalMicroOpNumber < oldestMicroOpNumber) {
            oldestMicroOpNumber = storeHeadTable[i].globalMicroOpNumber;
            slotToPopulate = i;
        }
    }

    StoreHeadTableEntry *entry = &storeHeadTable[slotToPopulate];
    entry->globalMicroOpNumber = op->globalMicroOpNumber;
    entry->cacheBlockAddress = cacheBlockAddress;
    entry->memoryAddress = op->oracle_info.va;
    entry->accessSize = op->oracle_info.mem_size;
    entry->valid = true;
    entry->headConsumed = false;
    entry->fused = false;
    entry->headOp = op;
}

/**
 * @brief Track an operation's source and destination registers.
 * 
 * @param op The operation to add to the store head table.
 * 
 * @return void
 */
void trackRegWrites(Op *op) {
    bool isSerializing = (op->inst_info->table_info.bar_type != NOT_BAR);

    if (op->inst_info->table_info.num_dest_regs == 0 && !isSerializing) {
        return;
    }

    // Collect the operation's number of destination and source registers. If
    // the number of registers is greater than the maximum allowed, set it to the maximum.
    uint8_t numDest = op->inst_info->table_info.num_dest_regs;
    uint8_t numSrc  = op->inst_info->table_info.num_src_regs;
    if (numDest > MAX_FUSION_REGS) numDest = MAX_FUSION_REGS;
    if (numSrc  > MAX_FUSION_REGS) numSrc  = MAX_FUSION_REGS;

    RegTrackEntry *entry = &regTrackTable[regTrackTableHead];
    entry->globalMicroOpNumber = op->globalMicroOpNumber;
    entry->numDestRegs = numDest;
    entry->numSrcRegs = numSrc;
    entry->isSerializing = isSerializing;

    // Copy the operation's destination and source registers to the register tracking table entry.
    for (int i = 0; i < numDest; i++) {
        entry->destRegs[i] = op->inst_info->dests[i].id;
    }

    for (int i = 0; i < numSrc; i++) {
        entry->srcRegs[i] = op->inst_info->srcs[i].id;
    }

    entry->valid = true;
    regTrackTableHead = (regTrackTableHead + 1) % REG_TRACK_TABLE_SIZE;
}

/**
 * @brief A deadlock exists if the TAIL depends, directly or transitively through the catalyst, 
 * on the HEAD's output. This function seeds the tag on the head's destinations, propagates it
 * through source and destination registers of the catalyst, and reports a deadlock
 * if any tail source carries the tag. Store heads write to no registers, so a store pair
 * can never deadlock this way.
 * 
 * @param tailOp The tail of the fusion pair.
 * @param headMicroOpNumber The global micro-operation number of the head of the fusion pair.
 * 
 * @return Boolean indicating if a deadlock exists.
 */
bool checkDeadlock(Op *tailOp, uint64_t headMicroOpNumber) {
    uint64_t tailMicroOpNumber = tailOp->globalMicroOpNumber;

    // Array used to track the HEAD's destination registers. Later propagated and 
    // tainted by catalyst operations until the tail is reached.
    bool taint[NUM_REG_IDS];
    memset(taint, 0, sizeof(taint));

    // Loop through the register tracking table and seed the head's destination registers. Also, 
    // accumulate the catalyst operations between the head and the tail.
    int catalyst[HELIOS_FUSION_WINDOW];
    int numCatalyst = 0;

    for (int i = 0; i < REG_TRACK_TABLE_SIZE; i++) {
        RegTrackEntry *e = &regTrackTable[i];

        if (!e->valid) {
            continue;
        }

        if (e->globalMicroOpNumber == headMicroOpNumber) {
            for (int d = 0; d < e->numDestRegs && d < MAX_FUSION_REGS; d++) {
                uint16_t r = e->destRegs[d];
                if (r < NUM_REG_IDS) {
                    taint[r] = true;
                }
            }
        } 
        
        else if (e->globalMicroOpNumber > headMicroOpNumber &&
                   e->globalMicroOpNumber < tailMicroOpNumber &&
                   numCatalyst < HELIOS_FUSION_WINDOW) {
            catalyst[numCatalyst++] = i;
        }
    }

    // Insertion sort the catalyst operations by globalMicroOpNumber.
    for (int a = 1; a < numCatalyst; a++) {
        int key = catalyst[a];
        uint64_t keyNum = regTrackTable[key].globalMicroOpNumber;
        int b = a - 1;
        while (b >= 0 && regTrackTable[catalyst[b]].globalMicroOpNumber > keyNum) {
            catalyst[b + 1] = catalyst[b];
            b--;
        }
        catalyst[b + 1] = key;
    }

    // Propagate the tag forward. Each catalyst op taints its destinations if one of its sources
    // are tainted. If none of its sources are tainted, the catalyst op clears the tag on its destinations.
    for (int c = 0; c < numCatalyst; c++) {
        RegTrackEntry *e = &regTrackTable[catalyst[c]];
        
        bool opTainted = false;
        for (int s = 0; s < e->numSrcRegs && s < MAX_FUSION_REGS; s++) {
            uint16_t r = e->srcRegs[s];
            if (r < NUM_REG_IDS && taint[r]) { 
                opTainted = true; 
                break; 
            }
        }
        for (int d = 0; d < e->numDestRegs && d < MAX_FUSION_REGS; d++) {
            uint16_t r = e->destRegs[d];
            if (r < NUM_REG_IDS) {
                taint[r] = opTainted;
            }
        }
    }

    // Deadlock if any of the tail's sources carries the head's tag.
    int numTailSrc = (int)tailOp->inst_info->table_info.num_src_regs;
    for (int s = 0; s < numTailSrc && s < MAX_SRCS; s++) {
        uint16_t r = tailOp->inst_info->srcs[s].id;
        if (r < NUM_REG_IDS && taint[r]) return true;
    }
    return false;
}

/**
 * @brief Checks if a serializing/barrier instruction lies in the catalyst.
 * 
 * @param headMicroOpNumber The global micro-operation number of the head of the fusion pair.
 * @param tailMicroOpNumber The global micro-operation number of the tail of the fusion pair.
 * 
 * @return Boolean indicating if a serializing/barrier instruction lies in the catalyst.
 */
static bool serializingInCatalyst(uint64_t headMicroOpNumber, uint64_t tailMicroOpNumber) {
    for (int i = 0; i < REG_TRACK_TABLE_SIZE; i++) {
        RegTrackEntry *e = &regTrackTable[i];

        if (e->valid && e->isSerializing &&
            e->globalMicroOpNumber > headMicroOpNumber &&
            e->globalMicroOpNumber < tailMicroOpNumber) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Count the number of already-validated fusions whose [head,tail] interval overlaps 
 * [head,tail] of the candidate pair. Intervals overlap if a.head < b.tail && b.head < a.tail. 
 * If two fusions already overlap with the candidate, then the candidate cannot fuse as it exceeds
 * the NCSF nesting cap. 
 *
 * This is a slightly conservative implementation that counts pairwise overlaps with the candidate. 
 * There is a possibility that very few fusions are disregarded by this implementation. The reason 
 * for this simplification is that the number of nested fusions allowed is small and the distance between
 * fusable micro-operations is minute, so the difference is negligible.
 * 
 * @param headMicroOpNumber The global micro-operation number of the head of the fusion pair.
 * @param tailMicroOpNumber The global micro-operation number of the tail of the fusion pair.
 * 
 * @return Int indicating the number of already-validated fusions whose [head,tail] interval overlaps 
 * [head,tail] of the candidate pair.
 */
static int countOverlappingFusions(uint64_t headMicroOpNumber, uint64_t tailMicroOpNumber) {
    int count = 0;
    for (int i = 0; i < NCSF_RING_SIZE; i++) {
        FusionIntervalEntry *e = &activeFusions[i];
        if (e->valid &&
            e->headMicroOpNumber < tailMicroOpNumber &&
            headMicroOpNumber < e->tailMicroOpNumber) {
            count++;
        }
    }
    return count;
}

/**
 * @brief Record a validated fusion's interval so later candidates can see it for nest-overlap counting.
 * 
 * @param headMicroOpNumber The global micro-operation number of the head of the fusion pair.
 * @param tailMicroOpNumber The global micro-operation number of the tail of the fusion pair.
 * 
 * @return void
 */
static void recordFusion(uint64_t headMicroOpNumber, uint64_t tailMicroOpNumber) {
    FusionIntervalEntry *e = &activeFusions[activeFusionsHead];
    e->headMicroOpNumber = headMicroOpNumber;
    e->tailMicroOpNumber = tailMicroOpNumber;
    e->valid = true;
    activeFusionsHead = (activeFusionsHead + 1) % NCSF_RING_SIZE;
}

/* A fused head is "holdable" (can still be delayed for its tail's operands) only while it is still
 * pre-issue: in the frontend / ROB / RS but not yet woken onto the ready list. Op_State is ordered
 * FETCHED < IN_ROB < IN_RS < (ready/scheduled/...) so state <= OS_IN_RS && !in_rdy_list captures it. */
static inline Flag heliosHeadHoldable(Op *headOp) {
    return headOp && headOp->state <= OS_IN_RS && !headOp->in_rdy_list;
}

/* paper NCS_Ready: mark a just-fused head to be held pre-issue until its tail maps (see the dispatch
 * gate in issue_queue.cc and the release in heliosWaitTailSrcs). No-op if the head already issued or
 * the feature is off; in that case the optimistic residue is counted at the tail's map. */
static void heliosHoldFusedHead(Op *headOp) {
    if (!HELIOS_FUSED_WAIT_TAIL_SRCS)
        return;
    if (heliosHeadHoldable(headOp))
        headOp->fusedHeadPendingTail = TRUE;
}

/* Release a held fused head: splice the TAIL's REG_DATA_DEP source producers into the head's wake-up
 * deps so the fused single access waits for them too (paper: the fused micro-op carries both nucleii's
 * sources). Runs at the tail's MAP (= paper Rename). If the head already issued despite the hold (rare
 * large-distance race), count the optimistic residue instead. Called only with a live head. */
static void heliosWaitTailSrcs(Op *tail, Op *head) {
    if (!HELIOS_FUSED_WAIT_TAIL_SRCS || !head || !head->fusedHeadPendingTail)
        return;

    head->fusedHeadPendingTail = FALSE;  // hold ends here regardless of outcome (prevents any hang)

    if (!heliosHeadHoldable(head)) {
        STAT_EVENT(head->proc_id, HELIOS_FUSED_HEAD_ALREADY_ISSUED);
        return;
    }

    for (uns i = 0; i < tail->num_srcs; i++) {
        if (tail->src_info[i].type != REG_DATA_DEP)
            continue;
        Op *producer = tail->src_info[i].op;
        if (!producer || !producer->op_pool_valid || producer->unique_num != tail->src_info[i].unique_num)
            continue;
        // Age-safety: only make the (older) head wait on an OLDER producer. Waiting on a YOUNGER
        // catalyst producer inverts RS age-order and can deadlock Scarab's in-order-fill scheduler
        // (older head stuck in RS waiting for a younger op that can't get an RS slot). Skip those --
        // the catalyst-RaW timing for that rare DBR pair stays optimistic (counted). SBR pairs share
        // the base (deduped); DBR pairs with an older base producer are still modeled.
        if (producer->op_num >= head->op_num) {
            STAT_EVENT(head->proc_id, HELIOS_FUSED_TAIL_SRC_YOUNGER_SKIPPED);
            continue;
        }
        helios_wire_fused_reg_src(head, producer);
    }
    STAT_EVENT(head->proc_id, HELIOS_FUSED_HEAD_HELD_FOR_TAIL);

    // The head sat in the RS held; if its sources are now all satisfied, nothing else will wake it,
    // so push it onto the ready list now (mirrors cmp_wake's tail logic).
    if (head->state == OS_IN_RS && op_sources_not_rdy_is_clear(head) &&
        cycle_count >= head->issue_cycle && !head->in_rdy_list) {
        issue_queue_wakeup(head);
    }
}

/* Squash-safety: release a held head whose tail was squashed before it could map (so heliosWaitTailSrcs
 * will never run for it). Called from reopenSurvivingHead on recovery. Just lifts the hold and, if the
 * head is sitting ready in the RS, pushes it onto the ready list (no tail srcs to add -- the tail is
 * gone). Safe during cmp_recover: flushTables runs before recover_issue_queue, which only frees flushed
 * (younger) entries, so this surviving head's RS entry persists. */
static void heliosReleaseHeldHead(Op *head) {
    if (!head || !head->fusedHeadPendingTail)
        return;
    head->fusedHeadPendingTail = FALSE;
    if (head->state == OS_IN_RS && op_sources_not_rdy_is_clear(head) &&
        cycle_count >= head->issue_cycle && !head->in_rdy_list) {
        issue_queue_wakeup(head);
    }
}

/* Deadlock-breaker for the NCS_Ready hold. A held fused head sits in the RS un-issued until its tail
 * maps. If such a head reaches the ROB head before its tail has mapped, it can never issue -> the ROB
 * can't drain -> Map stalls -> the tail can never map to release it (forward-progress deadlock, seen on
 * low-IPC server workloads). node_retire calls this when the oldest op is blocked: forfeit the
 * tail-source wait for this one op (rare; oracle values stay correct) so it issues and the ROB drains. */
void heliosUnblockRobHead(Op *op) {
    if (op && op->fusedHeadPendingTail) {
        STAT_EVENT(op->proc_id, HELIOS_FUSED_HEAD_FORCE_RELEASED);
        heliosReleaseHeldHead(op);
    }
}

/* Extended commit group (paper IV-B3): a fused HEAD at the ROB head must not retire until its whole
 * group -- the catalyst ops between head and tail, and the tail -- are all ready to retire, so a
 * catalyst fault/mispredict can still unfuse/flush the not-yet-retired head (precise exceptions).
 * Returns TRUE if the head must be held. Called from node_retire only when the op is the ROB head and
 * has itself passed op_not_ready_for_retire. Walks the in-order ROB list from the head:
 *   - block if any op up to the tail is not yet ready (still executing / recovery pending), or
 *   - block if the chain ends before reaching the tail (tail not yet dispatched into the ROB).
 * Deadlock-free: in-order dispatch => ROB-full implies the tail (a few ops younger) is already in the
 * ROB; and gating retirement never blocks execution, so the catalyst still completes and frees the head. */
Flag heliosExtCommitGroupBlocks(Op *head) {
    if (!head->headConsumed || head->fusedGroupLastOpNum <= head->op_num)
        return FALSE;  // not an active fused-group head
    Counter last = head->fusedGroupLastOpNum;
    Flag blocked = FALSE;
    Flag reached_tail = FALSE;
    // Scan the WHOLE group [head..tail]. Two jobs: (1) decide if any member is not yet ready, and
    // (2) release any member that is itself a fused head still in the NCS_Ready hold. A held member
    // can never become ready on its own while we stall this group -- its tail can't map if Map is
    // backed up behind the full ROB -- so leaving it held deadlocks (ecg x hold interaction). We must
    // scan the whole range (not early-return at the first not-ready op) so a held head is still found
    // and released even when an earlier member that depends on it is the one that's not ready.
    for (Op *o = head->next_node; o != NULL; o = o->next_node) {
        if (o->fusedHeadPendingTail)
            heliosUnblockRobHead(o);  // forfeit that op's tail-source wait so it can issue
        if (!(o->state == OS_DONE || OP_DONE(o)) || o->recovery_scheduled || o->redirect_scheduled)
            blocked = TRUE;
        if (o->op_num >= last) { reached_tail = TRUE; break; }
    }
    if (!reached_tail)
        return TRUE;        // tail not yet in the ROB -> group incomplete -> hold the head
    return blocked;
}

/**
 * @brief Validate a fusion prediction. If the prediction is correct, the pair is fused. If the prediction is incorrect,
 * the pair is either unfused in place or a pipeline flush is triggered. 
 * 
 * @param op The operation to validate. It is the TAIL of the fusion pair.
 * 
 * @return void
 */
void validatePrediction(Op *op) {

    // Op must be a candidate for fusion.
    if (!op->isFusionCandidate) {
        return;
    }

    uint64_t tailMicroOpNumber = op->globalMicroOpNumber;
    
    if (op->predictedDistanceToHead == 0 || op->predictedDistanceToHead > tailMicroOpNumber) {
        op->isPredictionCorrect = false;
        STAT_EVENT(op->proc_id, HELIOS_REJECT_DISTANCE_INVALID);
        return;
    }

    uint64_t headMicroOpNumber = tailMicroOpNumber - op->predictedDistanceToHead;
    uint64_t tailAddress       = op->oracle_info.va;
    uint8_t  tailSize          = op->oracle_info.mem_size;
    Mem_Type memType           = op->inst_info->table_info.mem_type;
    op->partnerMicroOpNumber   = headMicroOpNumber;

    // Gate: store-store fusion can be disabled independently (HELIOS_FUSE_STORES=0) while load
    // pairs keep fusing. This is the single funnel that sets isPredictionCorrect/op->fused, so
    // rejecting here guarantees no store pair is ever formed, dep-linked, or executed-as-fused.
    if (memType == MEM_ST && !HELIOS_FUSE_STORES) {
        op->isPredictionCorrect = false;
        STAT_EVENT(op->proc_id, HELIOS_REJECT_TYPE_DISABLED);
        return;
    }

    // If the operation is a load, check if it is viable for fusion.
    if (memType == MEM_LD) {
        LoadHeadTableEntry *head = NULL;
        for (int i = 0; i < LOAD_HEAD_TABLE_SIZE; i++) {
            if (loadHeadTable[i].valid && loadHeadTable[i].globalMicroOpNumber == headMicroOpNumber) {
                head = &loadHeadTable[i];
                break;
            }
        }

        // If no head is found, the head was likely evicted from the Allocation Queue. If the
        // head has already been fused, then the tail operation in question cannot fuse.
        if (head == NULL) {
            op->isPredictionCorrect = false;
            op->fusionHeadEvicted = true;
            STAT_EVENT(op->proc_id, HELIOS_REJECT_HEAD_EVICTED);
            return;
        }
        if (head->headConsumed) {
            op->isPredictionCorrect = false;
            op->headAlreadyFused = true;
            STAT_EVENT(op->proc_id, HELIOS_REJECT_HEAD_ALREADY_FUSED);
            return;
        }

        // Check if the fusion pair exceeds the NCSF nesting cap, or if it is blocked by a deadlock 
        // or serializing operation. If so, the pair is unfused in place. No flush is needed and 
        // no FP penalty is incurred.
        if (countOverlappingFusions(headMicroOpNumber, tailMicroOpNumber) >= MAX_NCSF_NEST) {
            op->isPredictionCorrect = false;
            op->fusionNestLimited = true;
            STAT_EVENT(op->proc_id, HELIOS_REJECT_NEST_LIMIT);
            return;
        }
        if (checkDeadlock(op, headMicroOpNumber)) {      
            op->isPredictionCorrect = false;
            op->fusionBlockedByDeadlock = true;
            STAT_EVENT(op->proc_id, HELIOS_REJECT_DEADLOCK);
            return;
        }
        if (serializingInCatalyst(headMicroOpNumber, tailMicroOpNumber)) { 
            op->isPredictionCorrect = false;
            op->fusionBlockedBySerializing = true;
            STAT_EVENT(op->proc_id, HELIOS_REJECT_SERIALIZING);
            return;
        }

        // Trigger a pipeline flush if the pair of micro operations access non-contiguous memory regions/do
        // not hit the same/adjacent cachelines.
        if (!canFuse(tailAddress, head->memoryAddress, tailSize, head->accessSize)) {
            op->isPredictionCorrect = false;
            op->fusionAddressMisprediction = true;  
            op->logForFlushing = true;
            STAT_EVENT(op->proc_id, HELIOS_REJECT_ADDR_MISMATCH);
            return;
        }

        // Helios (MICRO'22, Sec. IV-B4): NCS load-pair fusion MAY have loads and stores in its
        // catalyst -- loads already execute speculatively out-of-order with respect to stores while
        // respecting sequential semantics, so a store->load (RAW) hazard is resolved by the normal
        // memory-disambiguation / store-to-load-forwarding path, NOT by a preemptive fusion flush.
        // The only memory-related flush the paper takes for a load pair is a genuine memory-dependency
        // misprediction (its repair Case 7) -- identical to what an unfused load would incur. In
        // Scarab that ordering is enforced by oracle store dependences (map.c::add_store_deps, gated
        // by --mem_obey_store_dep), which the fused tail still acquires, so the tail's value stays
        // correct without a flush. Hence no store-hazard check here. (Store-pair fusion still unfuses
        // in place on a catalyst store below, matching the paper's NCSF_StorePair handling.)

        // Record the fusion pair so it can be counted for nest-overlap checking.
        recordFusion(headMicroOpNumber, tailMicroOpNumber);
        head->headConsumed = true;
        head->fused = true;
        op->fusedHeadOp = head->headOp; // Tie the tail to the head as a dependency.
        if (head->headOp && head->headOp->op_num == head->globalMicroOpNumber) {
            head->headOp->headConsumed = true; // Mark the head as consumed so it is excluded from the UCH.
            head->headOp->fusedGroupLastOpNum = tailMicroOpNumber;  // extended-commit-group bound
            heliosHoldFusedHead(head->headOp); // paper NCS_Ready: hold head pre-issue until tail maps.
        }
        op->isPredictionCorrect = true;
    }

    // If the operation is a store, check if it is viable for fusion.
    else if (memType == MEM_ST) {
        StoreHeadTableEntry *head = NULL;
        for (int i = 0; i < STORE_HEAD_TABLE_SIZE; i++) {
            if (storeHeadTable[i].valid && storeHeadTable[i].globalMicroOpNumber == headMicroOpNumber) {
                head = &storeHeadTable[i];
                break;
            }
        }
        
        // If no head is found, the head was likely evicted from the Allocation Queue. If the
        // head has already been fused, then the tail operation in question cannot fuse.
        if (head == NULL) {
            op->isPredictionCorrect = false;
            op->fusionHeadEvicted = true;
            STAT_EVENT(op->proc_id, HELIOS_REJECT_HEAD_EVICTED);
            return;
        }
        if (head->headConsumed) {
            op->isPredictionCorrect = false;
            op->headAlreadyFused = true;
            STAT_EVENT(op->proc_id, HELIOS_REJECT_HEAD_ALREADY_FUSED);
            return;
        }

        // Check if the fusion pair exceeds the NCSF nesting cap, or if it is blocked by a deadlock 
        // or serializing operation. If so, the pair is unfused in place. No flush is needed and 
        // no FP penalty is incurred.
        if (countOverlappingFusions(headMicroOpNumber, tailMicroOpNumber) >= MAX_NCSF_NEST) {
            op->isPredictionCorrect = false;
            op->fusionNestLimited = true;
            STAT_EVENT(op->proc_id, HELIOS_REJECT_NEST_LIMIT);
            return;
        }
        if (checkDeadlock(op, headMicroOpNumber)) {        
            op->isPredictionCorrect = false;
            op->fusionBlockedByDeadlock = true;
            STAT_EVENT(op->proc_id, HELIOS_REJECT_DEADLOCK);
            return;
        }
        if (serializingInCatalyst(headMicroOpNumber, tailMicroOpNumber)) { 
            op->isPredictionCorrect = false;
            op->fusionBlockedBySerializing = true;
            STAT_EVENT(op->proc_id, HELIOS_REJECT_SERIALIZING);
            return;
        }

        // Unfuse the pair in place if an intermediate store exists in the fusion pair's catalyst.
        for (int i = 0; i < STORE_HEAD_TABLE_SIZE; i++) {
            StoreHeadTableEntry *st = &storeHeadTable[i];
            if (st->valid &&
                st->globalMicroOpNumber > headMicroOpNumber &&
                st->globalMicroOpNumber < tailMicroOpNumber) {
                op->isPredictionCorrect = false;
                op->fusionBlockedByStore = true;
                STAT_EVENT(op->proc_id, HELIOS_REJECT_STORE_HAZARD);
                return;
            }
        }

        // Trigger a pipeline flush if the pair of micro operations access non-contiguous memory regions/do
        // not hit the same/adjacent cachelines.
        if (!canFuse(tailAddress, head->memoryAddress, tailSize, head->accessSize)) {
            op->isPredictionCorrect = false;
            op->fusionAddressMisprediction = true; 
            op->logForFlushing = true;
            STAT_EVENT(op->proc_id, HELIOS_REJECT_ADDR_MISMATCH);
            return;
        }

        // Record the fusion pair so it can be counted for nest-overlap checking.
        recordFusion(headMicroOpNumber, tailMicroOpNumber);
        head->headConsumed = true;
        head->fused = true;
        op->fusedHeadOp = head->headOp; // Tie the tail to the head as a dependency.
        if (head->headOp && head->headOp->op_num == head->globalMicroOpNumber) {
            head->headOp->headConsumed = true;       // exclude the head from the UCH
            head->headOp->fusedGroupLastOpNum = tailMicroOpNumber;  // extended-commit-group bound
            // paper NCS_Ready: hold the store head pre-issue until the tail maps and contributes its
            // source operands. Scarab stores carry their DATA register as a REG_DATA_DEP source and
            // do not split STA/STD, so the fused store's single access correctly waits for the tail
            // store's data (SBR base is shared -> deduped). Store heads produce no dest reg, so this
            // can never form a self-wait.
            heliosHoldFusedHead(head->headOp);
        }
        op->isPredictionCorrect = true;
    }
}

/**
 * @brief Re-open a fusion head whose tail was squashed by a recovery so it can fuse again.
 *
 * A catalyst-region flush squashes the tail but keeps the older head. The head's table entry would
 * otherwise keep headConsumed=true and -- because op_num rewinds on recovery (icache_stage.c) -- a
 * re-fetched tail can resolve back to this head and be wrongly rejected with
 * HELIOS_REJECT_HEAD_ALREADY_FUSED. Clearing the consumed state lets the surviving head fuse again.
 * The head lives in exactly one of the two head tables.
 *
 * @param headMicroOpNumber The global micro-operation number of the surviving head to re-open.
 *
 * @return void
 */
static void reopenSurvivingHead(uint64_t headMicroOpNumber) {
    for (int i = 0; i < LOAD_HEAD_TABLE_SIZE; i++) {
        LoadHeadTableEntry *h = &loadHeadTable[i];
        if (h->valid && h->globalMicroOpNumber == headMicroOpNumber) {
            h->headConsumed = false;
            h->fused = false;
            if (h->headOp && h->headOp->op_pool_valid && h->headOp->op_num == h->globalMicroOpNumber) {
                h->headOp->headConsumed = false;
                h->headOp->fusedGroupLastOpNum = 0;  // group dissolved (tail squashed) -> no retire gate
                heliosReleaseHeldHead(h->headOp); // its tail was squashed; lift the NCS_Ready hold.
            }
            return;
        }
    }
    for (int i = 0; i < STORE_HEAD_TABLE_SIZE; i++) {
        StoreHeadTableEntry *h = &storeHeadTable[i];
        if (h->valid && h->globalMicroOpNumber == headMicroOpNumber) {
            h->headConsumed = false;
            h->fused = false;
            if (h->headOp && h->headOp->op_pool_valid && h->headOp->op_num == h->globalMicroOpNumber) {
                h->headOp->headConsumed = false;
                h->headOp->fusedGroupLastOpNum = 0;  // group dissolved (tail squashed) -> no retire gate
                heliosReleaseHeldHead(h->headOp); // its tail was squashed; lift the NCS_Ready hold.
            }
            return;
        }
    }
}

/**
 * @brief Invalidate all entries in the load, store, register tracking, and fusion interval tables
 * that are newer than the global micro-operation number of the operation to flush till.
 *
 * @param globalMicroOpNumber The global micro-operation number of the operation to flush till.
 *
 * @return void
 */
void flushTables(uint64_t globalMicroOpNumber) {
    for (int i = 0; i < LOAD_HEAD_TABLE_SIZE; i++) {
        LoadHeadTableEntry *entry = &loadHeadTable[i];
        if (entry->valid && (entry->globalMicroOpNumber > globalMicroOpNumber)) {
            entry->valid = false;
        }
    }
    for (int i = 0; i < STORE_HEAD_TABLE_SIZE; i++) {
        StoreHeadTableEntry *entry = &storeHeadTable[i];
        if (entry->valid && (entry->globalMicroOpNumber > globalMicroOpNumber)) {
            entry->valid = false;
        }
    }

    for (int i = 0; i < REG_TRACK_TABLE_SIZE; i++) {
        RegTrackEntry *entry = &regTrackTable[i];
        if (entry->valid && (entry->globalMicroOpNumber > globalMicroOpNumber)) {
            entry->valid = false;
        }
    }

    for (int i = 0; i < NCSF_RING_SIZE; i++) {
        FusionIntervalEntry *entry = &activeFusions[i];
        if (entry->valid && (entry->tailMicroOpNumber > globalMicroOpNumber)) {
            // The tail is squashed by this recovery. If the head is older than the recovery point it
            // survives, so re-open it -- otherwise its headConsumed stays stale and a re-fetched tail
            // (op_num rewinds on recovery) is wrongly rejected as HELIOS_REJECT_HEAD_ALREADY_FUSED.
            if (entry->headMicroOpNumber <= globalMicroOpNumber)
                reopenSurvivingHead(entry->headMicroOpNumber);
            entry->valid = false;
        }
    }
}

/**
 * @brief Check for a fusion candidate.
 *
 * @param op The operation to check for fusion candidates.
 * 
 * @return void
 */
void checkFusionCandidates(Op *op) {

    // Operation must be a load or store.
    if ((op->inst_info->table_info.mem_type != MEM_LD) && (op->inst_info->table_info.mem_type != MEM_ST)) {
        return;
    }

    // Capture the sub-predictor's decisions and tie it to this op, so the selector can be trained
    // at a later point in time against the validated outcome (updateSelector).
    bool localPredicts = false;
    bool globalPredicts = false;
    uint16_t predictedDistance = predictFusion(op->inst_info->addr, op->bp_pred_info->pred_global_hist,
                                               &localPredicts, &globalPredicts);
    op->localFusionPrediction  = localPredicts;
    op->globalFusionPrediction = globalPredicts;

    // Verify that the fusion distance is in accordance with the allowed range. 
    if (predictedDistance > 0 && predictedDistance <= HELIOS_FUSION_WINDOW) {
        op->isFusionCandidate = true;
        op->predictedDistanceToHead = predictedDistance;
    }
}

/**
 * @brief Ties a validated fused tail to its head as a dependency, so the tail waits in the IQ/SQ.
 *
 * @param tailOp The TAIL of the fusion pair.
 * 
 * @return void
 */
void heliosAddFusionDep(Op *tailOp) {

    // Tail must be fused.
    if (!tailOp->fused)
        return;

    Mem_Type memType = tailOp->inst_info->table_info.mem_type;

    // Is the recorded head still the same live in-flight Op? (head tables are global, so the
    // proc_id check rejects a cross-proc "match"; the wake-up machinery requires same-proc.)
    Op *head = tailOp->fusedHeadOp;
    Flag headLive = (head && head->op_pool_valid &&
                     head->op_num == tailOp->partnerMicroOpNumber &&
                     head->proc_id == tailOp->proc_id);

    // paper NCS_Ready: now that the tail has been renamed (its src producers are known), make the
    // fused single access also wait for the tail's source operands, and release the held head. Runs
    // before the head-driven completion below (which is purely about the head's cache resolution).
    if (headLive)
        heliosWaitTailSrcs(tailOp, head);

    // ---- STORE tail: paper-faithful head-driven timing (TWO-SITE) ----
    // A fused store pair is one micro-op doing a single cache access. The tail does NOT issue
    // (fusedNoIssue) and carries NO synthetic dep. Unlike a load, a store is a MEM_ADDR_DEP +
    // MEM_DATA_DEP *source* for younger ops, and those deps must fire at the head's EXEC (address
    // gen) -- NOT at head->done_cycle, which would wrongly serialize younger store-ordering
    // dependents behind a head cache MISS. The tail's own retire gating (done_cycle/dcache_cycle,
    // node_precommit/op_not_ready_for_retire) is satisfied at the head's cache-resolution. Two
    // cycles => two head-driver sites:
    //   (A) head EXEC   -> heliosWakeFusedStoreTailDeps: wake tail MEM_ADDR/DATA_DEP + reg_file_consume
    //   (B) head DCACHE -> heliosCompleteFusedTail (store branch): stamp done_cycle/dcache_cycle
    // Here at the tail's map stage we cover the races where the head has already passed (A) and/or (B).
    if (memType == MEM_ST) {
        tailOp->fusedNoIssue = TRUE;

        // headLive==FALSE => head retired/recycled: it has both executed and resolved (data is in
        // the cache). Otherwise probe the head's progress via its own signals.
        Flag headExecuted = !headLive || head->wake_up_signaled[MEM_ADDR_DEP];  // head's AGU fired
        Flag headResolved = !headLive || head->done_cycle != MAX_CTR;           // head's single access resolved

        // (A) MEM-dep delivery, in program order (address known at the head's EXEC). Do it now iff
        // the head's exec hook already passed (or the head is gone), so the deferred hook can never
        // fire for this just-installed link. The non-issuing tail read its srcs at rename
        // (onpath_consumers_num++) but never runs exec's reg_file_consume; consume + stamp
        // sched_cycle here so commit and squash accounting stay balanced (mirrors exec_stage.c:254-259).
        if (headExecuted && !tailOp->fusedStoreDepsWoken) {
            tailOp->wake_cycle  = headLive ? MAX2(cycle_count, head->wake_cycle) : cycle_count;
            tailOp->sched_cycle = cycle_count;
            reg_file_consume(tailOp);
            wake_up_ops(tailOp, MEM_ADDR_DEP, model->wake_hook);
            wake_up_ops(tailOp, MEM_DATA_DEP, model->wake_hook);
            tailOp->fusedStoreDepsWoken = TRUE;
        }

        // (B) completion / retire gating, at the head's single cache access.
        if (headResolved) {
            tailOp->dcache_cycle       = cycle_count;
            tailOp->done_cycle         = headLive ? MAX2(cycle_count, head->done_cycle) : cycle_count;
            tailOp->state              = OS_SCHEDULED;
            tailOp->fusedTailCompleted = TRUE;
            STAT_EVENT(tailOp->proc_id, HELIOS_STORE_TAIL_IMMEDIATE_COMPLETE);
        } else {
            // Head live and not yet resolved (possibly not yet executed): install the back-channel.
            // The head's exec hook delivers (A) if not already done; its dcache hook delivers (B).
            head->fusedTailOp        = tailOp;
            head->fusedTailUniqueNum = tailOp->unique_num;
        }
        return;
    }

    // ---- LOAD tail: paper-faithful head-driven timing ----
    // The fused load pair performs ONE cache access that retrieves both registers' worth of data;
    // both destination registers are delivered to their dependents when that single access
    // resolves (head->done_cycle). So the tail does NOT issue to exec/dcache and does NOT carry a
    // synthetic dependency (the synthetic dep used to serialize issue AND time the wait; removing
    // it AND removing the tail from the issue path is strictly safer -- the tail can no longer
    // perturb the exec->dcache memory-ordering handshake that previously double-woke a load head).
    tailOp->fusedNoIssue = TRUE;

    if (headLive && !head->wake_up_signaled[REG_DATA_DEP]) {
        // Head still in flight and not yet produced: it completes this tail at its resolution
        // site (dcache hit / fill / store-forward / perfect). Install the back-channel.
        head->fusedTailOp        = tailOp;
        head->fusedTailUniqueNum = tailOp->unique_num;
        return;
    }

    // Head already produced (data available now) OR head retired/recycled (data is in the cache).
    // Complete the tail immediately, IN PROGRAM ORDER: it has just been renamed (its dst physical
    // entry is in ALLOC), so producing now is legal and never out-of-order. Consumers attach right
    // after this (add_to_wake_up_lists) and take the already-signaled fast path.
    Counter ready = headLive ? MAX2(cycle_count, head->done_cycle) : cycle_count;
    tailOp->dcache_cycle       = cycle_count;
    tailOp->done_cycle         = ready;
    tailOp->wake_cycle         = ready;
    tailOp->state              = OS_SCHEDULED;
    tailOp->fusedTailCompleted = TRUE;
    STAT_EVENT(tailOp->proc_id, HELIOS_TAIL_IMMEDIATE_COMPLETE);
    // The non-issuing tail never runs exec's reg_file_consume, but it DID read its source registers
    // at rename (onpath_consumers_num++). Consume them now and stamp sched_cycle so both commit
    // (reg_file_release_prev) and squash (reg_file_flush_squash_helios's executed=sched_cycle!=MAX_CTR)
    // keep onpath_consumed_count == onpath_consumers_num balanced. Mirrors exec_stage.c:254-259.
    tailOp->sched_cycle        = cycle_count;
    reg_file_consume(tailOp);
    wake_up_ops(tailOp, REG_DATA_DEP, model->wake_hook);
}

/**
 * @brief Site (A) of head-driven STORE-tail timing. Called from the head store's EXEC dep-wakeup
 * (exec_count==0), right after the head wakes its OWN MEM_ADDR/DATA_DEP. If this store is the head
 * of a fused store pair, deliver the tail's address/data to younger ordering dependents at the same
 * single AGU cycle: wake the tail's MEM_ADDR_DEP + MEM_DATA_DEP and balance its finite-RF source
 * accounting. Does NOT stamp done_cycle and does NOT null the link -- the tail's retire gating is
 * completed later at the head's cache resolution (heliosCompleteFusedTail, site B). No-op for any op
 * that is not a fused store-pair head. Idempotent via fusedStoreDepsWoken (the map-stage immediate
 * path may have already delivered the deps).
 *
 * @param head The store op whose AGU just fired.
 */
void heliosWakeFusedStoreTailDeps(Op *head) {
    if (!HELIOS_DO_FUSION)
        return;

    Op *tail = head->fusedTailOp;
    if (!tail)
        return;

    // Safe-pointer: the tail may have been squashed/recycled since the link was installed.
    if (!tail->op_pool_valid || tail->unique_num != head->fusedTailUniqueNum ||
        tail->proc_id != head->proc_id) {
        head->fusedTailOp = NULL;
        return;
    }
    if (tail->inst_info->table_info.mem_type != MEM_ST)  // stores only on this path
        return;
    if (tail->fusedStoreDepsWoken)                       // already delivered (immediate path)
        return;

    tail->wake_cycle  = MAX2(cycle_count, head->wake_cycle);  // younger deps ready at the head's AGU
    tail->sched_cycle = cycle_count;
    reg_file_consume(tail);
    wake_up_ops(tail, MEM_ADDR_DEP, model->wake_hook);
    wake_up_ops(tail, MEM_DATA_DEP, model->wake_hook);
    tail->fusedStoreDepsWoken = TRUE;
}

/**
 * @brief Called from each dcache load-completion site after a head load wakes its OWN consumers.
 * If this op is the head of a fused load pair, deliver the tail's register at the same single
 * access: the tail self-produces its own physical register (in program order -- it was renamed
 * long ago, dst entry is ALLOC) and wakes its own consumers at head->done_cycle. No-op for any op
 * that is not a fused-pair head. Idempotent (a head reaches exactly one completion site, but the
 * guard makes double-drive harmless).
 *
 * @param head The load op that just resolved its cache access.
 */
void heliosCompleteFusedTail(Op *head) {
    if (!HELIOS_DO_FUSION)
        return;

    Op *tail = head->fusedTailOp;
    if (!tail)
        return;

    // Safe-pointer: the tail may have been squashed/recycled since the link was installed.
    if (!tail->op_pool_valid || tail->unique_num != head->fusedTailUniqueNum ||
        tail->proc_id != head->proc_id) {
        head->fusedTailOp = NULL;
        return;
    }
    if (tail->fusedTailCompleted) {   // already completed (immediate-complete path)
        head->fusedTailOp = NULL;
        return;
    }

    if (tail->inst_info->table_info.mem_type == MEM_ST) {
        // ---- STORE tail completion (site B): retire gating only ----
        // The deps (MEM_ADDR/DATA_DEP wake + reg_file_consume) were delivered at the head's EXEC
        // (heliosWakeFusedStoreTailDeps, site A) or at the tail's map stage. Here we only stamp the
        // tail's done_cycle/dcache_cycle to the head's single cache access so it can precommit/retire.
        // Defensive: if the head executed and resolved without site A ever running (e.g. a 0-cycle
        // dcache), deliver the deps now too -- still in program order, the head is older.
        if (!tail->fusedStoreDepsWoken) {
            tail->wake_cycle  = MAX2(cycle_count, head->wake_cycle);
            tail->sched_cycle = cycle_count;
            reg_file_consume(tail);
            wake_up_ops(tail, MEM_ADDR_DEP, model->wake_hook);
            wake_up_ops(tail, MEM_DATA_DEP, model->wake_hook);
            tail->fusedStoreDepsWoken = TRUE;
        }
        tail->dcache_cycle       = cycle_count;
        tail->done_cycle         = head->done_cycle;   // tail's write completes at the single access
        tail->state              = OS_SCHEDULED;
        tail->fusedTailCompleted = TRUE;
        head->fusedTailOp        = NULL;
        STAT_EVENT(head->proc_id, HELIOS_HEAD_DRIVEN_STORE_COMPLETIONS);
        return;
    }

    // ---- LOAD tail completion (single site) ----
    tail->dcache_cycle       = cycle_count;
    tail->done_cycle         = head->done_cycle;   // both registers ready at the single access
    tail->wake_cycle         = head->done_cycle;
    tail->state              = OS_SCHEDULED;
    tail->fusedTailCompleted = TRUE;
    head->fusedTailOp        = NULL;
    STAT_EVENT(head->proc_id, HELIOS_HEAD_DRIVEN_TAIL_COMPLETIONS);
    // Balance finite-RF source accounting: the tail read its srcs at rename but never ran exec's
    // reg_file_consume. Consume now + stamp sched_cycle so commit and squash stay balanced
    // (see the immediate-complete path above; mirrors exec_stage.c:254-259).
    tail->sched_cycle        = cycle_count;
    reg_file_consume(tail);
    wake_up_ops(tail, REG_DATA_DEP, model->wake_hook);
}

/**
 * @brief Function injected at the FETCH stage. Called once per on-path op. Consults the FP
 * for a fusion candidate and validates the prediction. If the prediction is correct, the pair is fused.
 * If the prediction is incorrect, the pair is either unfused in place or a pipeline flush is triggered.
 *
 * If it is not predicted as a tail, it becomes a potential head nucleus.
 *
 * @param op The operation to process.
 * 
 * @return void
 */
void heliosFetchHook(Op *op) {

    op->globalMicroOpNumber = op->op_num;

    // Record the op_num of the last micro-instruction of this op's macro-instruction. num_uop is the
    // macro's total micro-instruction count, so the last micro-instruction is at (bom op_num + num_uop - 1).
    // Attach it to every micro-instruction within the macro.
    if (op->bom) {
        heliosMacroLastOpNum[op->proc_id] = op->op_num + op->inst_info->trace_info.num_uop - 1;
    }
    op->helios_macro_last_op_num = heliosMacroLastOpNum[op->proc_id];

    // Reset the fusion outcome fields.
    op->isFusionCandidate          = false;
    op->predictedDistanceToHead    = 0;
    op->localFusionPrediction      = false;
    op->globalFusionPrediction     = false;
    op->isPredictionCorrect        = false;
    op->fusionAddressMisprediction = false;
    op->logForFlushing             = false;
    op->fused                      = false;
    op->headConsumed               = false;
    op->headAlreadyFused           = false;
    op->fusionHeadEvicted          = false;
    op->fusionBlockedByStore       = false;
    op->fusionBlockedByDeadlock    = false;
    op->fusionBlockedBySerializing = false;
    op->fusionNestLimited          = false;
    op->partnerMicroOpNumber       = 0;
    op->fusedHeadOp                = NULL;
    op->fusedTailOp                = NULL;
    op->fusedTailUniqueNum         = 0;
    op->fusedNoIssue               = false;
    op->fusedTailCompleted         = false;
    op->fusedStoreDepsWoken        = false;
    op->fusedHeadPendingTail       = false;
    op->fusedGroupLastOpNum        = 0;

    // Check for fusion candidates and validate the prediction.
    Mem_Type memType = op->inst_info->table_info.mem_type;
    if (memType == MEM_LD || memType == MEM_ST) {
        STAT_EVENT(op->proc_id, (memType == MEM_LD) ? HELIOS_ONPATH_LOADS : HELIOS_ONPATH_STORES);
        checkFusionCandidates(op);
        // Record which sub-predictor uniquely proposed fusion (selector arbitration insight).
        if (op->localFusionPrediction && !op->globalFusionPrediction)
            STAT_EVENT(op->proc_id, HELIOS_SELECTOR_LOCAL_WINS);
        else if (op->globalFusionPrediction && !op->localFusionPrediction)
            STAT_EVENT(op->proc_id, HELIOS_SELECTOR_GLOBAL_WINS);
        if (op->isFusionCandidate) {
            STAT_EVENT(op->proc_id, HELIOS_FUSION_CANDIDATES);
            validatePrediction(op);            
            if (op->isPredictionCorrect) {
                op->fused = true;
                STAT_EVENT(op->proc_id, HELIOS_FUSIONS);
            } 
            else if (HELIOS_ENABLE_FLUSHES && op->logForFlushing) {
                // Trigger a pipeline flush at EXECUTE stage. 
                op->bp_pred_info->recover_at_exec = true;
            }
        } 
        else {
            // Not predicted as a tail. This memory op becomes a potential head nucleus.
            if (memType == MEM_LD) {
                addLoadHead(op);
            }
            else {
                addStoreHead(op);
            }
        }
    }

    // Track register writes from every op so checkDeadlock can find the producers of a
    // tail's source registers.
    trackRegWrites(op);
}

/**
 * @brief Function injected at the COMMIT stage. Called once per on-path op. Retiring, unfused memory
 * operations search the UCH for an older same-cache-line partner; on a match the distance trains the Fusion Predictor 
 * for this (tail) PC, building the confidence the frontend later uses to predict. If a match is not found
 * add the operation to the UCH. Train the selector accordingly, given whether or not a pair was 
 * correctly/incorrectly predicted.
 *
 * @param op The retiring operation.
 * 
 * @return void
 */
void heliosCommit(Op *op) {
    Mem_Type memType = op->inst_info->table_info.mem_type;
    if (memType != MEM_LD && memType != MEM_ST) {
        return;
    }

    bool fpMispredict = op->fusionAddressMisprediction;

    // Lower the FP's confidence of this pair if the prediction is incorrect and was not handled in place.
    if (op->isFusionCandidate && fpMispredict) {
        STAT_EVENT(op->proc_id, HELIOS_FUSION_MISPREDICT);
        updatePredictor(op->inst_info->addr, op->bp_pred_info->pred_global_hist);
    }

    // Train the selector given the fusion outcome and the sub-predictor's decisions.
    if (op->isFusionCandidate && (op->isPredictionCorrect || fpMispredict))
        updateSelector(op->inst_info->addr, op->bp_pred_info->pred_global_hist,
                       op->localFusionPrediction, op->globalFusionPrediction,
                       op->isPredictionCorrect);

    // Only add unfused operations to the UCH.
    if (op->fused || op->headConsumed) {
        if (op->fused)
            STAT_EVENT(op->proc_id, HELIOS_FUSIONS_COMMITTED);
        return;
    }

    uint64_t headPC   = 0;
    uint16_t distance = 0;

    // Update the UCH with the new operation. Either find a possible fusion candidate in the UCH or add a new entry.
    if (memType == MEM_LD) {
        distance = updateLoadUCH(op->inst_info->addr, op->oracle_info.va,
                                (uint8_t)op->oracle_info.mem_size, op->op_num, &headPC);
    }
    else {
        distance = updateStoreUCH(op->inst_info->addr, op->oracle_info.va,
                                 (uint8_t)op->oracle_info.mem_size, op->op_num, &headPC);
    }

    // A real fuseable pair was confirmed at commit. Train the FP on the tail's PC,
    // using the global history captured when this op was predicted.
    if (distance > 0) {
        addInstructionToPredictor(op->inst_info->addr, op->bp_pred_info->pred_global_hist, distance);
    }
}