// STD headers
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Custom headers
#include "ifuse_aci.h"
#include "ifuse_ideal_alloc.h"
#include "ifuse_ideal_limits.h"
#include "../general.param.h"
#include "../statistics.h"
#include "ifuse.param.h"

/**
 * Ideal Access Check Index (ACI)
 * ==============================
 * The ACI is a live validation table for predicted LD2 cache blocks. LD1
 * inserts the cache block it expects LD2 to access; LD2 checks its actual cache
 * block and consumes the prediction owned by the LD1 returned by APT.
 *
 * This ideal baseline models no sets, ways, LRU, or capacity evictions. The
 * software buckets below are only for simulator lookup speed.
 */

#define ACI_NUM_BUCKETS 4096U

typedef struct ACI_Node {
    uint64_t predicted_ld2_effective_addr;
    uint64_t timestamp;
    uint64_t ld1_micro_op_num;
    uint64_t ld1_load_num;
    struct ACI_Node* next;
} ACI_Node;

// Module state
static ACI_Node* aci_buckets[ACI_NUM_BUCKETS];
static bool      aci_initialized = false;
static uint64_t  aci_next_timestamp = 0;
static IfuseIdealAlloc aci_node_allocator;
static uint64_t        aci_live_prediction_count = 0;
static uint64_t        aci_live_prediction_peak = 0;

static void aci_note_prediction_inserted(void) {
    aci_live_prediction_count++;
    if (aci_live_prediction_count > aci_live_prediction_peak) {
        INC_STAT_EVENT(0, ACI_LIVE_PREDICTION_PEAK,
                       aci_live_prediction_count - aci_live_prediction_peak);
        aci_live_prediction_peak = aci_live_prediction_count;
    }
}

static void aci_note_prediction_removed(void) {
    if (aci_live_prediction_count > 0) {
        aci_live_prediction_count--;
    }
}

// Internal helper methods
static uint64_t aci_get_cacheblock_addr(uint64_t effective_addr);
static unsigned int aci_get_bucket_idx(uint64_t cacheblock_addr);
static void aci_remove_node(unsigned int bucket_idx, ACI_Node* prev,
                            ACI_Node* node);
static ACI_Node* aci_find_prediction(uint64_t cacheblock_addr,
                                     uint64_t ld1_micro_op_num,
                                     ACI_Node** out_prev);
static void aci_invalidate_cacheblock_prediction(uint64_t cacheblock_addr,
                                                 uint64_t ld1_micro_op_num);

/**
 * Initializes the ideal Access Check Index.
 */
void aci_init(void) {
    if (aci_initialized) {
        return;
    }

    memset(aci_buckets, 0, sizeof(aci_buckets));
    aci_next_timestamp = 0;
    aci_live_prediction_count = 0;
    aci_live_prediction_peak = 0;
    if (!ifuse_ideal_alloc_init_fixed(&aci_node_allocator, sizeof(ACI_Node),
                                      IFUSE_IDEAL_ACI_MAX_NODES)) {
        fprintf(stderr, "ACI: fixed pool alloc failed (%u nodes)\n",
                IFUSE_IDEAL_ACI_MAX_NODES);
        exit(1);
    }
    aci_initialized = true;
}

/**
 * Returns the cache-block address containing effective_addr.
 */
static uint64_t aci_get_cacheblock_addr(uint64_t effective_addr) {
    return effective_addr >> ACI_CACHE_LINE_BITS;
}

/**
 * Returns the software bucket for cacheblock_addr.
 */
static unsigned int aci_get_bucket_idx(uint64_t cacheblock_addr) {
    uint64_t h = cacheblock_addr;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (unsigned int)(h & (ACI_NUM_BUCKETS - 1U));
}

/**
 * Removes node from bucket_idx and releases its allocator slot.
 */
static void aci_remove_node(unsigned int bucket_idx, ACI_Node* prev,
                            ACI_Node* node) {
    if (prev) {
        prev->next = node->next;
    } else {
        aci_buckets[bucket_idx] = node->next;
    }
    aci_note_prediction_removed();
    ifuse_ideal_alloc_put(&aci_node_allocator, node);
}

/**
 * Returns the prediction for cacheblock_addr and ld1_micro_op_num.
 *
 * @param out_prev Optional output for the predecessor of the returned node.
 */
static ACI_Node* aci_find_prediction(uint64_t cacheblock_addr,
                                     uint64_t ld1_micro_op_num,
                                     ACI_Node** out_prev) {
    unsigned int bucket_idx = aci_get_bucket_idx(cacheblock_addr);
    ACI_Node*    prev = NULL;

    for (ACI_Node* node = aci_buckets[bucket_idx]; node; node = node->next) {
        if (aci_get_cacheblock_addr(node->predicted_ld2_effective_addr) ==
                cacheblock_addr &&
            node->ld1_micro_op_num == ld1_micro_op_num) {
            if (out_prev) {
                *out_prev = prev;
            }
            return node;
        }
        prev = node;
    }

    if (out_prev) {
        *out_prev = NULL;
    }
    return NULL;
}

/**
 * Inserts the cache block predicted for a live LD1.
 */
