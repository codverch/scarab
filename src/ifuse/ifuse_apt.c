// STD headers
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Custom headers
#include "ifuse_apt.h"
#include "ifuse_aci.h"
#include "ifuse_exec_pair.h"
#include "ifuse_ideal_alloc.h"
#include "ifuse_ideal_limits.h"
#include "ifuse_rename.h"
#include "../general.param.h"
#include "../statistics.h"
#include "ifuse.param.h"

/**
 * Ideal Active Pair Table (APT)
 * =============================
 * The APT is a live prediction queue. LD1 inserts an entry when it predicts a
 * future LD2; LD2 probes by PC and claims one unmatched prediction for that
 * LD2 PC.
 *
 * Multiple dynamic LD1s may wait for the same LD2 PC. The match policy is a
 * logical experiment knob, not a capacity model:
 *
 *   IFUSE_APT_MATCH_POLICY = 0: claim the first inserted unmatched LD1.
 *   IFUSE_APT_MATCH_POLICY = 1: claim the most recently inserted unmatched LD1.
 *
 * This ideal baseline has no hardware capacity model. It stores entries in
 * hash buckets for software speed, but the buckets do not imply set conflicts
 * or replacement. The ACI entry created with each APT entry is invalidated
 * when its owner leaves the APT.
 */

#define APT_NUM_BUCKETS 4096U

// Internal table node
typedef struct APT_Node {
    APT_Entry      entry;
    struct APT_Node* next;
} APT_Node;

// Module state
static APT_Node* apt_table[APT_NUM_BUCKETS];
static bool      apt_initialized = false;
static uint64_t  apt_next_timestamp = 0;
static uint64_t  apt_live_ld2_prediction_count = 0;
static uint64_t  apt_live_ld2_prediction_peak = 0;
static IfuseIdealAlloc apt_node_alloc;

// Internal helper methods
static unsigned int apt_bucket(Addr ld2_pc_addr);
static APT_Node* apt_find_matching_node(Addr ld2_pc_addr);
static void apt_note_prediction_inserted(void);
static void apt_note_prediction_removed(void);
static void apt_invalidate_pending_aci_prediction(const APT_Entry* entry);
static void apt_remove_node(unsigned int bucket, APT_Node* prev,
                            APT_Node* node, bool preserve_exec_pair);

/**
 * Initializes the ideal Active Pair Table.
 */
void apt_init(void) {
    if (apt_initialized) {
        return;
    }

    memset(apt_table, 0, sizeof(apt_table));
    apt_next_timestamp = 0;
    apt_live_ld2_prediction_count = 0;
    apt_live_ld2_prediction_peak = 0;
    if (!ifuse_ideal_alloc_init_fixed(&apt_node_alloc, sizeof(APT_Node),
                                      IFUSE_IDEAL_APT_MAX_NODES)) {
        fprintf(stderr, "APT: fixed pool alloc failed (%u nodes)\n",
                IFUSE_IDEAL_APT_MAX_NODES);
        exit(1);
    }
    apt_initialized = true;
}

/**
 * Returns the software bucket for ld2_pc_addr.
 *
 * The bucket exists only to make simulator lookup efficient. It is not a
 * modeled hardware set index.
 */
static unsigned int apt_bucket(Addr ld2_pc_addr) {
    uint64_t h = (uint64_t)ld2_pc_addr;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (unsigned int)(h & (APT_NUM_BUCKETS - 1U));
}

/**
 * Returns the unmatched node selected by the active APT match policy.
 */
static APT_Node* apt_find_matching_node(Addr ld2_pc_addr) {
    unsigned int bucket = apt_bucket(ld2_pc_addr);
    APT_Node* best_node = NULL;

    for (APT_Node* node = apt_table[bucket]; node; node = node->next) {
        if (node->entry.valid &&
            !node->entry.matched &&
            node->entry.ld2_pc_addr == ld2_pc_addr) {
            if (!best_node) {
                best_node = node;
                continue;
            }

            if (IFUSE_APT_MATCH_POLICY == 1) {
                if (node->entry.timestamp > best_node->entry.timestamp) {
                    best_node = node;
                }
            } else {
                if (node->entry.timestamp < best_node->entry.timestamp) {
                    best_node = node;
                }
            }
        }
    }

    return best_node;
}

/**
 * Records that one live APT LD2 prediction was inserted.
 */
static void apt_note_prediction_inserted(void) {
    apt_live_ld2_prediction_count++;
    if (apt_live_ld2_prediction_count > apt_live_ld2_prediction_peak) {
        INC_STAT_EVENT(0, APT_LIVE_LD2_PREDICTION_PEAK,
                       apt_live_ld2_prediction_count -
                       apt_live_ld2_prediction_peak);
        apt_live_ld2_prediction_peak = apt_live_ld2_prediction_count;
    }
}

/**
 * Records that one live APT LD2 prediction was removed.
 */
