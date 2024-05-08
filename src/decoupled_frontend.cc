#include "decoupled_frontend.h"
#include "frontend/frontend_intf.h"
#include "op.h"
#include "op_pool.h"
#include "isa/isa_macros.h"
#include "prefetcher/pref.param.h"
#include "prefetcher/fdip_new.h"
#include "memory/memory.param.h"

#include <deque>
#include <vector>
#include <iostream>
#include <tuple>
#include <cmath>

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECOUPLED_FE, ##args)

// a threshold of utility ratio
#define UTILITY_RATIO_THRESHOLD 0.70
// a threshold of timeliness ratio
#define TIMELINESS_RATIO_THRESHOLD 0.77

typedef enum FT_Ended_By_enum {
  FT_ENDED_BY_INIT,
  FT_ICACHE_LINE_BOUNDARY,
  FT_TAKEN_BRANCH,
  FT_BAR_FETCH,
  FT_APP_EXIT
} FT_Ended_By;

typedef struct FT_struct {
  std::vector<Op*> ops;
  // indicate the next op index to read by the consumer (icache or uop)
  uint64_t op_pos;
  // the PC of the first inst in this FT
  Addr start;
  // the FT size in bytes, counting from the first byte of the first inst to the last byte of the last inst
  Addr length;

  FT_Ended_By ended_by;

  // FT API
  void ft_add_op(Op *op, FT_Ended_By ft_ended_by);
  void ft_free_ops_and_clear();
  bool ft_can_fetch_op();
  bool ft_fetch_op(Op** op, Flag* end_of_ft);
  void ft_return_op(Op *op);
} FT;

// Per core fetch target queue:
// Each core has a queue of FTs,
// where each FT contains a queue of micro instructions.
std::vector<std::deque<FT>> per_core_ftq;
// keep track of the current FT to be pushed next
std::vector<FT> per_core_current_ft_to_push;
// keep track of the current FT being used by the icache / uop cache
std::vector<FT> per_core_current_ft_in_use;

std::vector<int> per_core_off_path;
std::vector<int> per_core_sched_off_path;
std::vector<uint64_t> per_core_op_count;
std::vector<std::vector<decoupled_fe_iter>> per_core_ftq_iterators;
std::vector<uint64_t> per_core_recovery_addr;
std::vector<uint64_t> per_core_redirect_cycle;
std::vector<bool> per_core_stalled;
std::vector<uint64_t> per_core_ftq_ft_num;
extern std::vector<Utility_Timeliness_Info> per_core_utility_timeliness_info;

//per_core pointers
std::deque<FT> *df_ftq;
int *off_path;
int *sched_off_path;
int set_proc_id;
std::vector<decoupled_fe_iter> *ftq_iterator;
//need to overwrite op->op_num with decoupeld fe

bool trace_mode;

void alloc_mem_decoupled_fe(uns numCores) {
  per_core_ftq.resize(numCores);
  per_core_current_ft_to_push.resize(numCores);
  per_core_current_ft_in_use.resize(numCores);
  per_core_off_path.resize(numCores);
  per_core_sched_off_path.resize(numCores);
  per_core_op_count.resize(numCores);
  per_core_ftq_iterators.resize(numCores);
  per_core_recovery_addr.resize(numCores);
  per_core_redirect_cycle.resize(numCores);
  per_core_stalled.resize(numCores);
  per_core_ftq_ft_num.resize(numCores);
}

void init_decoupled_fe(uns proc_id, const char*) {
  trace_mode = false;

#ifdef ENABLE_PT_MEMTRACE
  trace_mode |= (FRONTEND == FE_PT || FRONTEND == FE_MEMTRACE);
#endif

  per_core_off_path[proc_id] = false;
  per_core_sched_off_path[proc_id] = false;
  per_core_op_count[proc_id] = 1;
  per_core_recovery_addr[proc_id] = 0;
  per_core_redirect_cycle[proc_id] = 0;
  per_core_ftq_ft_num[proc_id] = FE_FTQ_BLOCK_NUM;

}