void aci_insert_prediction(uint64_t predicted_ld2_effective_addr,
                           uint64_t ld1_micro_op_num,
                           uint64_t ld1_load_num) {
    if (!aci_initialized) {
        aci_init();
    }

    uint64_t predicted_ld2_cacheblock_addr =
        aci_get_cacheblock_addr(predicted_ld2_effective_addr);
    unsigned int bucket_idx = aci_get_bucket_idx(predicted_ld2_cacheblock_addr);
    ACI_Node* replayed_prediction = NULL;
    ACI_Node* oldest_same_block_prediction = NULL;

    for (ACI_Node* node = aci_buckets[bucket_idx]; node; node = node->next) {
        if (aci_get_cacheblock_addr(node->predicted_ld2_effective_addr) !=
            predicted_ld2_cacheblock_addr) {
            continue;
        }
        if (node->ld1_micro_op_num == ld1_micro_op_num) {
            replayed_prediction = node;
        }
        if (!oldest_same_block_prediction ||
            node->timestamp < oldest_same_block_prediction->timestamp) {
            oldest_same_block_prediction = node;
        }
    }

    if (replayed_prediction) {
        replayed_prediction->ld1_load_num = ld1_load_num;
        replayed_prediction->timestamp = ++aci_next_timestamp;
        STAT_EVENT(0, ACI_REPLAYED_INSERTS);
        return;
    }

    ACI_Node* node = (ACI_Node*)ifuse_ideal_alloc_get(&aci_node_allocator);
    if (!node) {
        STAT_EVENT(0, ACI_BACKING_ALLOC_FAILURES);
        return;
    }

    node->predicted_ld2_effective_addr = predicted_ld2_effective_addr;
    node->ld1_micro_op_num             = ld1_micro_op_num;
    node->ld1_load_num                 = ld1_load_num;
    node->timestamp                    = ++aci_next_timestamp;
    node->next                         = aci_buckets[bucket_idx];
    aci_buckets[bucket_idx]            = node;

    STAT_EVENT(0, ACI_PREDICTION_INSERTS);
    aci_note_prediction_inserted();
    if (oldest_same_block_prediction) {
        STAT_EVENT(0, ACI_SAME_CACHEBLOCK_INSERTS);
    }
}

/**
 * Checks actual_ld2_effective_addr and consumes the matching LD1 prediction.
 */
ACI_Result aci_check_and_consume_prediction(
    uint64_t actual_ld2_effective_addr,
    uint64_t predicted_ld2_effective_addr,
    uint64_t ld1_micro_op_num) {
    if (!aci_initialized) {
        aci_init();
    }

    uint64_t actual_ld2_cacheblock_addr =
        aci_get_cacheblock_addr(actual_ld2_effective_addr);
    ACI_Node* prev = NULL;
    ACI_Node* node =
        aci_find_prediction(actual_ld2_cacheblock_addr, ld1_micro_op_num, &prev);
    if (!node) {
        STAT_EVENT(0, ACI_LOOKUP_NO_MATCHES);
        if (predicted_ld2_effective_addr != 0 &&
            actual_ld2_cacheblock_addr !=
            aci_get_cacheblock_addr(predicted_ld2_effective_addr)) {
            return ACI_LOOKUP_WRONG_PREDICTED_BLOCK;
        }
        return ACI_LOOKUP_NO_MATCHING_ENTRY;
    }

    aci_remove_node(aci_get_bucket_idx(actual_ld2_cacheblock_addr), prev, node);
    STAT_EVENT(0, ACI_LOOKUP_MATCHES);
    return ACI_LOOKUP_MATCH;
}

/**
 * Invalidates predictions for cacheblock_addr.
 */
static void aci_invalidate_cacheblock_prediction(uint64_t cacheblock_addr,
                                                 uint64_t ld1_micro_op_num) {
    if (!aci_initialized) {
        return;
    }

    unsigned int bucket_idx = aci_get_bucket_idx(cacheblock_addr);
    ACI_Node* prev = NULL;
    ACI_Node* node = aci_buckets[bucket_idx];

    while (node) {
        ACI_Node* next = node->next;
        bool same_cacheblock =
            aci_get_cacheblock_addr(node->predicted_ld2_effective_addr) ==
            cacheblock_addr;
        bool same_load1 =
            ld1_micro_op_num == 0 ||
            node->ld1_micro_op_num == ld1_micro_op_num;

        if (same_cacheblock && same_load1) {
            aci_remove_node(bucket_idx, prev, node);
            STAT_EVENT(0, ACI_PREDICTION_INVALIDATIONS);
            if (ld1_micro_op_num != 0) {
                return;
            }
        } else {
            prev = node;
        }
        node = next;
    }
}

/**
 * Invalidates the prediction for predicted_ld2_effective_addr.
 */
void aci_invalidate_prediction(uint64_t predicted_ld2_effective_addr,
                               uint64_t ld1_micro_op_num) {
    aci_invalidate_cacheblock_prediction(
        aci_get_cacheblock_addr(predicted_ld2_effective_addr),
        ld1_micro_op_num);
}

/**
 * Removes predictions whose LD2 did not arrive within IFUSE_FUSION_DISTANCE.
 */
void aci_cleanup_stale(uint64_t current_load_num) {
    if (!aci_initialized) {
        return;
    }

    for (unsigned int bucket_idx = 0; bucket_idx < ACI_NUM_BUCKETS; ++bucket_idx) {
        ACI_Node* prev = NULL;
        ACI_Node* node = aci_buckets[bucket_idx];

        while (node) {
            ACI_Node* next = node->next;
            if (current_load_num - node->ld1_load_num >
                IFUSE_FUSION_DISTANCE) {
                aci_remove_node(bucket_idx, prev, node);
                STAT_EVENT(0, ACI_STALE_PREDICTION_REMOVALS);
            } else {
                prev = node;
            }
            node = next;
        }
    }
}