static void apt_note_prediction_removed(void) {
    if (apt_live_ld2_prediction_count > 0) {
        apt_live_ld2_prediction_count--;
    }
}

/**
 * Observes the current number of live APT LD2 predictions.
 */
void apt_observe_live_ld2_predictions(void) {
    if (!apt_initialized) {
        return;
    }

    STAT_EVENT(0, APT_LIVE_LD2_PREDICTION_OBSERVATIONS);
    INC_STAT_EVENT(0, APT_LIVE_LD2_PREDICTION_TOTAL,
                   apt_live_ld2_prediction_count);
}

/**
 * Invalidates the ACI prediction owned by this APT entry.
 *
 * LD1 creates both an APT entry and an ACI entry. Once LD2 claims the APT entry,
 * ACI validation owns the ACI entry: a correct prediction consumes it, and an
 * ACI failure explicitly invalidates it. APT cleanup only invalidates ACI for
 * predictions that were never claimed by LD2.
 */
static void apt_invalidate_pending_aci_prediction(const APT_Entry* entry) {
    if (!entry || !entry->valid || entry->matched) {
        return;
    }

    aci_invalidate_prediction(entry->predicted_ld2_effective_addr,
                              entry->ld1_micro_op_num);
}

/**
 * Removes node from bucket and releases its allocator slot.
 */
static void apt_remove_node(unsigned int bucket, APT_Node* prev,
                            APT_Node* node, bool preserve_exec_pair) {
    if (!node) {
        return;
    }

    apt_invalidate_pending_aci_prediction(&node->entry);
    if (prev) {
        prev->next = node->next;
    } else {
        apt_table[bucket] = node->next;
    }
    apt_note_prediction_removed();

    // An unmatched LOAD1 owns its extra physical register through this APT
    // entry. A matching LOAD2 must take ownership before removing the entry.
    // Stale cleanup reaches this path when the predicted LOAD2 never arrives.
    ifuse_free_ld2_physical_reg(node->entry.ld2_physical_reg_id);
    if (!preserve_exec_pair) {
        ifuse_exec_pair_forget_ld1_prediction(
            node->entry.ld1_micro_op_num);
    }

    ifuse_ideal_alloc_put(&apt_node_alloc, node);
}

/**
 * Returns and claims the unmatched entry waiting for ld2_pc_addr.
 */
APT_Entry* apt_lookup(Addr ld2_pc_addr) {
    if (!apt_initialized) {
        apt_init();
    }
    if (ld2_pc_addr == 0) {
        return NULL;
    }

    APT_Node* node = apt_find_matching_node(ld2_pc_addr);
    if (!node) {
        return NULL;
    }

    node->entry.matched = true;
    return &node->entry;
}

/**
 * Inserts a live LD1 prediction for a future LD2.
 */
APT_Entry* apt_insert_entry(Addr ld1_pc_addr,
                            Addr ld1_effective_addr,
                            unsigned int ld1_micro_op_num,
                            unsigned int ld1_memory_access_size,
                            unsigned int predicted_ld2_memory_access_size,
                            Addr ld2_pc_addr,
                            Addr predicted_ld2_effective_addr,
                            unsigned int fct_delta_slot_idx) {
    if (!apt_initialized) {
        apt_init();
    }
    if (ld2_pc_addr == 0) {
        return NULL;
    }

    APT_Node* node = (APT_Node*)ifuse_ideal_alloc_get(&apt_node_alloc);
    if (!node) {
        fprintf(stderr, "APT: alloc failed for ld2_pc=0x%llx\n",
                (unsigned long long)ld2_pc_addr);
        return NULL;
    }

    node->entry.ld2_pc_addr                      = ld2_pc_addr;
    node->entry.ld1_pc_addr                      = ld1_pc_addr;
    node->entry.ld1_effective_addr               = ld1_effective_addr;
    node->entry.ld1_micro_op_num                 = ld1_micro_op_num;
    node->entry.ld1_memory_access_size           = ld1_memory_access_size;
    node->entry.ld2_physical_reg_id              = 0xFFFF;
    node->entry.predicted_ld2_effective_addr     = predicted_ld2_effective_addr;
    node->entry.predicted_ld2_memory_access_size = predicted_ld2_memory_access_size;
    node->entry.fct_delta_slot_idx               = fct_delta_slot_idx;
    node->entry.valid                            = true;
    node->entry.matched                          = false;
    node->entry.timestamp                        = ++apt_next_timestamp;

    unsigned int bucket = apt_bucket(ld2_pc_addr);
    node->next = apt_table[bucket];
    apt_table[bucket] = node;
    apt_note_prediction_inserted();
    return &node->entry;
}

/**
 * Removes the entry for a dynamic LD1 after rename-stage bookkeeping completes.
 */
