// STD headers
#include <stdbool.h>
#include <stdint.h>

// Custom headers
#include "globals/global_types.h"

#ifndef IFUSE_APT_H
#define IFUSE_APT_H

/**
 * Stores one live LD1 prediction waiting for its predicted LD2.
 */
typedef struct APT_Entry {
    // Predicted LD2 identity
    Addr         ld2_pc_addr;

    // Dynamic LD1 context
    Addr         ld1_pc_addr;
    Addr         ld1_effective_addr;
    unsigned int ld1_micro_op_num;

    // Predicted LD2 memory access
    Addr         predicted_ld2_effective_addr;
    unsigned int predicted_ld2_memory_access_size;

    // Bookkeeping
    bool         valid;
    bool         matched;
    uint64_t     timestamp;
} APT_Entry;

/**
 * Initializes the ideal Active Pair Table.
 */
void apt_init(void);

/**
 * Returns and claims the unmatched entry waiting for ld2_pc_addr.
 *
 * When multiple dynamic LD1s wait for the same LD2 PC, IFUSE_APT_MATCH_POLICY
 * chooses whether lookup claims the first inserted or most recently inserted
 * unmatched entry.
 *
 * @param ld2_pc_addr The PC address of the candidate LD2.
 * @return The unmatched APT entry for this LD2 PC, or NULL.
 */
APT_Entry* apt_lookup(Addr ld2_pc_addr);

/**
 * Inserts a live LD1 prediction for a future LD2.
 *
 * Multiple unmatched predictions may exist for the same LD2 PC. This keeps
 * APT capacity ideal while allowing the LD2 match policy to be studied
 * independently from table size.
 *
 * @param ld1_pc_addr The PC address of the predicting LD1.
 * @param ld1_effective_addr The effective memory address accessed by LD1.
 * @param ld1_micro_op_num The dynamic micro-op number of LD1.
 * @param predicted_ld2_memory_access_size The predicted LD2 access size.
 * @param ld2_pc_addr The predicted LD2 PC address.
 * @param predicted_ld2_effective_addr The predicted LD2 effective address.
 * @return The inserted APT entry, or NULL if insertion fails.
 */
APT_Entry* apt_insert_entry(Addr ld1_pc_addr,
                            Addr ld1_effective_addr,
                            unsigned int ld1_micro_op_num,
                            unsigned int predicted_ld2_memory_access_size,
                            Addr ld2_pc_addr,
                            Addr predicted_ld2_effective_addr);

/**
 * Removes the entry for a dynamic LD1 after rename-stage bookkeeping completes.
 *
 * @param ld2_pc_addr The predicted LD2 PC address used to find the APT bucket.
 * @param ld1_micro_op_num The dynamic micro-op number of LD1.
 */
void apt_remove_entry_by_ld1_micro_op(Addr ld2_pc_addr,
                                      unsigned int ld1_micro_op_num);

/**
 * Observes the current number of live APT LD2 predictions.
 *
 * The average number of live APT LD2 predictions is:
 *   APT_LIVE_LD2_PREDICTION_TOTAL / APT_LIVE_LD2_PREDICTION_OBSERVATIONS
 */
void apt_observe_live_ld2_predictions(void);

/**
 * Removes entries whose LD2 did not arrive within IFUSE_FUSION_DISTANCE.
 *
 * @param current_op_num The current dynamic micro-op number.
 */
void apt_cleanup_stale(unsigned int current_op_num);

#endif /* IFUSE_APT_H */
