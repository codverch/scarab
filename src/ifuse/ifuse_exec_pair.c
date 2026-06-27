#include "ifuse_exec_pair.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ifuse.param.h"
#include "ifuse_ideal_alloc.h"
#include "ifuse_ideal_limits.h"
#include "../map.h"
#include "../map_rename.h"
#include "../memory/memory.param.h"
#include "../op_info.h"

#define IFUSE_EXEC_PAIR_NUM_BUCKETS 4096U

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
    Flag    ld1_slow_memory;
    struct IFuse_Exec_Pair* next;
} IFuse_Exec_Pair;

static IFuse_Exec_Pair* ifuse_exec_pair_table[IFUSE_EXEC_PAIR_NUM_BUCKETS];
static IfuseIdealAlloc  ifuse_exec_pair_alloc;
static bool             ifuse_exec_pair_initialized = false;

static unsigned int ifuse_exec_pair_bucket(Counter ld1_op_num) {
    uint64_t h = (uint64_t)ld1_op_num;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (unsigned int)(h & (IFUSE_EXEC_PAIR_NUM_BUCKETS - 1U));
}

static void ifuse_exec_pair_init(void) {
    if (ifuse_exec_pair_initialized) {
        return;
    }

    memset(ifuse_exec_pair_table, 0, sizeof(ifuse_exec_pair_table));
    if (!ifuse_ideal_alloc_init_fixed(&ifuse_exec_pair_alloc,
                                      sizeof(IFuse_Exec_Pair),
                                      IFUSE_IDEAL_EXEC_PAIR_MAX_NODES)) {
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

/*
 * LOAD1 paid real memory latency when it missed or its access took longer than
 * a simple L1 hit. Fast-hit LOAD1s defer LOAD2 dependent wake to LOAD2's AGU.
 */
static Flag ifuse_load1_had_slow_memory(const Op* load1) {
    if (load1->oracle_info.dcmiss) {
        return TRUE;
    }

    Counter mem_latency = load1->wake_cycle - load1->issue_cycle;
    return mem_latency >
           (Counter)(DCACHE_CYCLES + load1->inst_info->extra_ld_latency);
}

static Flag ifuse_should_early_wake_ld2(const IFuse_Exec_Pair* pair) {
    if (!IFUSE_LD2_EARLY_WAKEUP_ON_MISS_ONLY) {
        return TRUE;
    }

    return pair->ld1_slow_memory;
}

static void ifuse_exec_pair_signal_ld2(
    IFuse_Exec_Pair* pair, void (*wake_action)(Op*, Op*, uns));

static void ifuse_exec_pair_maybe_signal_ld2(
    IFuse_Exec_Pair* pair, void (*wake_action)(Op*, Op*, uns)) {
    if (!pair->ld2_op || !ifuse_should_early_wake_ld2(pair)) {
        return;
    }

    ifuse_exec_pair_signal_ld2(pair, wake_action);
    ifuse_exec_pair_remove(pair->ld1_op_num);
}

static void ifuse_exec_pair_finalize_ld2(Op* ld2_op) {
    if (!ld2_op->ifuse_ld2_agu_completed) {
        return;
    }

    if (ld2_op->ifuse_ld2_early_wake_signaled) {
        ld2_op->done_cycle =
            MAX2(ld2_op->wake_cycle, ld2_op->dcache_cycle);
    } else {
        ld2_op->done_cycle = ld2_op->wake_cycle;
    }
    ld2_op->state = OS_DONE;
}

/**
 * Models the aggressive fused datapath: LOAD2's dependents observe their
 * producer as ready at LOAD1's wake cycle, without waiting for LOAD2's normal
 * cache access to complete.
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

    ld2_op->wake_cycle = pair->ld1_wake_cycle;
    ld2_op->exec_cycle = pair->ld1_wake_cycle;

    // LOAD2's fused result is now available in the physical register allocated
    // by LOAD1. Update Scarab's register metadata at the same modeled event.
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
        }
    }

    ld2_op->wake_up_signaled[REG_DATA_DEP] = TRUE;
    ld2_op->ifuse_ld2_early_wake_signaled = TRUE;
    ifuse_exec_pair_finalize_ld2(ld2_op);
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

    // LOAD1 may have completed before LOAD2 reached map.
    if (pair->ld1_completed) {
        ifuse_exec_pair_maybe_signal_ld2(pair, wake_action);
    }
}

void ifuse_exec_pair_handle_producer_wakeup(
    Op* op, Dep_Type type, void (*wake_action)(Op*, Op*, uns)) {
    IFuse_Exec_Pair* pair;

    if (!op || op->off_path || op->ifuse_load_role != LOAD1 ||
        type != REG_DATA_DEP) {
        return;
    }

    ifuse_exec_pair_init();

    pair = ifuse_exec_pair_get_or_insert(op->op_num);
    if (!pair) {
        return;
    }

    pair->ld1_completed = TRUE;
    pair->ld1_wake_cycle = op->wake_cycle;
    pair->ld1_slow_memory = ifuse_load1_had_slow_memory(op);

    ifuse_exec_pair_maybe_signal_ld2(pair, wake_action);
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

    if (op->ifuse_ld2_agu_completed) {
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

void ifuse_exec_pair_complete_ld2_agu(Op* op,
                                      void (*wake_action)(Op*, Op*, uns)) {
    if (!ifuse_exec_pair_bypass_ld2_memory_pipeline(op)) {
        return;
    }

    // Frontend oracle-assisted validation already established that this LOAD2
    // is fused. Account for its AGU use, then suppress its redundant d-cache
    // access and memory request.
    op->dcache_cycle = cycle_count;
    op->ifuse_ld2_agu_completed = TRUE;

    if (!op->ifuse_ld2_early_wake_signaled) {
        op->done_cycle =
            cycle_count + DCACHE_CYCLES + op->inst_info->extra_ld_latency;
        op->wake_cycle = op->done_cycle;
        wake_up_ops(op, REG_DATA_DEP, wake_action);
        ifuse_exec_pair_finalize_ld2(op);
        ifuse_exec_pair_remove(op->ifuse_partner_op_num);
        return;
    }

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
