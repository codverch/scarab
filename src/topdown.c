/*
 * Copyright (c) 2025 University of California, Santa Cruz
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : topdown.c
 * Author       : Yinyuan Zhao, Litz Lab
 * Date         : 05/2025
 *    Implements the Top-Down performance analysis methodology based on:
 *      Yasin, A. "A Top-Down Method for Performance Analysis and Counters Architecture,"
 *      2014 IEEE International Symposium on Performance Analysis of Systems and Software.
 ***************************************************************************************/

#include "topdown.h"

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "dcache_stage.h"
#include "idq_stage.h"
#include "lsq.h"
#include "map_stage.h"
#include "node_stage.h"
#include "op.h"
#include "ifuse/ifuse_exec_pair.h"

const static uns64 TOPDOWN_SCALE_FACTOR = 10000;
const static int TOPDOWN_RECOVERY_DEPTH = 2;

/**************************************************************************************/
/* Events Update */

/* ==================================================================================
 * Event Definitions
 * ----------------------------------------------------------------------------------
 * TotalSlots*            = Total number of issue-pipeline slots.
 * SlotsIssued*           = Utilized issue-pipeline slots to issue operations.
 * SlotsRetired*          = Utilized issue-pipeline slots to retire (complete) operations.
 * FetchBubbles           = Unutilized issue-pipeline slots while there is no backend-stall.
 * RecoveryBubbles        = Unutilized issue-pipeline slots due to recovery from earlier miss-speculation.
 * BrMispredRetired*      = Retired miss-predicted branch instructions.
 * MachineClears*         = Machine clear events (pipeline is flushed).
 * MsSlotsRetired*        = Retired pipeline slots supplied by the micro-sequencer fetch-unit.
 * OpsExecuted*           = Number of operations executed in a cycle.
 * MemStalls.AnyLoad      = Cycles with no uops executed and at least one in-flight load not completed.
 * MemStalls.L1miss       = Cycles with no uops executed and at least one in-flight load that missed the L1-cache.
 * MemStalls.L2miss       = Cycles with no uops executed and at least one in-flight load that missed the L2-cache.
 * MemStalls.L3miss       = Cycles with no uops executed and at least one in-flight load that missed the L3-cache.
 * MemStalls.Stores       = Cycles with few uops executed and no more stores can be issued.
 * ExtMemOutstanding      = Number of outstanding requests to the memory controller every cycle.
 *
 * -----------------------------------------------------------------------------------
 * [Defined in https://github.com/andikleen/pmu-tools/blob/master/skl_client_ratios.py]
 *
 * BackendStalls = CoreStalls + (∑OpsExecuted[= FEW]) + StoreStalls
 * =================================================================================== */

void topdown_bp_recovery(uns proc_id, Op* op) {
  ASSERT(op->proc_id, op->inst_info->table_info.cf_type);

  STAT_EVENT(proc_id, TOPDOWN_MACHINE_CLEAR_CYCLES);
  if (op->bp_pred_info->recover_at_exec) {
    ASSERT(op->proc_id, !op->off_path);
    STAT_EVENT(proc_id, TOPDOWN_BR_RECOVER_AT_EXEC_RETIRED_CYCLES);
  }

  idq_stage_set_recovery_cycle(TOPDOWN_RECOVERY_DEPTH);
}

/*
 * Backend-bound breakdown.
 *
 * Backend Bound is a residual: every slot not used by Retiring, Bad Speculation
 * or Frontend Bound. topdown_idq_update() can compute that residual cycle by
 * cycle, so each backend slot is charged to exactly one structure and one ROB
 * head state, and each breakdown sums to Backend Bound.
 *
 * Structure: what stopped allocation this cycle. Rename stalls on the physical
 * register file; dispatch stops on a full ROB, LQ or SQ. Anything else (a
 * partially issued group with nothing full) is Other.
 *
 * Head state: why the window is not draining, from the oldest op.
 */