void set_decoupled_fe(int proc_id) {
  df_ftq = &(per_core_ftq.data()[proc_id]);
  off_path = &(per_core_off_path.data()[proc_id]);
  sched_off_path = &(per_core_sched_off_path.data()[proc_id]);
  ftq_iterator = &(per_core_ftq_iterators.data()[proc_id]);
  set_proc_id = proc_id;
}


void reset_decoupled_fe() {}

void recover_decoupled_fe(int proc_id) {
  per_core_off_path[proc_id] = false;
  per_core_sched_off_path[proc_id] = false;
  per_core_recovery_addr[proc_id] = bp_recovery_info->recovery_fetch_addr;

  for (auto it = per_core_ftq[proc_id].begin(); it != per_core_ftq[proc_id].end(); it++) {
    it->ft_free_ops_and_clear();
  }
  per_core_ftq[proc_id].clear();

  per_core_current_ft_to_push[proc_id].ft_free_ops_and_clear();
  per_core_current_ft_in_use[proc_id].ft_free_ops_and_clear();

  per_core_op_count[proc_id] = bp_recovery_info->recovery_op_num + 1;
  DEBUG(set_proc_id,
        "Recovery signalled fetch_addr0x:%llx\n", bp_recovery_info->recovery_fetch_addr);

  for (auto it = per_core_ftq_iterators[proc_id].begin(); it != per_core_ftq_iterators[proc_id].end(); it++) {
    // When the FTQ flushes, reset all iterators
    it->ft_pos = 0;
    it->op_pos = 0;
    it->flattened_op_pos = 0;
  }
  if (FDIP_ADJUSTABLE_FTQ && per_core_utility_timeliness_info[proc_id].adjust) {
    double cur_utility_ratio = per_core_utility_timeliness_info[proc_id].utility_ratio;
    double cur_timeliness_ratio = per_core_utility_timeliness_info[proc_id].timeliness_ratio;
    if (FDIP_ADJUSTABLE_FTQ == 1) { // utility-based adjustment UFTQ-AUR
      DEBUG(set_proc_id, "Current utility ratio : %lf, current FTQ ft num : %lu\n", cur_utility_ratio, per_core_ftq_ft_num[proc_id]);
      if (cur_utility_ratio < UTILITY_RATIO_THRESHOLD) {
        per_core_ftq_ft_num[proc_id] -= std::round(per_core_ftq_ft_num[proc_id] * (UTILITY_RATIO_THRESHOLD - cur_utility_ratio));
        if (per_core_ftq_ft_num[proc_id] < UFTQ_MIN_FTQ_BLOCK_NUM)
          per_core_ftq_ft_num[proc_id] = UFTQ_MIN_FTQ_BLOCK_NUM;
      } else if (cur_utility_ratio > UTILITY_RATIO_THRESHOLD) {
        per_core_ftq_ft_num[proc_id] += std::round(per_core_ftq_ft_num[proc_id] * (cur_utility_ratio - UTILITY_RATIO_THRESHOLD));
        if (per_core_ftq_ft_num[proc_id] > UFTQ_MAX_FTQ_BLOCK_NUM)
          per_core_ftq_ft_num[proc_id] = UFTQ_MAX_FTQ_BLOCK_NUM;
      }
      DEBUG(set_proc_id, "New FTQ ft num : %lu\n", per_core_ftq_ft_num[proc_id]);
      per_core_utility_timeliness_info[proc_id].adjust = FALSE;
    } else if (FDIP_ADJUSTABLE_FTQ == 2) { // timeliness-based adjustment UFTQ-ATR
      DEBUG(set_proc_id, "Current timeliness ratio : %lf, current FTQ ft num : %lu\n", cur_timeliness_ratio, per_core_ftq_ft_num[proc_id]);
      if (cur_timeliness_ratio < TIMELINESS_RATIO_THRESHOLD) {
        per_core_ftq_ft_num[proc_id] -= std::round(per_core_ftq_ft_num[proc_id] * (TIMELINESS_RATIO_THRESHOLD - cur_timeliness_ratio));
        if (per_core_ftq_ft_num[proc_id] < UFTQ_MIN_FTQ_BLOCK_NUM)
          per_core_ftq_ft_num[proc_id] = UFTQ_MIN_FTQ_BLOCK_NUM;
      } else if (cur_timeliness_ratio > TIMELINESS_RATIO_THRESHOLD) {
        per_core_ftq_ft_num[proc_id] += std::round(per_core_ftq_ft_num[proc_id] * (cur_timeliness_ratio - TIMELINESS_RATIO_THRESHOLD));
        if (per_core_ftq_ft_num[proc_id] > UFTQ_MAX_FTQ_BLOCK_NUM)
          per_core_ftq_ft_num[proc_id] = UFTQ_MAX_FTQ_BLOCK_NUM;
      }
      DEBUG(set_proc_id, "New FTQ ft num : %lu\n", per_core_ftq_ft_num[proc_id]);
      per_core_utility_timeliness_info[proc_id].adjust = FALSE;
    } else if (FDIP_ADJUSTABLE_FTQ == 3) { // combined method UFTQ-ATR-AUR
      uint64_t qdaur = per_core_ftq_ft_num[proc_id];
      uint64_t qdatr = per_core_ftq_ft_num[proc_id];
      if (cur_utility_ratio < UTILITY_RATIO_THRESHOLD)
        qdaur -= std::round(per_core_ftq_ft_num[proc_id] * (UTILITY_RATIO_THRESHOLD - cur_utility_ratio));
      else if (cur_utility_ratio > UTILITY_RATIO_THRESHOLD)
        qdaur += std::round(per_core_ftq_ft_num[proc_id] * (cur_utility_ratio - UTILITY_RATIO_THRESHOLD));
      if (cur_timeliness_ratio < TIMELINESS_RATIO_THRESHOLD)
        qdatr -= std::round(per_core_ftq_ft_num[proc_id] * (TIMELINESS_RATIO_THRESHOLD - cur_timeliness_ratio));
      else if (cur_timeliness_ratio > TIMELINESS_RATIO_THRESHOLD)
        qdatr += std::round(per_core_ftq_ft_num[proc_id] * (cur_timeliness_ratio - TIMELINESS_RATIO_THRESHOLD));
      per_core_ftq_ft_num[proc_id] = std::round(-2.3*qdaur - 31.2*qdatr + 0.007*qdaur*qdaur + 0.1*qdatr*qdatr + 0.3*qdaur*qdatr);
      if (per_core_ftq_ft_num[proc_id] < UFTQ_MIN_FTQ_BLOCK_NUM)
        per_core_ftq_ft_num[proc_id] = UFTQ_MIN_FTQ_BLOCK_NUM;
      else if (per_core_ftq_ft_num[proc_id] > UFTQ_MAX_FTQ_BLOCK_NUM)
        per_core_ftq_ft_num[proc_id] = UFTQ_MAX_FTQ_BLOCK_NUM;
    }
  }

  auto op = bp_recovery_info->recovery_op;

  if(per_core_stalled[set_proc_id]) {
    DEBUG(set_proc_id,
          "Unstalled off-path fetch barrier due to recovery fetch_addr0x:%llx off_path:%i op_num:%llu\n",
          op->inst_info->addr, op->off_path, op->op_num);
    per_core_stalled[set_proc_id] = false;
  }

  if (op->oracle_info.recover_at_decode)
    STAT_EVENT(proc_id, FTQ_RECOVER_DECODE);
  else if (op->oracle_info.recover_at_exec)
    STAT_EVENT(proc_id, FTQ_RECOVER_EXEC);

  uint64_t offpath_cycles = cycle_count - per_core_redirect_cycle[proc_id];
  ASSERT(proc_id, cycle_count > per_core_redirect_cycle[proc_id]);
  INC_STAT_EVENT(proc_id, FTQ_OFFPATH_CYCLES, offpath_cycles);
  per_core_redirect_cycle[proc_id] = 0;

  //FIXME always fetch off path ops? should we get rid of this parameter?
  frontend_recover(proc_id, bp_recovery_info->recovery_inst_uid);
  ASSERTM(proc_id, bp_recovery_info->recovery_fetch_addr == frontend_next_fetch_addr(proc_id),
          "Scarab's recovery addr 0x%llx does not match frontend's recovery "
          "addr 0x%llx\n",
          bp_recovery_info->recovery_fetch_addr, frontend_next_fetch_addr(proc_id));
}

