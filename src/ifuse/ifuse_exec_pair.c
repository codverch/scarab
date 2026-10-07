#include "ifuse_exec_pair.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ifuse_ideal_alloc.h"
#include "ifuse_ideal_limits.h"
#include "ifuse.param.h"
#include "../globals/global_defs.h"
#include "../globals/global_vars.h"
#include "../map_rename.h"
#include "../op_info.h"
#include "../statistics.h"

/**
 * Execution-side state for one validated LOAD1-to-LOAD2 pair.
 *
 * APT is removed when LOAD2 reaches rename because register ownership has
 * transferred. This buffer lives longer: it remembers whether LOAD1 has
 * completed and whether LOAD2 has reached map, so either arrival order works.
 */
typedef struct IFuse_Exec_Pair {
    Counter ld1_op_num;
    Op*     ld2_op;
    Counter ld1_wake_cycle;
    Flag    ld1_completed;
    /*
     * The fused access reads LD2's word from its own L1-D bank, so that word
     * needs a read port on its bank, just as a separate LD2 would. If the
     * port is busy, only LD2's word retries (dcache_stage.c).
     */
    Flag    ld2_word_requested;
    Flag    ld2_word_done;
    Flag    ld2_word_hit;
    Counter ld2_word_ready_cycle;
    struct IFuse_Exec_Pair* next;
} IFuse_Exec_Pair;

static IFuse_Exec_Pair** ifuse_exec_pair_table = NULL;
static unsigned int      ifuse_exec_pair_num_buckets = 0;
static unsigned int      ifuse_exec_pair_bucket_mask = 0;
static IfuseIdealAlloc   ifuse_exec_pair_alloc;
static bool              ifuse_exec_pair_initialized = false;
static uint64_t          ifuse_exec_pair_live_count = 0;
static uint64_t          ifuse_exec_pair_live_peak = 0;

static bool ifuse_exec_pair_validate_table_params(void) {
    if (IFUSE_EXEC_PAIR_NUM_BUCKETS == 0U ||
        IFUSE_EXEC_PAIR_NUM_BUCKETS > IFUSE_IDEAL_MAX_BUCKETS ||
        (IFUSE_EXEC_PAIR_NUM_BUCKETS & (IFUSE_EXEC_PAIR_NUM_BUCKETS - 1U)) != 0U) {
        fprintf(stderr,
                "IFuse exec pair: ifuse_exec_pair_num_buckets must be a power "
                "of two <= %u\n",
                IFUSE_IDEAL_MAX_BUCKETS);
        return false;
    }
    if (IFUSE_EXEC_PAIR_MAX_NODES == 0U ||
        IFUSE_EXEC_PAIR_MAX_NODES > IFUSE_IDEAL_MAX_NODES) {
        fprintf(stderr,
                "IFuse exec pair: ifuse_exec_pair_max_nodes must be in [1, %u]\n",
                IFUSE_IDEAL_MAX_NODES);
        return false;
    }
    return true;
}

static void ifuse_exec_pair_note_inserted(void) {
    ifuse_exec_pair_live_count++;
    if (ifuse_exec_pair_live_count > ifuse_exec_pair_live_peak) {
        INC_STAT_EVENT(0, IFUSE_INFLIGHT_FUSED_PAIRS_PEAK,
                       ifuse_exec_pair_live_count - ifuse_exec_pair_live_peak);
        ifuse_exec_pair_live_peak = ifuse_exec_pair_live_count;
    }
}

static void ifuse_exec_pair_note_removed(void) {
    if (ifuse_exec_pair_live_count > 0) {
        ifuse_exec_pair_live_count--;
    }
}

static unsigned int ifuse_exec_pair_bucket(Counter ld1_op_num) {
    uint64_t h = (uint64_t)ld1_op_num;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (unsigned int)(h & ifuse_exec_pair_bucket_mask);
}

static void ifuse_exec_pair_init(void) {
    if (ifuse_exec_pair_initialized) {
        return;
    }

    if (!ifuse_exec_pair_validate_table_params()) {
        exit(1);
    }

    ifuse_exec_pair_num_buckets = IFUSE_EXEC_PAIR_NUM_BUCKETS;
    ifuse_exec_pair_bucket_mask = ifuse_exec_pair_num_buckets - 1U;
    ifuse_exec_pair_table = (IFuse_Exec_Pair**)calloc(
        ifuse_exec_pair_num_buckets, sizeof(IFuse_Exec_Pair*));
    if (!ifuse_exec_pair_table) {
        fprintf(stderr, "IFuse exec pair: calloc failed for %u buckets\n",
                ifuse_exec_pair_num_buckets);
        exit(1);
    }

    memset(ifuse_exec_pair_table, 0,
           ifuse_exec_pair_num_buckets * sizeof(IFuse_Exec_Pair*));
    if (!ifuse_ideal_alloc_init_fixed(&ifuse_exec_pair_alloc,
                                      sizeof(IFuse_Exec_Pair),
                                      IFUSE_EXEC_PAIR_MAX_NODES)) {
        fprintf(stderr, "IFuse exec pair buffer: fixed pool alloc failed\n");
        exit(1);
    }
    ifuse_exec_pair_initialized = true;
}