static void topdown_backend_attribute(uns proc_id, int backend_slots, Flag stalled_cycle) {
  if (backend_slots <= 0)
    return;

  int full = TOPDOWN_BE_FULL_OTHER_SLOTS;
  if (map->reg_file_stall)
    full = TOPDOWN_BE_FULL_PRF_SLOTS;
  else if (node_dispatch_block == NODE_DISPATCH_BLOCK_ROB)
    full = TOPDOWN_BE_FULL_ROB_SLOTS;
  else if (node_dispatch_block == NODE_DISPATCH_BLOCK_LQ)
    full = TOPDOWN_BE_FULL_LQ_SLOTS;
  else if (node_dispatch_block == NODE_DISPATCH_BLOCK_SQ)
    full = TOPDOWN_BE_FULL_SQ_SLOTS;
  INC_STAT_EVENT(proc_id, full, backend_slots);
  if (stalled_cycle)
    STAT_EVENT(proc_id, TOPDOWN_BE_FULL_PRF_CYCLES + (full - TOPDOWN_BE_FULL_PRF_SLOTS));

  int head_state = TOPDOWN_BE_HEAD_EMPTY_SLOTS;
  Op* head = node->node_head;
  if (head) {
    Mem_Type mem_type = head->inst_info->table_info.mem_type;
    if (head->state == OS_DONE || OP_DONE(head))
      head_state = TOPDOWN_BE_HEAD_DONE_SLOTS;
    else if (head->engine_info.l1_miss)
      head_state = TOPDOWN_BE_HEAD_LLC_MISS_SLOTS;
    else if (head->engine_info.dcmiss)
      head_state = TOPDOWN_BE_HEAD_L1D_MISS_SLOTS;
    else if (ifuse_exec_pair_bypass_ld2_memory_pipeline(head))
      head_state = TOPDOWN_BE_HEAD_FUSED_LD2_SLOTS;  // waiting for LOAD1's data
    else if (mem_type == MEM_ST)
      head_state = TOPDOWN_BE_HEAD_STORE_SLOTS;
    else if (mem_type != NOT_MEM && (head->state == OS_WAIT_DCACHE || head->state == OS_WAIT_MEM))
      head_state = TOPDOWN_BE_HEAD_DCACHE_PORT_SLOTS;  // no L1-D port or MSHR this cycle
    else if (mem_type != NOT_MEM && head->state != OS_IN_ROB && head->state != OS_READY &&
             head->state != OS_LOW_PRIORITY)
      head_state = TOPDOWN_BE_HEAD_L1D_HIT_SLOTS;  // issued, hit, still in flight
    else if (head->state == OS_IN_ROB)
      head_state = TOPDOWN_BE_HEAD_IQ_SLOTS;
    else if (head->state == OS_READY || head->state == OS_LOW_PRIORITY)
      head_state = TOPDOWN_BE_HEAD_PORT_SLOTS;
    else if (head->state == OS_SCHEDULED || head->state == OS_TENTATIVE)
      head_state = TOPDOWN_BE_HEAD_EXEC_SLOTS;
    else
      head_state = TOPDOWN_BE_HEAD_DEP_SLOTS;
  }
  INC_STAT_EVENT(proc_id, head_state, backend_slots);
  if (stalled_cycle)
    STAT_EVENT(proc_id, TOPDOWN_BE_HEAD_LLC_MISS_CYCLES + (head_state - TOPDOWN_BE_HEAD_LLC_MISS_SLOTS));
}