void debug_decoupled_fe() {

}

void update_decoupled_fe() {
  uns cf_num = 0;
  uint64_t bytes_this_cycle = 0;
  uint64_t cfs_taken_this_cycle = 0;
  static int fwd_progress = 0;
  fwd_progress++;
  if (fwd_progress >= 100000) {
    std::cout << "No forward progress for 1000000 cycles" << std::endl;
    ASSERT(0,0);
  }
  if (*off_path)
    STAT_EVENT(set_proc_id, FTQ_CYCLES_OFFPATH);
  else
    STAT_EVENT(set_proc_id, FTQ_CYCLES_ONPATH);

  while(1) {
    ASSERT(set_proc_id, decoupled_fe_ftq_num_fts() <= per_core_ftq_ft_num[set_proc_id]);
    ASSERT(set_proc_id, cfs_taken_this_cycle <= FE_FTQ_TAKEN_CFS_PER_CYCLE);

    if (decoupled_fe_ftq_num_fts() == per_core_ftq_ft_num[set_proc_id]) {
      DEBUG(set_proc_id, "Break due to full FTQ\n");
      if (*off_path)
        STAT_EVENT(set_proc_id, FTQ_BREAK_FULL_FT_OFFPATH);
      else
        STAT_EVENT(set_proc_id, FTQ_BREAK_FULL_FT_ONPATH);
      break;
    }
    if (cfs_taken_this_cycle == FE_FTQ_TAKEN_CFS_PER_CYCLE) {
      DEBUG(set_proc_id, "Break due to max cfs taken per cycle\n");
      if (*off_path)
        STAT_EVENT(set_proc_id, FTQ_BREAK_MAX_CFS_TAKEN_OFFPATH);
      else
        STAT_EVENT(set_proc_id, FTQ_BREAK_MAX_CFS_TAKEN_ONPATH);
      break;
    }
    // use `>=` because inst size does not necessarily align with FE_FTQ_BYTES_PER_CYCLE
    if (bytes_this_cycle >= FE_FTQ_BYTES_PER_CYCLE) {
      DEBUG(set_proc_id, "Break due to max bytes per cycle\n");
      if (*off_path)
        STAT_EVENT(set_proc_id, FTQ_BREAK_MAX_BYTES_OFFPATH);
      else
        STAT_EVENT(set_proc_id, FTQ_BREAK_MAX_BYTES_ONPATH);
      break;
    }
    if (BP_MECH != MTAGE_BP && !bp_is_predictable(g_bp_data, set_proc_id)) {
      DEBUG(set_proc_id, "Break due to limited branch predictor\n");
      if (*off_path)
        STAT_EVENT(set_proc_id, FTQ_BREAK_PRED_BR_OFFPATH);
      else
        STAT_EVENT(set_proc_id, FTQ_BREAK_PRED_BR_ONPATH);
      break;
    }
    if (per_core_stalled[set_proc_id]) {
      DEBUG(set_proc_id, "Break due to wait for fetch barrier resolved\n");
      if (*off_path)
        STAT_EVENT(set_proc_id, FTQ_BREAK_BAR_FETCH_OFFPATH);
      else
        STAT_EVENT(set_proc_id, FTQ_BREAK_BAR_FETCH_ONPATH);
      break;
    }
    if (!frontend_can_fetch_op(set_proc_id)) {
      std::cout << "Warning could not fetch inst from frontend" << std::endl;
      break;
    }

    fwd_progress = 0;
    uint64_t pred_addr = 3;
    Op* op = alloc_op(set_proc_id);
    frontend_fetch_op(set_proc_id, op);
    op->op_num = per_core_op_count[set_proc_id]++;
    op->off_path = *off_path;

    if(op->table_info->cf_type) {
      ASSERT(set_proc_id, op->eom);
      pred_addr = bp_predict_op(g_bp_data, op, cf_num++, op->inst_info->addr);
      DEBUG(set_proc_id,
            "Predict CF fetch_addr:%llx true_npc:%llx pred_npc:%lx mispred:%i misfetch:%i btb miss:%i taken:%i recover_at_decode:%i recover_at_exec:%i off_path:%i bar_fetch:%i\n",
            op->inst_info->addr, op->oracle_info.npc, pred_addr,
            op->oracle_info.mispred, op->oracle_info.misfetch,
            op->oracle_info.btb_miss, op->oracle_info.pred == TAKEN,
            op->oracle_info.recover_at_decode, op->oracle_info.recover_at_exec,
            *off_path, op->table_info->bar_type & BAR_FETCH);

      /* On fetch barrier stall the frontend. Ignore BTB misses here as the exec frontend cannot
         handle recovery/execution until syscalls retire. This is ok as stalling causes the same
         cycle penalty than recovering from BTB miss. */
      if ((op->table_info->bar_type & BAR_FETCH) || IS_CALLSYS(op->table_info)) {
        op->oracle_info.recover_at_decode = FALSE;
        op->oracle_info.recover_at_exec = FALSE;
        decoupled_fe_stall(op);
      }

      if(op->oracle_info.recover_at_decode || op->oracle_info.recover_at_exec) {
        ASSERT(0, (int)op->oracle_info.recover_at_decode + (int)op->oracle_info.recover_at_exec < 2);
        /* If already on the off-path do not schedule recovery as scarab cannot recover OOO
           (An older op may recover at exec and a younger op may recover at decode)
           This is not accurate but it should not affect the time spend on the off-path */
        if (*off_path) {
          op->oracle_info.recover_at_decode = FALSE;
          op->oracle_info.recover_at_exec = FALSE;
        }
        *off_path = true;
        frontend_redirect(set_proc_id, op->inst_uid, pred_addr);
        per_core_redirect_cycle[set_proc_id] = cycle_count;
      }
      // If we are already on the off-path redirect on all taken branches in TRACE-MODE
      else if (trace_mode && *off_path && op->oracle_info.pred == TAKEN) {
        frontend_redirect(set_proc_id, op->inst_uid, pred_addr);
      }
    }
    else {
      ASSERT(0,!(op->oracle_info.recover_at_decode | op->oracle_info.recover_at_exec));
      /* On fetch barrier stall the frontend. */
      if (op->table_info->bar_type & BAR_FETCH) {
        decoupled_fe_stall(op);
      }
    }
    // We start a new fetch target if:
    // 1. crossing a icache line
    // 2. taking a control flow op
    // 3. seeing a sysop or serializing (fence) instruction
    // 4. reaching the application exit
    FT_Ended_By ft_ended_by = FT_ENDED_BY_INIT;
    if (op->eom) {
      uns offset = ADDR_PLUS_OFFSET(op->inst_info->addr, op->inst_info->trace_info.inst_size) -
                    ROUND_DOWN(op->inst_info->addr, ICACHE_LINE_SIZE);
      bool end_of_icache_line = offset >= ICACHE_LINE_SIZE;
      bool cf_taken = op->table_info->cf_type && op->oracle_info.pred == TAKEN;
      bool bar_fetch = IS_CALLSYS(op->table_info) || op->table_info->bar_type & BAR_FETCH;

      if (op->exit) {
        ft_ended_by = FT_APP_EXIT;
      } else if (bar_fetch) {
        ft_ended_by = FT_BAR_FETCH;
      } else if (cf_taken) {
        ft_ended_by = FT_TAKEN_BRANCH;
      } else if (end_of_icache_line) {
        ft_ended_by = FT_ICACHE_LINE_BOUNDARY;
      }

      bytes_this_cycle += op->inst_info->trace_info.inst_size;
      cfs_taken_this_cycle += cf_taken || bar_fetch;
    }

    per_core_current_ft_to_push[set_proc_id].ft_add_op(op, ft_ended_by);
    // ft_ended_by != FT_ENDED_BY_INIT indicates the end of the current fetch target
    // it is now ready to be pushed to the queue
    if (ft_ended_by != FT_ENDED_BY_INIT) {
      ASSERT(set_proc_id, per_core_current_ft_to_push[set_proc_id].start && per_core_current_ft_to_push[set_proc_id].length && per_core_current_ft_to_push[set_proc_id].ops.size());
      df_ftq->emplace_back(per_core_current_ft_to_push[set_proc_id]);
      per_core_current_ft_to_push[set_proc_id] = FT();
    }

    if (*off_path) {
      STAT_EVENT(set_proc_id, FTQ_FETCHED_INS_OFFPATH);
    }
    else {
      STAT_EVENT(set_proc_id, FTQ_FETCHED_INS_ONPATH);
    }
      
    DEBUG(set_proc_id,
          "Push new op to FTQ fetch_addr0x:%llx off_path:%i op_num:%llu dis:%s recovery_addr:%lx fetch_bar:%i\n",
          op->inst_info->addr, op->off_path, op->op_num, disasm_op(op, TRUE), per_core_recovery_addr[set_proc_id], op->table_info->bar_type & BAR_FETCH);
    // Recovery sanity check
    if (per_core_recovery_addr[set_proc_id]) {
      ASSERT(set_proc_id, per_core_recovery_addr[set_proc_id] == op->inst_info->addr);
      per_core_recovery_addr[set_proc_id] = 0;
    }
  }
}

