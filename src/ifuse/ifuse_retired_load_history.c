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
    uint64_t     load_num;
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

static unsigned int retired_load_history_row_num(unsigned int logical_row) {
    return (retired_load_history_first_row + logical_row) %
           RETIRED_LOAD_HISTORY_CAPACITY;
}

/**
 * Rebuilds the software-only cache-block index after rows move.
 *
 * The history contains at most 512 rows, so rebuilding the index after a
 * removal keeps deletion bookkeeping simple without affecting the modeled
 * hardware structure.
 */
static void retired_load_history_rebuild_index(void) {
    for (unsigned int bucket = 0; bucket < RETIRED_LOAD_HISTORY_NUM_BUCKETS;
         bucket++) {
        retired_load_history_bucket_heads[bucket] = -1;
    }

    for (unsigned int logical_row = 0;
         logical_row < retired_load_history_num_rows; logical_row++) {
        unsigned int row_num = retired_load_history_row_num(logical_row);
        RetiredLoadHistoryRow* row = &retired_load_history_rows[row_num];
        unsigned int bucket =
            retired_load_history_bucket(row->load_cacheblock_addr);

        row->next_bucket_row = retired_load_history_bucket_heads[bucket];
        retired_load_history_bucket_heads[bucket] = (int)row_num;
    }
}

/**
 * Removes one logical row and compacts newer rows toward the oldest slot.
 *
 * Keeping the active circular-buffer window contiguous is important: insert,
 * expiry, and capacity eviction all derive their positions from first_row and
 * num_rows. Leaving an invalid hole would eventually overwrite a live row.
 *
 * @param logical_row Zero-based row position relative to the oldest entry.
 */
static void retired_load_history_remove_row(unsigned int logical_row) {
    if (logical_row >= retired_load_history_num_rows) {
        return;
    }

    for (unsigned int move_row = logical_row;
         move_row + 1U < retired_load_history_num_rows; move_row++) {
        unsigned int dst_row_num = retired_load_history_row_num(move_row);
        unsigned int src_row_num = retired_load_history_row_num(move_row + 1U);
        retired_load_history_rows[dst_row_num] =
            retired_load_history_rows[src_row_num];
    }

    unsigned int last_row_num =
        retired_load_history_row_num(retired_load_history_num_rows - 1U);
    memset(&retired_load_history_rows[last_row_num], 0,
           sizeof(retired_load_history_rows[last_row_num]));
    retired_load_history_rows[last_row_num].next_bucket_row = -1;
    retired_load_history_num_rows--;
    retired_load_history_rebuild_index();
}

/**
 * Removes the oldest history row.
 */
static void retired_load_history_discard_oldest_row(void) {
    if (retired_load_history_num_rows == 0) {
        return;
    }

    retired_load_history_remove_row(0);
}

/**
 * Removes rows whose dynamic distance is beyond IFUSE_FUSION_DISTANCE.
 *
 * @param current_load_num The newest retiring load's dynamic load number.
 */
static void retired_load_history_prune(uint64_t current_load_num) {
    uint64_t min_load_num =
        (current_load_num > IFUSE_FUSION_DISTANCE) ?
            (current_load_num - IFUSE_FUSION_DISTANCE) : 1U;

    while (retired_load_history_num_rows > 0 &&
           retired_load_history_rows[retired_load_history_first_row]
                   .load_num < min_load_num) {
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
                                 unsigned int load_micro_op_num,
                                 uint64_t load_num) {
    retired_load_history_prune(load_num);

    if (retired_load_history_num_rows >= RETIRED_LOAD_HISTORY_CAPACITY) {
        retired_load_history_discard_oldest_row();
    }

    unsigned int row_num =
        retired_load_history_row_num(retired_load_history_num_rows);
    RetiredLoadHistoryRow* row = &retired_load_history_rows[row_num];

    row->load_pc_addr            = load_pc_addr;
    row->load_effective_addr     = load_effective_addr;
    row->load_cacheblock_addr =
        retired_load_history_cacheblock_addr(load_effective_addr);
    row->load_memory_access_size = load_memory_access_size;
    row->load_micro_op_num       = load_micro_op_num;
    row->load_num                = load_num;
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
    uint64_t current_load_num,
    RetiredLoadHistoryEntry* matched_load) {
    retired_load_history_prune(current_load_num);

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
        if (current_load_num - row->load_num >
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

    unsigned int best_logical_row =
        (best_row_num + RETIRED_LOAD_HISTORY_CAPACITY -
         retired_load_history_first_row) %
        RETIRED_LOAD_HISTORY_CAPACITY;
    retired_load_history_remove_row(best_logical_row);
    return true;
}

void retired_load_history_invalidate_cacheblock(Addr store_cacheblock_addr) {
    unsigned int logical_row = 0;
    while (logical_row < retired_load_history_num_rows) {
        unsigned int row_num = retired_load_history_row_num(logical_row);
        RetiredLoadHistoryRow* row = &retired_load_history_rows[row_num];
        if (row->load_cacheblock_addr == store_cacheblock_addr) {
            /*
             * Removal compacts the next row into this logical position. Check
             * the same position again so one store removes every older load
             * touching its cache block.
             */
            retired_load_history_remove_row(logical_row);
        } else {
            logical_row++;
        }
    }
}
