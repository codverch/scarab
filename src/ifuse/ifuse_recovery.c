#include "ifuse_recovery.h"

#include "../bp/bp.h"
#include "../globals/assert.h"
#include "../globals/utils.h"
#include "ifuse_apt.h"

static Flag    ifuse_recovery_flushing = FALSE;
static Counter ifuse_recovery_op_num = 0;

void ifuse_sched_recovery(Op* ld2_op, Counter detection_cycle) {
    ASSERT(0, ld2_op);
    ASSERT(ld2_op->proc_id, !ld2_op->off_path);
    ASSERT(ld2_op->proc_id, ld2_op->ifuse_ld2_prediction_failed);
    ASSERT(ld2_op->proc_id, ld2_op->oracle_info.npc);

    /*
     * Scarab recovery boundaries must preserve whole instructions. LOAD2 can
     * be one uop inside a multi-uop instruction, so retain through that
     * instruction's final uop and flush only younger instructions.
     */
    Op* recovery_op = ld2_op;
    while (!recovery_op->eom) {
        recovery_op = recovery_op->next_node;
        ASSERT(ld2_op->proc_id, recovery_op);
        ASSERT(ld2_op->proc_id,
               recovery_op->inst_uid == ld2_op->inst_uid);
    }

    /*
     * Keep an older pending recovery if one already exists. Otherwise schedule
     * a one-cycle-later replay rooted at LOAD2. This deliberately does not set
     * branch-predictor recovery flags: LOAD2 is a data-speculation failure, not
     * a control-flow misprediction.
    */
    if (bp_recovery_info->recovery_cycle != MAX_CTR &&
        bp_recovery_info->recovery_op_num < recovery_op->op_num) {
        return;
    }

    bp_recovery_info->recovery_cycle = detection_cycle + 1;
    bp_recovery_info->recovery_fetch_addr =
        ADDR_PLUS_OFFSET(ld2_op->inst_info->addr, ld2_op->inst_info->trace_info.inst_size);
    bp_recovery_info->recovery_op_num = recovery_op->op_num;
    bp_recovery_info->recovery_cf_type =
        recovery_op->inst_info->table_info.cf_type;
    bp_recovery_info->recovery_info = recovery_op->recovery_info;
    bp_recovery_info->recovery_info.op_num = recovery_op->op_num;
    bp_recovery_info->recovery_inst_info = recovery_op->inst_info;
    bp_recovery_info->recovery_force_offpath = FALSE;
    bp_recovery_info->recovery_op = recovery_op;
    bp_recovery_info->recovery_unique_num = recovery_op->unique_num;
    bp_recovery_info->recovery_inst_uid = recovery_op->inst_uid;
    bp_recovery_info->wpe_flag = FALSE;
    bp_recovery_info->ifuse_recovery = TRUE;

    /*
     * Retirement must wait until recovery fires. LOAD2 survives recovery and
     * later retires normally after its real memory request completes.
     */
    recovery_op->recovery_scheduled = TRUE;
}

void ifuse_recovery_begin_flush(Counter recovery_op_num) {
    ifuse_recovery_flushing = TRUE;
    ifuse_recovery_op_num = recovery_op_num;
}

void ifuse_recovery_end_flush(void) {
    ifuse_recovery_flushing = FALSE;
    ifuse_recovery_op_num = 0;
}

void ifuse_recover_flushed_op(const Op* op) {
    if (!ifuse_recovery_flushing || !op ||
        op->op_num <= ifuse_recovery_op_num) {
        return;
    }

    if (op->ifuse_load_role == LOAD1) {
        /*
         * LOAD1 is leaving the machine, so its prediction can no longer be
         * fulfilled. APT removal also invalidates ACI and frees any extra
         * physical register that LOAD1 allocated before being squashed.
         */
        apt_remove_entry_by_ld1_micro_op_any_pc((unsigned int)op->op_num);
        return;
    }

    if (op->ifuse_load_role == LOAD2 ||
        op->ifuse_load_role == PREDICTED_NOT_FUSED) {
        /*
         * If LOAD2 had not reached rename, APT still owns a matched entry.
         * Reopen that claim for the surviving LOAD1 so re-fetched LOAD2 can
         * match it. If rename already removed the entry, this is a no-op.
         */
        apt_reopen_matched_entry(op->inst_info->addr,
                                 (unsigned int)op->ifuse_partner_op_num);
    }
}