bool decoupled_fe_can_fetch_op(int proc_id) {
  return per_core_current_ft_in_use[proc_id].ft_can_fetch_op() || decoupled_fe_can_fetch_ft(proc_id);
}

// A temporary interface implementing the old API `decoupled_fe_fetch_op`
// using the new APIs, so there are minimal changes to the icache stage for now.
// Later the icache stage will be restrctured and this will be removed.
bool decoupled_fe_fetch_op(Op** op, int proc_id) {
  // if first time getting ft, fitst ft after flush, or ft used up
  // need to fetch a new ft
  if (!per_core_current_ft_in_use[set_proc_id].ft_can_fetch_op()) {
    if (decoupled_fe_can_fetch_ft(proc_id)) {
      Addr start, length;
      decoupled_fe_fetch_ft(&start, &length, proc_id);
    } else {
      return false;
    }
  }

  ASSERT(proc_id, per_core_current_ft_in_use[set_proc_id].ft_can_fetch_op());

  Flag end_of_ft;
  ASSERT(proc_id, per_core_current_ft_in_use[set_proc_id].ft_fetch_op(op, &end_of_ft));
  return true;
}

// A temporary interface implementing the old API `decoupled_fe_return_op`
// using the new APIs, so there are minimal changes to the icache stage for now.
// Later the icache stage will be restrctured and this will be removed.
void decoupled_fe_return_op(Op *op) {
  per_core_current_ft_in_use[set_proc_id].ft_return_op(op);

  DEBUG(set_proc_id,
        "Return fetched op backt to FTQ fetch_addr0x:%llx off_path:%i\n",
        op->inst_info->addr, *off_path);
}