static IFuse_Exec_Pair* ifuse_exec_pair_find(Counter ld1_op_num) {
    unsigned int bucket = ifuse_exec_pair_bucket(ld1_op_num);

    for (IFuse_Exec_Pair* pair = ifuse_exec_pair_table[bucket];
         pair;
         pair = pair->next) {
        if (pair->ld1_op_num == ld1_op_num) {
            return pair;
        }
    }
    return NULL;
}

static IFuse_Exec_Pair* ifuse_exec_pair_get_or_insert(Counter ld1_op_num) {
    IFuse_Exec_Pair* pair = ifuse_exec_pair_find(ld1_op_num);
    if (pair) {
        return pair;
    }

    pair = (IFuse_Exec_Pair*)ifuse_ideal_alloc_get(&ifuse_exec_pair_alloc);
    if (!pair) {
        return NULL;
    }

    memset(pair, 0, sizeof(*pair));
    pair->ld1_op_num = ld1_op_num;

    unsigned int bucket = ifuse_exec_pair_bucket(ld1_op_num);
    pair->next = ifuse_exec_pair_table[bucket];
    ifuse_exec_pair_table[bucket] = pair;
    ifuse_exec_pair_note_inserted();
    return pair;
}

static void ifuse_exec_pair_remove(Counter ld1_op_num) {
    unsigned int bucket = ifuse_exec_pair_bucket(ld1_op_num);
    IFuse_Exec_Pair* prev = NULL;
    IFuse_Exec_Pair* pair = ifuse_exec_pair_table[bucket];

    while (pair) {
        if (pair->ld1_op_num == ld1_op_num) {
            if (prev) {
                prev->next = pair->next;
            } else {
                ifuse_exec_pair_table[bucket] = pair->next;
            }
            ifuse_ideal_alloc_put(&ifuse_exec_pair_alloc, pair);
            ifuse_exec_pair_note_removed();
            return;
        }
        prev = pair;
        pair = pair->next;
    }
}

/*
 * A squashed LOAD2 can remain pool-valid briefly through FT ownership while a
 * re-fetched LOAD2 has already claimed the same surviving LOAD1. Remove the
 * pair only if it still belongs to the LOAD2 object being freed.
 */
static void ifuse_exec_pair_remove_ld2_if_current(const Op* ld2_op) {
    IFuse_Exec_Pair* pair =
        ifuse_exec_pair_find(ld2_op->ifuse_partner_op_num);

    if (pair && pair->ld2_op == ld2_op) {
        ifuse_exec_pair_remove(pair->ld1_op_num);
    }
}

static void ifuse_exec_pair_record_prefetch_lead(const Op* ld2_op) {
    Counter ld1_wake = ld2_op->wake_cycle;
    Counter ld2_agu = ld2_op->dcache_cycle;
    Counter prefetch_lead = 0;

    STAT_EVENT(ld2_op->proc_id, IFUSE_PREFETCH_LEAD_OPPORTUNITIES);

    if (ld1_wake != MAX_CTR && ld2_agu != MAX_CTR && ld2_agu > ld1_wake) {
        prefetch_lead = ld2_agu - ld1_wake;
        STAT_EVENT(ld2_op->proc_id, IFUSE_PREFETCH_LEAD_EVENTS);
        INC_STAT_EVENT(ld2_op->proc_id, IFUSE_PREFETCH_LEAD_CYCLES,
                       prefetch_lead);
        INC_STAT_EVENT(ld2_op->proc_id, IFUSE_AVG_PREFETCH_LEAD,
                       prefetch_lead);
    }

    if (ld2_op->ifuse_pair_distance > 0) {
        INC_STAT_EVENT(ld2_op->proc_id, IFUSE_PAIR_DISTANCE_TOTAL,
                       ld2_op->ifuse_pair_distance);
        INC_STAT_EVENT(ld2_op->proc_id, IFUSE_AVG_PAIR_DISTANCE,
                       ld2_op->ifuse_pair_distance);
    }
}