void topdown_idq_update(uns proc_id, int count_available, int count_issued, int count_issued_on_path) {
  INC_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS, ISSUE_WIDTH);
  INC_STAT_EVENT(proc_id, TOPDOWN_ISSUED_SLOTS, count_issued);
  INC_STAT_EVENT(proc_id, TOPDOWN_RETIRED_SLOTS, count_issued_on_path);

  int recovery_cycle = idq_stage_get_recovery_cycle();
  if (recovery_cycle != 0) {
    ASSERT(proc_id, recovery_cycle > 0);
    idq_stage_set_recovery_cycle(recovery_cycle - 1);
    INC_STAT_EVENT(proc_id, TOPDOWN_RECOVERY_BUBBLES_SLOTS, ISSUE_WIDTH - count_available);
    topdown_backend_attribute(proc_id, MIN2(count_available, (int)ISSUE_WIDTH) - count_issued, FALSE);
    return;
  }

  // only increment frontend-stall when there is no backend-stall
  if (count_issued == 0 && idq_stage_get_stage_data()->op_count > 0) {
    STAT_EVENT(proc_id, TOPDOWN_BACKEND_STALLS_CYCLES);
    if (lsq_get_in_flight_load_num() > 0) {
      STAT_EVENT(proc_id, TOPDOWN_MEM_LOAD_STALLS_CYCLES);
    } else if (!lsq_available(MEM_ST)) {
      STAT_EVENT(proc_id, TOPDOWN_MEM_STORE_STALLS_CYCLES);
    }
    topdown_backend_attribute(proc_id, ISSUE_WIDTH - count_issued, TRUE);
    return;
  }

  INC_STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES_SLOTS, ISSUE_WIDTH - count_available);
  topdown_backend_attribute(proc_id, MIN2(count_available, (int)ISSUE_WIDTH) - count_issued, FALSE);
  if (count_available == 0)
    STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES_GT_MIW_CYCLES);
}

void topdown_exec_update(uns proc_id, uns8 fus_busy) {
  if (fus_busy <= TOPDOWN_FU_EXEC_FEW && node->node_count != 0) {
    STAT_EVENT(proc_id, TOPDOWN_EXEC_STALLS_CYCLES);
  }
}

/**************************************************************************************/
/*
 * Metrics Update
 *  At the end of simulation, the events can be directly used to calculate the
 *  metrics using the following formulas.
 */

/*
 * Top-Down Metrics Formulas
 *
 * Frontend Bound       = FetchBubbles / TotalSlots
 * Bad Speculation      = (SlotsIssued - SlotsRetired + RecoveryBubbles) / TotalSlots
 * Retiring             = SlotsRetired / TotalSlots
 * Backend Bound        = 1 - (Frontend Bound + Bad Speculation + Retiring)
 *
 * Fetch Latency Bound  = FetchBubbles[≥ #MIW] / Clocks
 * Fetch Bandwidth Bound = Frontend Bound - Fetch Latency Bound
 *
 * #BrMispredFraction   = BrMispredRetired / (BrMispredRetired + MachineClears)
 * Branch Mispredicts   = #BrMispredFraction * Bad Speculation
 * Machine Clears       = Bad Speculation - Branch Mispredicts
 *
 * MicroSequencer       = MsSlotsRetired / TotalSlots
 * BASE                 = Retiring - MicroSequencer
 *
 * #ExecutionStalls     = (∑OpsExecuted[= FEW]) / Clocks
 *
 * Memory Bound         = (MemStalls.AnyLoad + MemStalls.Stores) / Clocks
 * Core Bound           = #ExecutionStalls - Memory Bound
 *
 * L1 Bound             = (MemStalls.AnyLoad - MemStalls.L1miss) / Clocks
 * L2 Bound             = (MemStalls.L1miss - MemStalls.L2miss) / Clocks
 * L3 Bound             = (MemStalls.L2miss - MemStalls.L3miss) / Clocks
 * Ext. Memory Bound    = MemStalls.L3miss / Clocks
 *
 * MEM Bandwidth        = ExtMemOutstanding[≥ THRESHOLD] / ExtMemOutstanding[≥ 1]
 * MEM Latency          = (ExtMemOutstanding[≥ 1] / Clocks) - MEM Bandwidth
 */