bool decoupled_fe_can_fetch_ft(int proc_id) {
  return per_core_ftq[proc_id].size() > 0;
}

bool decoupled_fe_fetch_ft(Addr* ft_start, Addr* ft_length, int proc_id) {
  if (per_core_ftq[proc_id].size()) {
    per_core_current_ft_in_use[proc_id] = per_core_ftq[proc_id].front();
    FT* ft = &per_core_current_ft_in_use[proc_id];
    *ft_start = ft->start;
    *ft_length = ft->length;
    per_core_ftq[proc_id].pop_front();

    for (auto it = per_core_ftq_iterators[proc_id].begin(); it != per_core_ftq_iterators[proc_id].end(); it++) {
      if (FDIP_BP_CONFIDENCE && it->ft_pos == 0 && it->op_pos == 0) {
        ASSERT(proc_id, it->flattened_op_pos == 0);
        fdip_set_cur_op(proc_id, ft->ops.front());
      }

      // When the icache consumes an FT decrement the iter's offset so it points to the same entry as before
      if (it->ft_pos > 0) {
        ASSERT(proc_id, it->flattened_op_pos >= ft->ops.size());
        it->flattened_op_pos -= ft->ops.size();
        it->ft_pos--;
      } else {
        ASSERT(proc_id, it->flattened_op_pos < ft->ops.size());
        it->flattened_op_pos = 0;
        it->op_pos = 0;
      }
    }

    return true;
  }
  return false;
}

