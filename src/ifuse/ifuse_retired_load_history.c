// STD headers
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Custom headers
#include "ifuse_retired_load_history.h"
#include "../general.param.h"
#include "ifuse.param.h"

/**
 * Retired Load History
 * ====================
 * The retired-load history is the runtime trainer's short-term memory. It keeps
 * recently retired loads so a retiring LD2 can find an older LD1 in the same
 * cache block and within IFUSE_FUSION_DISTANCE.
 *
 * This table intentionally models a simple 512-entry circular history, not a
 * realistic hardware structure. The side hash index exists only to avoid
 * scanning unrelated retired loads in the retire hot path.
 */

#define RETIRED_LOAD_HISTORY_CAPACITY    512U
#define RETIRED_LOAD_HISTORY_NUM_BUCKETS 1024U
#define RETIRED_LOAD_HISTORY_LINE_SIZE   64U

typedef struct RetiredLoadHistoryRow {
    Addr         load_pc_addr;
    Addr         load_effective_addr;
    Addr         load_cacheblock_addr;
    unsigned int load_memory_access_size;
    unsigned int load_micro_op_num;
    int          next_bucket_row;
    bool         valid;
} RetiredLoadHistoryRow;

// Module state
static RetiredLoadHistoryRow
    retired_load_history_rows[RETIRED_LOAD_HISTORY_CAPACITY];
static int          retired_load_history_bucket_heads[RETIRED_LOAD_HISTORY_NUM_BUCKETS];
static unsigned int retired_load_history_first_row = 0;
static unsigned int retired_load_history_num_rows  = 0;

// Address helpers

/**
 * Returns the cache-block address containing a load effective address.
 *
 * @param load_effective_addr The effective memory address accessed by a load.
 * @return The cache-block address.
 */
static Addr retired_load_history_cacheblock_addr(Addr load_effective_addr) {
    return load_effective_addr & ~(Addr)(RETIRED_LOAD_HISTORY_LINE_SIZE - 1U);
}

/**
 * Returns the history side-index bucket for a cache-block address.
 *
 * @param load_cacheblock_addr The cache-block address.
 * @return The bucket number.
 */
static unsigned int retired_load_history_bucket(Addr load_cacheblock_addr) {
    uint64_t hash_value = (uint64_t)load_cacheblock_addr;
    hash_value ^= hash_value >> 33;
    hash_value *= 0xff51afd7ed558ccdULL;
    hash_value ^= hash_value >> 33;
    return (unsigned int)(hash_value & (RETIRED_LOAD_HISTORY_NUM_BUCKETS - 1U));
}

// Row management

/**
 * Removes a row from its cache-block bucket chain.
 *
 * @param row_num The history row to remove.
 */
static void retired_load_history_unlink_row(unsigned int row_num) {
    RetiredLoadHistoryRow* row = &retired_load_history_rows[row_num];
    unsigned int bucket = retired_load_history_bucket(row->load_cacheblock_addr);
    int* bucket_link = &retired_load_history_bucket_heads[bucket];

    while (*bucket_link >= 0) {
        unsigned int current_row_num = (unsigned int)(*bucket_link);
        if (current_row_num == row_num) {
            *bucket_link = retired_load_history_rows[current_row_num].next_bucket_row;
            row->next_bucket_row = -1;
            return;
        }
        bucket_link = &retired_load_history_rows[current_row_num].next_bucket_row;
    }
}

/**
 * Removes the oldest history row.
 */
static void retired_load_history_discard_oldest_row(void) {
    if (retired_load_history_num_rows == 0) {
        return;
    }

    retired_load_history_unlink_row(retired_load_history_first_row);
    retired_load_history_rows[retired_load_history_first_row].valid = false;
    retired_load_history_first_row =
        (retired_load_history_first_row + 1U) % RETIRED_LOAD_HISTORY_CAPACITY;
    retired_load_history_num_rows--;
}

/**
 * Removes rows whose dynamic distance is beyond IFUSE_FUSION_DISTANCE.
 *
 * @param current_micro_op_num The newest retiring load's micro-op number.
 */
static void retired_load_history_prune(unsigned int current_micro_op_num) {
    unsigned int min_micro_op_num =
        (current_micro_op_num > IFUSE_FUSION_DISTANCE) ?
            (current_micro_op_num - IFUSE_FUSION_DISTANCE) : 1U;

    while (retired_load_history_num_rows > 0 &&
           retired_load_history_rows[retired_load_history_first_row]
                   .load_micro_op_num < min_micro_op_num) {
        retired_load_history_discard_oldest_row();
    }
}