static void ifuse_exec_pair_finalize_ld2(Op* ld2_op) {
    Counter serve_done;

    if (!ld2_op->ifuse_ld2_early_wake_signaled ||
        !ld2_op->ifuse_ld2_agu_completed) {
        return;
    }
    /* Avoid double-counting if both wake and AGU paths call finalize. */
    if (ld2_op->state == OS_DONE) {
        return;
    }

    // The fused LOAD2 is complete only after its data path and AGU accounting
    // have both completed. It can now retire normally from the ROB.
    serve_done = MAX2(ld2_op->wake_cycle, ld2_op->dcache_cycle);

    /* Observed readiness only (no assumed L1-hit "cycles saved"). */
    if (ld2_op->wake_cycle > ld2_op->dcache_cycle) {
        STAT_EVENT(ld2_op->proc_id, IFUSE_PARTIAL_MITIGATED);
        INC_STAT_EVENT(ld2_op->proc_id, IFUSE_REMAINING_WAIT_AFTER_AGU,
                       ld2_op->wake_cycle - ld2_op->dcache_cycle);
    } else {
        STAT_EVENT(ld2_op->proc_id, IFUSE_FULL_MITIGATED);
    }

    ifuse_exec_pair_record_prefetch_lead(ld2_op);

    ld2_op->done_cycle = serve_done;
    ld2_op->state = OS_DONE;
}

/**
 * Models the fused datapath: LOAD2 gets its data from LOAD1's memory access
 * rather than issuing its own cache access, and its dependents may wake
 * IFUSE_LD2_WAKE_DELAY cycles after LOAD1's data arrives.
 *
 * LOAD1 performs the single fused memory access, and that access returns both
 * LOAD1's and LOAD2's data. An ideal datapath could hand LOAD2's half to its
 * consumers in the same cycle as LOAD1's half. Real hardware probably cannot:
 * the second value has to be split off the wide access and written into a
 * separate physical register before the scheduler can broadcast its tag.
 * --ifuse_ld2_wake_delay (IFUSE_LD2_WAKE_DELAY, default 1) charges cycles for
 * that extra step.
 *
 * Example: if LOAD1's data is ready in cycle 100, LOAD1's consumers can issue
 * in cycle 100 and LOAD2's consumers in cycle 101. Setting it to 0 recovers
 * the original same-cycle model.
 */
static void ifuse_exec_pair_signal_ld2(
    IFuse_Exec_Pair* pair, void (*wake_action)(Op*, Op*, uns)) {
    Op* ld2_op = pair->ld2_op;

    if (!ld2_op || !ld2_op->op_pool_valid || ld2_op->off_path ||
        ld2_op->ifuse_recovery_squashed ||
        ld2_op->ifuse_load_role != LOAD2 ||
        ld2_op->ifuse_ld2_early_wake_signaled) {
        return;
    }

    // The memory access itself still happens at LOAD1's time, so exec_cycle
    // stays at LOAD1's wake cycle. Only the moment LOAD2's value becomes
    // visible to consumers moves later. That works because every dependent
    // computes its ready time as rdy_cycle = MAX2(rdy_cycle, wake_cycle) in
    // simple_wake() (map.c). Pushing LOAD2's wake_cycle back therefore
    // delays all of LOAD2's dependents: the ones woken below, and any that
    // reach map later and are woken from add_to_wake_up_lists().
    //
    // LOAD2's word has its own bank read. On a hit, its data is ready when
    // that read finishes. On a miss, it arrives with LOAD1's fill. Without a
    // bank conflict both cases give LOAD1's wake cycle, as before.
    Counter ld2_data_cycle = pair->ld1_wake_cycle;
    if (pair->ld2_word_requested) {
        ld2_data_cycle = pair->ld2_word_hit ?
                             pair->ld2_word_ready_cycle :
                             MAX2(pair->ld1_wake_cycle,
                                  pair->ld2_word_ready_cycle);
    }
    ld2_op->wake_cycle = ld2_data_cycle + IFUSE_LD2_WAKE_DELAY;
    ld2_op->exec_cycle = ld2_data_cycle;

    // LOAD2's fused result is produced into the physical register reserved by
    // LOAD1 and rebound as LOAD2's destination at rename.
    reg_file_produce(ld2_op);

    for (Wake_Up_Entry* wake = ld2_op->wake_up_head; wake; wake = wake->next) {
        Op* dep_op = wake->op;

        if (wake->dep_type != REG_DATA_DEP || !dep_op ||
            dep_op->unique_num != wake->unique_num ||
            !dep_op->op_pool_valid ||
            dep_op->ifuse_recovery_squashed) {
            continue;
        }

        if (op_sources_test_not_rdy(dep_op, wake->rdy_bit)) {
            op_sources_clear_not_rdy(dep_op, wake->rdy_bit);
            wake_action(ld2_op, dep_op, wake->rdy_bit);
            Flag is_load = dep_op->inst_info->table_info.mem_type == MEM_LD;
            STAT_EVENT(ld2_op->proc_id, IFUSE_LD2_CONSUMERS_WAITING);
            if (is_load)
                STAT_EVENT(ld2_op->proc_id, IFUSE_LD2_CONSUMERS_WAITING_LOAD);
            if (op_sources_not_rdy_is_clear(dep_op) &&
                dep_op->rdy_cycle == ld2_op->wake_cycle) {
                STAT_EVENT(ld2_op->proc_id, IFUSE_LD2_CONSUMERS_WAITING_CRITICAL);
                if (is_load)
                    STAT_EVENT(ld2_op->proc_id, IFUSE_LD2_CONSUMERS_WAITING_CRITICAL_LOAD);
            }
        }
    }

    ld2_op->wake_up_signaled[REG_DATA_DEP] = TRUE;
    ld2_op->ifuse_ld2_early_wake_signaled = TRUE;

    // True fusion: LOAD2 never enters the ROB/IQ/LSQ to independently
    // generate its own address, so there is no separate AGU event to wait
    // for. Its "address generation" is free, modeled as completing here.
    ld2_op->dcache_cycle = cycle_count;
    ld2_op->ifuse_ld2_agu_completed = TRUE;

    ifuse_exec_pair_finalize_ld2(ld2_op);
}