uint64_t decoupled_fe_next_fetch_addr(int proc_id) {
  //ASSERT(proc_id, per_core_ftq[proc_id].size() > 0);
  if(!per_core_ftq[proc_id].size())
    return frontend_next_fetch_addr(proc_id);

  if(!per_core_current_ft_in_use[proc_id].ops.empty())
    return per_core_current_ft_in_use[proc_id].ops.front()->inst_info->addr;

  if(!per_core_ftq[proc_id].front().ops.empty())
    return per_core_ftq[proc_id].front().ops.front()->inst_info->addr;

  ASSERT(proc_id, 0);
  return 0;
}
  
decoupled_fe_iter* decoupled_fe_new_ftq_iter() {
  per_core_ftq_iterators[set_proc_id].push_back(decoupled_fe_iter());
  return &per_core_ftq_iterators[set_proc_id].back();
}

/* Returns the Op at current FTQ iterator position. Returns NULL if the FTQ is empty */ 
Op* decoupled_fe_ftq_iter_get(decoupled_fe_iter* iter, bool *end_of_ft) {
  // if FTQ is empty or if iter has seen all FTs
  if (df_ftq->empty() || iter->ft_pos == df_ftq->size()) {
    if (df_ftq->empty())
      ASSERT(set_proc_id, iter->ft_pos == 0 && iter->op_pos == 0 && iter->flattened_op_pos == 0);
    return NULL;
  }

  ASSERT(set_proc_id, iter->ft_pos >= 0);
  ASSERT(set_proc_id, iter->ft_pos < df_ftq->size());
  ASSERT(set_proc_id, iter->op_pos >= 0);
  ASSERT(set_proc_id, iter->op_pos < df_ftq->at(iter->ft_pos).ops.size());
  *end_of_ft = iter->op_pos == df_ftq->at(iter->ft_pos).ops.size() - 1;
  return df_ftq->at(iter->ft_pos).ops[iter->op_pos];
}