// Public API

void retired_load_history_clear(void) {
    retired_load_history_first_row = 0;
    retired_load_history_num_rows  = 0;

    for (unsigned int bucket = 0; bucket < RETIRED_LOAD_HISTORY_NUM_BUCKETS;
         bucket++) {
        retired_load_history_bucket_heads[bucket] = -1;
    }

    memset(retired_load_history_rows, 0, sizeof(retired_load_history_rows));
    for (unsigned int row_num = 0; row_num < RETIRED_LOAD_HISTORY_CAPACITY;
         row_num++) {
        retired_load_history_rows[row_num].next_bucket_row = -1;
    }
}

void retired_load_history_insert(Addr load_pc_addr,
                                 Addr load_effective_addr,
                                 unsigned int load_memory_access_size,
                                 unsigned int load_micro_op_num) {
    retired_load_history_prune(load_micro_op_num);

    if (retired_load_history_num_rows >= RETIRED_LOAD_HISTORY_CAPACITY) {
        retired_load_history_discard_oldest_row();
    }

    unsigned int row_num =
        (retired_load_history_first_row + retired_load_history_num_rows) %
        RETIRED_LOAD_HISTORY_CAPACITY;
    RetiredLoadHistoryRow* row = &retired_load_history_rows[row_num];

    row->load_pc_addr            = load_pc_addr;
    row->load_effective_addr     = load_effective_addr;
    row->load_cacheblock_addr =
        retired_load_history_cacheblock_addr(load_effective_addr);
    row->load_memory_access_size = load_memory_access_size;
    row->load_micro_op_num       = load_micro_op_num;
    row->next_bucket_row         = -1;
    row->valid                   = true;

    unsigned int bucket = retired_load_history_bucket(row->load_cacheblock_addr);
    row->next_bucket_row = retired_load_history_bucket_heads[bucket];
    retired_load_history_bucket_heads[bucket] = (int)row_num;
    retired_load_history_num_rows++;
}

bool retired_load_history_find_and_remove_match(
    Addr current_load_effective_addr,
    unsigned int current_load_micro_op_num,
    RetiredLoadHistoryEntry* matched_load) {
    retired_load_history_prune(current_load_micro_op_num);

    Addr current_load_cacheblock_addr =
        retired_load_history_cacheblock_addr(current_load_effective_addr);
    unsigned int bucket =
        retired_load_history_bucket(current_load_cacheblock_addr);

    RetiredLoadHistoryRow* best_row = NULL;
    unsigned int best_row_num = 0;

    for (int row_num = retired_load_history_bucket_heads[bucket]; row_num >= 0;
         row_num = retired_load_history_rows[(unsigned int)row_num]
                       .next_bucket_row) {
        RetiredLoadHistoryRow* row =
            &retired_load_history_rows[(unsigned int)row_num];

        if (!row->valid) {
            continue;
        }
        if (row->load_micro_op_num >= current_load_micro_op_num) {
            continue;
        }
        if (current_load_micro_op_num - row->load_micro_op_num >
            IFUSE_FUSION_DISTANCE) {
            continue;
        }
        if (row->load_cacheblock_addr != current_load_cacheblock_addr) {
            continue;
        }
        if (!best_row || row->load_micro_op_num > best_row->load_micro_op_num) {
            best_row = row;
            best_row_num = (unsigned int)row_num;
        }
    }

    if (!best_row) {
        return false;
    }

    matched_load->load_pc_addr            = best_row->load_pc_addr;
    matched_load->load_effective_addr     = best_row->load_effective_addr;
    matched_load->load_memory_access_size = best_row->load_memory_access_size;
    matched_load->load_micro_op_num       = best_row->load_micro_op_num;

    retired_load_history_unlink_row(best_row_num);
    best_row->valid = false;
    return true;
}

void retired_load_history_invalidate_cacheblock(Addr store_cacheblock_addr) {
    unsigned int bucket = retired_load_history_bucket(store_cacheblock_addr);
    int* bucket_link = &retired_load_history_bucket_heads[bucket];

    while (*bucket_link >= 0) {
        unsigned int row_num = (unsigned int)(*bucket_link);
        RetiredLoadHistoryRow* row = &retired_load_history_rows[row_num];

        if (row->load_cacheblock_addr == store_cacheblock_addr) {
            *bucket_link = row->next_bucket_row;
            row->next_bucket_row = -1;
            row->valid = false;
        } else {
            bucket_link = &row->next_bucket_row;
        }
    }
}
