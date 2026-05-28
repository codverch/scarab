// Custom headers
#include "globals/global_types.h"

#ifndef IFUSE_TRAINING_TABLE_H
#define IFUSE_TRAINING_TABLE_H

/**
 * Initializes the IFuse training table.
 */
void training_table_init(void);

/**
 * Records a fusible retired load pair and promotes frequent pairs into FCT.
 *
 * The caller has already checked the retire-history rule: LD1 and LD2 accessed
 * the same cache block, are within IFUSE_FUSION_DISTANCE, and have no intervening
 * store to that cache block.
 *
 * @param ld1_pc_addr The PC address of LD1.
 * @param ld2_pc_addr The PC address of LD2.
 * @param ld1_effective_addr The effective memory address accessed by LD1.
 * @param ld2_effective_addr The effective memory address accessed by LD2.
 * @param ld1_memory_access_size The memory access size of LD1.
 * @param ld2_memory_access_size The memory access size of LD2.
 * @param ld1_micro_op_num The dynamic micro-op number of LD1.
 * @param ld2_micro_op_num The dynamic micro-op number of LD2.
 * @param proc_id The core id used for statistics.
 */
void training_table_observe_fusible_pair(
    Addr ld1_pc_addr,
    Addr ld2_pc_addr,
    Addr ld1_effective_addr,
    Addr ld2_effective_addr,
    unsigned int ld1_memory_access_size,
    unsigned int ld2_memory_access_size,
    unsigned int ld1_micro_op_num,
    unsigned int ld2_micro_op_num,
    unsigned int proc_id);

/**
 * Retrains the actual fusible pair observed after a failed FCT prediction.
 *
 * Unlike ordinary retire-history training, this path is allowed to retry
 * promotion for the actual observed pair after a misprediction. Ordinary retire
 * observations promote once at threshold and then reinforce only the installed
 * exact FCT pair.
 *
 * @param ld1_pc_addr The PC address of LD1.
 * @param ld2_pc_addr The PC address of LD2.
 * @param ld1_effective_addr The effective memory address accessed by LD1.
 * @param ld2_effective_addr The effective memory address accessed by LD2.
 * @param ld1_memory_access_size The memory access size of LD1.
 * @param ld2_memory_access_size The memory access size of LD2.
 * @param ld1_micro_op_num The dynamic micro-op number of LD1.
 * @param ld2_micro_op_num The dynamic micro-op number of LD2.
 * @param proc_id The core id used for statistics.
 */
void training_table_retrain_fusible_pair(
    Addr ld1_pc_addr,
    Addr ld2_pc_addr,
    Addr ld1_effective_addr,
    Addr ld2_effective_addr,
    unsigned int ld1_memory_access_size,
    unsigned int ld2_memory_access_size,
    unsigned int ld1_micro_op_num,
    unsigned int ld2_micro_op_num,
    unsigned int proc_id);

#endif /* IFUSE_TRAINING_TABLE_H */