/* Increments the iterator and returns the Op at FTQ iterator position. Returns NULL if the FTQ is empty */
Op* decoupled_fe_ftq_iter_get_next(decoupled_fe_iter* iter, bool *end_of_ft) {
  if (iter->ft_pos + 1 == df_ftq->size() && iter->op_pos + 1 == df_ftq->at(iter->ft_pos).ops.size()) {
    // if iter is at the last op and the last FT
    iter->ft_pos += 1;
    // at this moment iter is at the last FT
    // but later FTQ will receive new FT
    // so we prepare for that case by setting op_pos to zero
    iter->op_pos = 0;
    iter->flattened_op_pos++;
    return NULL;
  } else if (iter->ft_pos == df_ftq->size()) {
    // if iter has seen all FTs
    ASSERT(set_proc_id, iter->op_pos == 0);
    return NULL;
  } else if (iter->op_pos + 1 == df_ftq->at(iter->ft_pos).ops.size()) {
    // if iter is at the last op, but not the last FT
    iter->ft_pos += 1;
    iter->op_pos = 0;
    iter->flattened_op_pos++;
  } else {
    // if iter is not at the last op, nor the last FT
    iter->op_pos++;
    iter->flattened_op_pos++;
  }
  return decoupled_fe_ftq_iter_get(iter, end_of_ft);
}

/* Returns iter flattened offset from the start of the FTQ, this offset gets incremented
   by advancing the iter and decremented by the icache consuming FTQ entries,
   and reset by flushes */
uint64_t decoupled_fe_ftq_iter_offset(decoupled_fe_iter* iter) {
  return iter->flattened_op_pos;
}

/* Returns iter ft offset from the start of the FTQ, this offset gets incremented
   by advancing the iter and decremented by the icache consuming FTQ entries,
   and reset by flushes */
uint64_t decoupled_fe_ftq_iter_ft_offset(decoupled_fe_iter* iter) {
  return iter->ft_pos;
}

uint64_t decoupled_fe_ftq_num_ops() {
  uint64_t num_ops = 0;
  for (auto ft = df_ftq->begin(); ft != df_ftq->end(); ft++) {
    num_ops += ft->ops.size();
  }
  return num_ops;
}