void apt_remove_entry_by_ld1_micro_op(Addr ld2_pc_addr,
                                      unsigned int ld1_micro_op_num) {
    if (!apt_initialized || ld2_pc_addr == 0) {
        return;
    }

    unsigned int bucket = apt_bucket(ld2_pc_addr);
    APT_Node* prev = NULL;
    for (APT_Node* node = apt_table[bucket]; node; node = node->next) {
        if (node->entry.valid &&
            node->entry.ld2_pc_addr == ld2_pc_addr &&
            node->entry.ld1_micro_op_num == ld1_micro_op_num) {
            apt_remove_node(bucket, prev, node, false);
            return;
        }
        prev = node;
    }
}

void apt_remove_entry_by_ld1_micro_op_any_pc(unsigned int ld1_micro_op_num) {
    if (!apt_initialized) {
        return;
    }

    for (unsigned int bucket = 0; bucket < APT_NUM_BUCKETS; bucket++) {
        APT_Node* prev = NULL;
        for (APT_Node* node = apt_table[bucket]; node; node = node->next) {
            if (node->entry.valid &&
                node->entry.ld1_micro_op_num == ld1_micro_op_num) {
                apt_remove_node(bucket, prev, node, false);
                return;
            }
            prev = node;
        }
    }
}

bool apt_reopen_matched_entry(Addr ld2_pc_addr,
                              unsigned int ld1_micro_op_num) {
    if (!apt_initialized || ld2_pc_addr == 0) {
        return false;
    }

    unsigned int bucket = apt_bucket(ld2_pc_addr);
    for (APT_Node* node = apt_table[bucket]; node; node = node->next) {
        if (node->entry.valid &&
            node->entry.matched &&
            node->entry.ld2_pc_addr == ld2_pc_addr &&
            node->entry.ld1_micro_op_num == ld1_micro_op_num) {
            /*
             * Fetch-time validation may have consumed or invalidated ACI.
             * Recreate the prediction so the re-fetched LOAD2 performs the
             * same validation before it reaches rename.
             */
            node->entry.matched = false;
            aci_insert_prediction(node->entry.predicted_ld2_effective_addr,
                                  node->entry.ld1_micro_op_num);
            return true;
        }
    }

    return false;
}

bool apt_set_ld2_physical_reg_id(unsigned int ld1_micro_op_num,
                                 unsigned int ld2_physical_reg_id) {
    if (!apt_initialized || ld2_physical_reg_id == 0xFFFF) {
        return false;
    }

    for (unsigned int bucket = 0; bucket < APT_NUM_BUCKETS; bucket++) {
        for (APT_Node* node = apt_table[bucket]; node; node = node->next) {
            if (node->entry.valid &&
                node->entry.ld1_micro_op_num == ld1_micro_op_num) {
                node->entry.ld2_physical_reg_id = ld2_physical_reg_id;
                return true;
            }
        }
    }

    return false;
}

bool apt_take_ld2_physical_reg_id(Addr ld2_pc_addr,
                                  unsigned int ld1_micro_op_num,
                                  unsigned int* ld2_physical_reg_id) {
    if (!apt_initialized || ld2_pc_addr == 0 || !ld2_physical_reg_id) {
        return false;
    }

    unsigned int bucket = apt_bucket(ld2_pc_addr);
    APT_Node* prev = NULL;
    for (APT_Node* node = apt_table[bucket]; node; node = node->next) {
        if (node->entry.valid &&
            node->entry.ld2_pc_addr == ld2_pc_addr &&
            node->entry.ld1_micro_op_num == ld1_micro_op_num &&
            node->entry.ld2_physical_reg_id != 0xFFFF) {
            // LOAD2 now owns the register. Clear APT ownership before removing
            // the entry so apt_remove_node() does not return it to the free list.
            *ld2_physical_reg_id = node->entry.ld2_physical_reg_id;
            node->entry.ld2_physical_reg_id = 0xFFFF;
            apt_remove_node(bucket, prev, node, true);
            return true;
        }
        prev = node;
    }

    return false;
}

/**
 * Removes entries whose LD2 did not arrive within IFUSE_FUSION_DISTANCE.
 */
void apt_cleanup_stale(uint64_t current_micro_op_num) {
    if (!apt_initialized) {
        return;
    }

    for (unsigned int bucket = 0; bucket < APT_NUM_BUCKETS; ++bucket) {
        APT_Node* prev = NULL;
        APT_Node* node = apt_table[bucket];

        while (node) {
            APT_Node* next = node->next;
            // Only unmatched predictions expire here. A matched LOAD2 has
            // already arrived at fetch; its extra register stays live until
            // LOAD2 renames or the prediction is explicitly removed.
            if (node->entry.valid && !node->entry.matched &&
                current_micro_op_num - node->entry.ld1_micro_op_num >
                    IFUSE_FUSION_DISTANCE) {
                apt_remove_node(bucket, prev, node, false);
            } else {
                prev = node;
            }
            node = next;
        }
    }
}
