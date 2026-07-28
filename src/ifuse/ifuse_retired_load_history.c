#include "ifuse_retired_load_history.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../general.param.h"
#include "../statistics.h"
#include "ifuse.param.h"

#define HISTORY_LINE_SIZE 64U

/*
 * Hardware-design storage model (distinct from HistoryRow below, whose links
 * and full-width types are simulator bookkeeping): each retired load buffer
 * entry needs a 48-bit PC, 48-bit effective address, 3-bit log2(access
 * size), 10-bit modulo-1024 micro-op timestamp, and 1 valid bit, for 110
 * bits/entry. The timestamp is required because fusion distance is measured
 * in all micro-ops, not merely in loads; a global 10-bit timestamp advances
 * once per micro-op and age is their modulo-1024 difference. Ten timestamp
 * bits distinguish ages 0..511 from expired entries across wraparound; this
 * is sized off IFUSE_FUSION_DISTANCE alone and does not change with capacity.
 *
 * IFUSE_RLB_CAPACITY (default 128) is deliberately smaller than
 * IFUSE_FUSION_DISTANCE (default 512): the buffer only needs to hold the
 * loads live within the distance window, not one slot per micro-op in it.
 * Measured load density is ~20-30% of instructions, so a 512-op window
 * rarely holds more than ~150-160 live loads. Running out of slots early
 * (RLB_CAPACITY_EVICTIONS) just forces the globally oldest live load out
 * ahead of schedule, same effect as if it had aged out on its own -- capacity
 * and distance are independent knobs. At 128 entries the packed RLB costs
 * 128 * 110 = 14,080 bits = 1.71875 KiB, versus 6.875 KiB at 512.
 */
typedef struct HistoryRow {
    RetiredLoadHistoryEntry load;
    Addr block;
    int bucket_prev;
    int bucket_next;
    int age_prev;
    int age_next;
    int free_next;
    bool valid;
} HistoryRow;

static HistoryRow* rows;
static int* bucket_heads;
static unsigned int history_capacity;
static unsigned int history_bucket_mask;
static int oldest_row;
static int newest_row;
static int free_head;
static bool initialized;

static Addr block_addr(Addr addr) {
    return addr & ~(Addr)(HISTORY_LINE_SIZE - 1U);
}

static unsigned int bucket_for(Addr block) {
    uint64_t key = (uint64_t)block;
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return (unsigned int)(key & history_bucket_mask);
}

static unsigned int next_pow2_at_least(unsigned int value) {
    unsigned int p = 1U;
    while (p < value)
        p <<= 1;
    return p;
}

void retired_load_history_init(void) {
    if (!IFUSE_FUSION_DISTANCE) {
        fprintf(stderr,
                "Runtime I-Fuse fusion distance must be >= 1 micro-op\n");
        exit(1);
    }
    if (!IFUSE_RLB_CAPACITY) {
        fprintf(stderr, "Runtime I-Fuse RLB capacity must be >= 1 entry\n");
        exit(1);
    }

    free(rows);
    free(bucket_heads);

    history_capacity = IFUSE_RLB_CAPACITY;
    unsigned int buckets = next_pow2_at_least(2U * history_capacity);
    history_bucket_mask = buckets - 1U;

    rows = (HistoryRow*)calloc(history_capacity, sizeof(*rows));
    bucket_heads = (int*)malloc((size_t)buckets * sizeof(*bucket_heads));
    if (!rows || !bucket_heads) {
        fprintf(stderr,
                "Could not allocate I-Fuse retired load history (%u entries)\n",
                history_capacity);
        exit(1);
    }
    for (unsigned int i = 0; i < buckets; ++i)
        bucket_heads[i] = -1;
    for (unsigned int i = 0; i < history_capacity; ++i)
        rows[i].free_next = i + 1U < history_capacity ? (int)(i + 1U) : -1;
    oldest_row = -1;
    newest_row = -1;
    free_head = 0;
    initialized = true;
}