uint64_t decoupled_fe_ftq_num_fts() {
  return  per_core_ftq[set_proc_id].size();
}

void decoupled_fe_stall(Op *op) {
  per_core_stalled[set_proc_id] = true;
  DEBUG(set_proc_id,
        "Decoupled fetch stalled due to barrier fetch_addr0x:%llx off_path:%i op_num:%llu\n",
        op->inst_info->addr, op->off_path, op->op_num);
}

void decoupled_fe_retire(Op *op, int proc_id, uns64 inst_uid) {
  if((op->table_info->bar_type & BAR_FETCH) || IS_CALLSYS(op->table_info)) {
    per_core_stalled[set_proc_id] = false;
    DEBUG(set_proc_id,
          "Decoupled fetch unstalled due to retired barrier fetch_addr0x:%llx off_path:%i op_num:%llu list_count:%i\n",
          op->inst_info->addr, op->off_path, op->op_num, td->seq_op_list.count);
    ASSERT(set_proc_id, td->seq_op_list.count == 1);
  }

  //unblock pin exec driven, trace frontends do not need to block/unblock
  frontend_retire(proc_id, inst_uid);
}

void FT::ft_add_op(Op *op, FT_Ended_By ft_ended_by) {
  if (ops.empty()) {
    ASSERT(set_proc_id, op->bom && !start);
    start = op->inst_info->addr;
  } else {
    if (op->bom) {
      // assert consecutivity
      ASSERT(set_proc_id, ops.back()->inst_info->addr + ops.back()->inst_info->trace_info.inst_size
                      == op->inst_info->addr);
    } else {
      // assert all uops of the same inst share the same addr
      ASSERT(set_proc_id, ops.back()->inst_info->addr == op->inst_info->addr);
    }
  }
  ops.emplace_back(op);
  if (ft_ended_by != FT_ENDED_BY_INIT) {
    ASSERT(set_proc_id, op->eom && !length);
    ASSERT(set_proc_id, start);
    length = op->inst_info->addr + op->inst_info->trace_info.inst_size - start;
    ASSERT(set_proc_id, ended_by == FT_ENDED_BY_INIT);
    ended_by = ft_ended_by;
  }
}

void FT::ft_free_ops_and_clear() {
  while (op_pos < ops.size()) {
    free_op(ops[op_pos]);
    op_pos++;
  }

  ops.clear();
  op_pos = 0;
  start = 0;
  length = 0;
  ended_by = FT_ENDED_BY_INIT;
}

bool FT::ft_can_fetch_op() {
  return op_pos < ops.size();
}

bool FT::ft_fetch_op(Op** op, Flag* end_of_ft) {
  if (op_pos < ops.size()) {
    *op = ops[op_pos];

    if (op_pos + 1 == ops.size()) {
      *end_of_ft = TRUE;
    } else {
      *end_of_ft = FALSE;
    }

    op_pos++;
    DEBUG(set_proc_id,
          "Fetch op from FT fetch_addr0x:%llx off_path:%i op_num:%llu\n",
          (*op)->inst_info->addr, (*op)->off_path, (*op)->op_num);

    return true;
  }
  return false;
}

void FT::ft_return_op(Op *op) {
  // return op to fetch target
  ASSERT(set_proc_id, op_pos);
  ASSERT(set_proc_id, op == ops[op_pos-1]);
  op_pos--;

  // note: based on the reason why the op is ever returned,
  // if the returned op if the first one in the ft,
  // might need to think about if need to return the ft to ftq as well.
  // in that case, need to modify per core fdip iter:
  // ================================================
  // for (auto it = per_core_ftq_iterators[set_proc_id].begin(); it != per_core_ftq_iterators[set_proc_id].end(); it++) {
  //   // When the icache returns an op increment the iter's offset so it points to the same entry as before
  //   if (it->op_pos) {
  //     it->op_pos++;
  //   }
  //   if (it->flattened_op_pos) {
  //     it->flattened_op_pos++;
  //   }
  // }
  // ================================================

  // for now, do not return ft anyhow.
  DEBUG(set_proc_id,
        "Return fetched op backt to FT fetch_addr0x:%llx off_path:%i\n",
        op->inst_info->addr, *off_path);
}