void topdown_done(uns proc_id) {
  /* Top-Level Breakdown */
  uns64 frontend_bound = GET_STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES_SLOTS) * TOPDOWN_SCALE_FACTOR /
                         GET_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS);
  INC_STAT_EVENT(proc_id, TOPDOWN_FRONTEND_BOUND, frontend_bound);

  uns64 bad_spec_slots = GET_STAT_EVENT(proc_id, TOPDOWN_ISSUED_SLOTS) -
                         GET_STAT_EVENT(proc_id, TOPDOWN_RETIRED_SLOTS) +
                         GET_STAT_EVENT(proc_id, TOPDOWN_RECOVERY_BUBBLES_SLOTS);
  uns64 bad_spec_bound = bad_spec_slots * TOPDOWN_SCALE_FACTOR / GET_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS);
  INC_STAT_EVENT(proc_id, TOPDOWN_BAD_SPEC_BOUND, bad_spec_bound);

  uns64 retiring_bound = GET_STAT_EVENT(proc_id, TOPDOWN_RETIRED_SLOTS) * TOPDOWN_SCALE_FACTOR /
                         GET_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS);
  INC_STAT_EVENT(proc_id, TOPDOWN_RETIRING_BOUND, retiring_bound);

  uns64 backend_bound = TOPDOWN_SCALE_FACTOR - frontend_bound - bad_spec_bound - retiring_bound;
  INC_STAT_EVENT(proc_id, TOPDOWN_BACKEND_BOUND, backend_bound);

  /* Backend Breakdown */
  // prevent division by zero
  uns64 backend_stalls_cycles = GET_STAT_EVENT(proc_id, TOPDOWN_BACKEND_STALLS_CYCLES);
  if (backend_stalls_cycles == 0) {
    backend_stalls_cycles++;
  }

  uns64 mem_stalls_cycles = (GET_STAT_EVENT(proc_id, TOPDOWN_MEM_LOAD_STALLS_CYCLES) +
                             GET_STAT_EVENT(proc_id, TOPDOWN_MEM_STORE_STALLS_CYCLES));
  uns64 mem_bound = backend_bound * mem_stalls_cycles / backend_stalls_cycles;
  INC_STAT_EVENT(proc_id, TOPDOWN_MEM_BOUND, mem_bound);
  INC_STAT_EVENT(proc_id, TOPDOWN_CORE_BOUND, backend_bound - mem_bound);

  /* Retiring Breakdown */
  // TODO: need more metadata from the simulation frontend to determine if an operand requires MicroSequencer

  /* Front-End Breakdown */
  ASSERT(proc_id, GET_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS) != 0);
  uns64 latency_bound = GET_STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES_GT_MIW_CYCLES) * TOPDOWN_SCALE_FACTOR /
                        GET_STAT_EVENT(proc_id, NODE_CYCLE);
  INC_STAT_EVENT(proc_id, TOPDOWN_FETCH_LATENCY_BOUND, latency_bound);
  INC_STAT_EVENT(proc_id, TOPDOWN_FETCH_BANDWIDTH_BOUND, frontend_bound - latency_bound);

  /* Bad Spec Breakdown */
  // prevent division by zero
  uns64 bad_spec_cycles = GET_STAT_EVENT(proc_id, TOPDOWN_BR_RECOVER_AT_EXEC_RETIRED_CYCLES) +
                          GET_STAT_EVENT(proc_id, TOPDOWN_MACHINE_CLEAR_CYCLES);
  if (bad_spec_cycles == 0) {
    bad_spec_cycles++;
  }

  INC_STAT_EVENT(proc_id, TOPDOWN_BR_RECOVER_AT_EXEC_BOUND,
                 bad_spec_bound * GET_STAT_EVENT(proc_id, TOPDOWN_BR_RECOVER_AT_EXEC_RETIRED_CYCLES) / bad_spec_cycles);
  INC_STAT_EVENT(proc_id, TOPDOWN_MACHINE_CLEARS_BOUND,
                 bad_spec_bound - GET_STAT_EVENT(proc_id, TOPDOWN_BR_RECOVER_AT_EXEC_BOUND));
}