static void remove_row(int row_idx) {
    HistoryRow* row = &rows[row_idx];
    unsigned int bucket = bucket_for(row->block);

    if (row->bucket_prev >= 0)
        rows[row->bucket_prev].bucket_next = row->bucket_next;
    else
        bucket_heads[bucket] = row->bucket_next;
    if (row->bucket_next >= 0)
        rows[row->bucket_next].bucket_prev = row->bucket_prev;

    if (row->age_prev >= 0)
        rows[row->age_prev].age_next = row->age_next;
    else
        oldest_row = row->age_next;
    if (row->age_next >= 0)
        rows[row->age_next].age_prev = row->age_prev;
    else
        newest_row = row->age_prev;

    memset(row, 0, sizeof(*row));
    row->bucket_prev = row->bucket_next = -1;
    row->age_prev = row->age_next = -1;
    row->free_next = free_head;
    free_head = row_idx;
}

static void prune(Counter current_micro_op_num) {
    while (oldest_row >= 0) {
        Counter older = rows[oldest_row].load.micro_op_num;
        if (current_micro_op_num > older &&
            current_micro_op_num - older >= IFUSE_FUSION_DISTANCE) {
            STAT_EVENT(0, RLB_AGE_EVICTIONS);
            remove_row(oldest_row);
        } else
            break;
    }
}

void retired_load_history_insert(Addr pc, Addr effective_addr, uns mem_size,
                                 Counter micro_op_num) {
    if (!initialized)
        retired_load_history_init();
    prune(micro_op_num);
    if (free_head < 0) {
        STAT_EVENT(0, RLB_CAPACITY_EVICTIONS);
        remove_row(oldest_row);
    }

    int row_idx = free_head;
    HistoryRow* row = &rows[row_idx];
    free_head = row->free_next;

    row->load.pc = pc;
    row->load.effective_addr = effective_addr;
    row->load.mem_size = mem_size;
    row->load.micro_op_num = micro_op_num;
    row->block = block_addr(effective_addr);
    row->valid = true;

    unsigned int bucket = bucket_for(row->block);
    row->bucket_prev = -1;
    row->bucket_next = bucket_heads[bucket];
    if (row->bucket_next >= 0)
        rows[row->bucket_next].bucket_prev = row_idx;
    bucket_heads[bucket] = row_idx;

    row->age_prev = newest_row;
    row->age_next = -1;
    if (newest_row >= 0)
        rows[newest_row].age_next = row_idx;
    else
        oldest_row = row_idx;
    newest_row = row_idx;
}

bool retired_load_history_take_match(Addr effective_addr,
                                     Counter micro_op_num,
                                     RetiredLoadHistoryEntry* match) {
    if (!initialized)
        retired_load_history_init();
    prune(micro_op_num);

    Addr block = block_addr(effective_addr);
    for (int row_idx = bucket_heads[bucket_for(block)]; row_idx >= 0;
         row_idx = rows[row_idx].bucket_next) {
        HistoryRow* row = &rows[row_idx];
        if (row->valid && row->block == block &&
            row->load.micro_op_num < micro_op_num &&
            micro_op_num - row->load.micro_op_num < IFUSE_FUSION_DISTANCE) {
            *match = row->load;
            remove_row(row_idx);
            return true;
        }
    }
    return false;
}

void retired_load_history_invalidate_block(Addr effective_addr) {
    if (!initialized)
        retired_load_history_init();

    Addr block = block_addr(effective_addr);
    int row_idx = bucket_heads[bucket_for(block)];
    while (row_idx >= 0) {
        int next = rows[row_idx].bucket_next;
        if (rows[row_idx].block == block)
            remove_row(row_idx);
        row_idx = next;
    }
}

void retired_load_history_invalidate_range(Addr effective_addr, uns mem_size) {
    if (!mem_size) {
        retired_load_history_invalidate_block(effective_addr);
        return;
    }

    Addr first = block_addr(effective_addr);
    Addr last_addr = effective_addr + (Addr)mem_size - 1U;
    if (last_addr < effective_addr)
        last_addr = ~(Addr)0;
    Addr last = block_addr(last_addr);

    for (Addr block = first;; block += HISTORY_LINE_SIZE) {
        retired_load_history_invalidate_block(block);
        if (block == last || block > ~(Addr)0 - HISTORY_LINE_SIZE)
            break;
    }
}