/*
 * Signals LOAD2 once its data exists, then drops the pair once LOAD1 is done
 * too. LOAD2's data exists when LOAD1 completed (no separate word read), or
 * when LOAD2's word read hit, or when it missed and LOAD1's fill returned.
 */
static void ifuse_exec_pair_try_finish(
    IFuse_Exec_Pair* pair, void (*wake_action)(Op*, Op*, uns)) {
    if (!pair->ld2_op) {
        return;
    }

    Flag word_done = !pair->ld2_word_requested || pair->ld2_word_done;
    Flag word_hit  = pair->ld2_word_requested && pair->ld2_word_hit;
    if (word_done && (pair->ld1_completed || word_hit)) {
        ifuse_exec_pair_signal_ld2(pair, wake_action);
    }
    if (word_done && pair->ld1_completed) {
        ifuse_exec_pair_remove(pair->ld1_op_num);
    }
}

void ifuse_exec_pair_track_mapped_load(Op* op,
                                       void (*wake_action)(Op*, Op*, uns)) {
    if (!op || op->off_path) {
        return;
    }

    ifuse_exec_pair_init();

    if (op->ifuse_load_role == LOAD1) {
        (void)ifuse_exec_pair_get_or_insert(op->op_num);
        return;
    }

    if (op->ifuse_load_role == PREDICTED_NOT_FUSED) {
        ifuse_exec_pair_remove(op->ifuse_partner_op_num);
        return;
    }

    if (op->ifuse_load_role != LOAD2) {
        return;
    }

    /*
     * LOAD1 is older and creates the execution-side record when it maps. If
     * replay cleanup already removed that record, recreating it here would lose
     * a possibly completed LOAD1 wakeup and leave LOAD2 waiting forever. Keep
     * the pair absent so LOAD2 conservatively uses its ordinary memory path.
     */
    IFuse_Exec_Pair* pair =
        ifuse_exec_pair_find(op->ifuse_partner_op_num);
    if (!pair) {
        return;
    }

    pair->ld2_op = op;

    // LOAD1 may have completed before LOAD2 reached map. Its wake-up links are
    // installed now, so the fused result can be signaled immediately.
    ifuse_exec_pair_try_finish(pair, wake_action);
}

void ifuse_exec_pair_handle_producer_wakeup(
    Op* op, Dep_Type type, void (*wake_action)(Op*, Op*, uns)) {
    if (!op || op->off_path || op->ifuse_load_role != LOAD1 ||
        type != REG_DATA_DEP) {
        return;
    }

    ifuse_exec_pair_init();

    IFuse_Exec_Pair* pair = ifuse_exec_pair_get_or_insert(op->op_num);
    if (!pair) {
        return;
    }

    pair->ld1_completed = TRUE;
    pair->ld1_wake_cycle = op->wake_cycle;

    // LOAD2 may already be waiting in the map-stage-created pair entry.
    ifuse_exec_pair_try_finish(pair, wake_action);
}

