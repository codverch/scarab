#include "ifuse_fusion_log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ifuse.param.h"
#include "../statistics.h"

#define IFUSE_FUSION_LOG_CACHE_LINE_SIZE 64U

static const char IFUSE_FUSION_PAIR_CSV_HEADER[] =
    "load1_pc,load1_data_addr,load1_block_offset,load1_mem_size,load1_micro_op_num,"
    "load2_pc,load2_data_addr,load2_block_offset,load2_mem_size,load2_micro_op_num,"
    "micro_op_distance";

static FILE*  fusion_log = NULL;
static Counter logged_fused_pair_count = 0;

static Addr ifuse_fusion_log_cache_block_offset(Addr effective_addr) {
    return effective_addr &
           (Addr)(IFUSE_FUSION_LOG_CACHE_LINE_SIZE - 1U);
}

static void fusion_log_error(const char* action) {
    fprintf(stderr,
            "IFuse: could not %s fusion output file '%s': %s\n",
            action, IFUSE_FUSION_LOG, strerror(errno));
}

static void close_fusion_log(void) {
    if (!fusion_log) {
        return;
    }

    if (fflush(fusion_log) != 0) {
        fusion_log_error("flush");
    }
    if (fclose(fusion_log) != 0) {
        fusion_log_error("close");
    }
    fusion_log = NULL;
}

static void open_fusion_log(void) {
    if (fusion_log) {
        return;
    }

    if (!IFUSE_FUSION_LOG || !IFUSE_FUSION_LOG[0]) {
        fprintf(stderr, "IFuse: fusion output path must not be empty.\n");
        exit(1);
    }

    fusion_log = fopen(IFUSE_FUSION_LOG, "w");
    if (!fusion_log) {
        fusion_log_error("open");
        exit(1);
    }

    if (fprintf(fusion_log, "%s\n", IFUSE_FUSION_PAIR_CSV_HEADER) < 0) {
        fusion_log_error("initialize");
        exit(1);
    }

    if (fflush(fusion_log) != 0) {
        fusion_log_error("initialize");
        exit(1);
    }

    atexit(close_fusion_log);
}

void ifuse_fusion_log_retired_pair(Op* op) {
    if (!IFUSE_FUSION_LOG || !IFUSE_FUSION_LOG[0]) {
        return;
    }

    if (!op || op->off_path || op->ifuse_load_role != LOAD2 ||
        !op->inst_info || op->oracle_info.va == 0 ||
        op->oracle_info.mem_size == 0 || op->ifuse_partner_op_num == 0 ||
        op->ifuse_partner_ld1_va == 0 || op->ifuse_partner_ld1_mem_size == 0) {
        return;
    }

    open_fusion_log();

    Counter micro_op_distance =
        op->op_num - op->ifuse_partner_op_num;

    if (fprintf(fusion_log,
                "0x%llx,0x%llx,%llu,%u,%llu,0x%llx,0x%llx,%llu,%u,%llu,%llu\n",
                (unsigned long long)op->ifuse_partner_ld1_pc,
                (unsigned long long)op->ifuse_partner_ld1_va,
                (unsigned long long)ifuse_fusion_log_cache_block_offset(
                    op->ifuse_partner_ld1_va),
                op->ifuse_partner_ld1_mem_size,
                (unsigned long long)op->ifuse_partner_op_num,
                (unsigned long long)op->inst_info->addr,
                (unsigned long long)op->oracle_info.va,
                (unsigned long long)ifuse_fusion_log_cache_block_offset(
                    op->oracle_info.va),
                op->oracle_info.mem_size,
                (unsigned long long)op->op_num,
                (unsigned long long)micro_op_distance) < 0) {
        fusion_log_error("write to");
        return;
    }

    STAT_EVENT(op->proc_id, IFUSE_FUSED_PAIRS_LOGGED);

    if (++logged_fused_pair_count % 1000 == 0 && fflush(fusion_log) != 0) {
        fusion_log_error("flush");
    }
}