Flag ifuse_exec_pair_request_ld2_word(const Op* ld1_op) {
    if (!ifuse_exec_pair_initialized || !ld1_op || ld1_op->off_path ||
        ld1_op->ifuse_load_role != LOAD1 || ld1_op->ifuse_pred_ld2_va == 0) {
        return FALSE;
    }

    IFuse_Exec_Pair* pair = ifuse_exec_pair_find(ld1_op->op_num);
    if (!pair || pair->ld2_word_requested) {
        return FALSE;
    }

    pair->ld2_word_requested = TRUE;
    return TRUE;
}

Flag ifuse_exec_pair_ld2_word_pending(Counter ld1_op_num) {
    if (!ifuse_exec_pair_initialized) {
        return FALSE;
    }

    IFuse_Exec_Pair* pair = ifuse_exec_pair_find(ld1_op_num);
    return pair && pair->ld2_word_requested && !pair->ld2_word_done;
}

void ifuse_exec_pair_ld2_word_read(Counter ld1_op_num, Flag hit,
                                   Counter ready_cycle,
                                   void (*wake_action)(Op*, Op*, uns)) {
    if (!ifuse_exec_pair_initialized) {
        return;
    }

    // LOAD1's prediction may have been dropped while the word waited.
    IFuse_Exec_Pair* pair = ifuse_exec_pair_find(ld1_op_num);
    if (!pair || !pair->ld2_word_requested || pair->ld2_word_done) {
        return;
    }

    pair->ld2_word_done        = TRUE;
    pair->ld2_word_hit         = hit;
    pair->ld2_word_ready_cycle = ready_cycle;
    ifuse_exec_pair_try_finish(pair, wake_action);
}

Flag ifuse_exec_pair_skip_duplicate_wakeup(const Op* op, Dep_Type type) {
    return op && type == REG_DATA_DEP &&
           op->ifuse_load_role == LOAD2 &&
           op->ifuse_ld2_early_wake_signaled;
}

Flag ifuse_exec_pair_bypass_ld2_memory_pipeline(const Op* op) {
    if (!op || op->ifuse_load_role != LOAD2) {
        return FALSE;
    }

    if (op->ifuse_ld2_early_wake_signaled) {
        return TRUE;
    }

    /*
     * Replay can invalidate the live execution-side pair after frontend
     * classification. Without that pair there is no LOAD1 completion event
     * left to supply LOAD2's fused result, so use the ordinary load pipeline.
     */
    if (!ifuse_exec_pair_initialized) {
        return FALSE;
    }

    IFuse_Exec_Pair* pair =
        ifuse_exec_pair_find(op->ifuse_partner_op_num);
    return pair && pair->ld2_op == op;
}

void ifuse_exec_pair_complete_ld2_agu(Op* op) {
    if (!ifuse_exec_pair_bypass_ld2_memory_pipeline(op)) {
        return;
    }

    // Frontend oracle-assisted validation already established that this LOAD2
    // is fused. Account for its AGU use, then suppress its redundant d-cache
    // access and memory request.
    op->dcache_cycle = cycle_count;
    op->ifuse_ld2_agu_completed = TRUE;
    ifuse_exec_pair_finalize_ld2(op);
}

void ifuse_exec_pair_forget_ld1_prediction(Counter ld1_op_num) {
    if (!ifuse_exec_pair_initialized) {
        return;
    }

    ifuse_exec_pair_remove(ld1_op_num);
}

void ifuse_exec_pair_forget_op(const Op* op) {
    if (!ifuse_exec_pair_initialized || !op) {
        return;
    }

    if (op->ifuse_load_role == LOAD1) {
        /*
         * A retired LOAD1 can leave a live APT prediction behind. Preserve its
         * completed wake record until LOAD2 consumes the prediction or APT
         * cleanup explicitly discards it.
         */
        return;
    }

    if (op->ifuse_load_role == LOAD2) {
        ifuse_exec_pair_remove_ld2_if_current(op);
    } else if (op->ifuse_partner_op_num != 0) {
        ifuse_exec_pair_remove(op->ifuse_partner_op_num);
    }
}

void ifuse_exec_pair_observe_live_pairs(uns proc_id) {
    if (!ifuse_exec_pair_initialized) {
        return;
    }

    STAT_EVENT(proc_id, IFUSE_INFLIGHT_FUSED_PAIRS_OBSERVATIONS);
    INC_STAT_EVENT(proc_id, IFUSE_INFLIGHT_FUSED_PAIRS_TOTAL,
                   ifuse_exec_pair_live_count);
}
