#include <assert.h>
#include <stdio.h>

#include "ifuse/ifuse_plru.h"
#include "ifuse/ifuse_retired_load_history.h"

const uns IFUSE_FUSION_DISTANCE = 512;
const uns IFUSE_RLB_CAPACITY = 128;

static void test_micro_op_distance(void) {
    RetiredLoadHistoryEntry match;

    retired_load_history_init();
    retired_load_history_insert(0x1000, 0x2040, 8, 10);
    assert(retired_load_history_take_match(0x2048, 521, &match));
    assert(match.pc == 0x1000);

    retired_load_history_init();
    retired_load_history_insert(0x1000, 0x2040, 8, 10);
    assert(!retired_load_history_take_match(0x2048, 522, &match));
}

static void test_most_recent_and_store_invalidation(void) {
    RetiredLoadHistoryEntry match;

    retired_load_history_init();
    retired_load_history_insert(0x1000, 0x3000, 8, 10);
    retired_load_history_insert(0x2000, 0x3008, 8, 20);
    assert(retired_load_history_take_match(0x3010, 30, &match));
    assert(match.pc == 0x2000);

    retired_load_history_init();
    retired_load_history_insert(0x1000, 0x4000, 8, 10);
    retired_load_history_invalidate_block(0x4018);
    assert(!retired_load_history_take_match(0x4020, 20, &match));

    retired_load_history_init();
    retired_load_history_insert(0x1000, 0x4038, 8, 10);
    retired_load_history_insert(0x2000, 0x4040, 8, 11);
    retired_load_history_invalidate_range(0x403c, 8);
    assert(!retired_load_history_take_match(0x4038, 20, &match));
    assert(!retired_load_history_take_match(0x4040, 20, &match));
}

static void test_capacity_forced_eviction(void) {
    RetiredLoadHistoryEntry match;

    retired_load_history_init();
    /* Fill the RLB to exactly IFUSE_RLB_CAPACITY entries, all well within
     * IFUSE_FUSION_DISTANCE of each other -- nothing here ages out. */
    for (unsigned int i = 0; i < IFUSE_RLB_CAPACITY; ++i) {
        Addr block = 0x100000 + (Addr)i * 0x1000;
        retired_load_history_insert(0x5000 + i, block, 8, i + 1);
    }

    /* One more insert must force something out early via capacity, not age. */
    retired_load_history_insert(0x9000, 0x900000, 8, IFUSE_RLB_CAPACITY + 1);

    /* The very first (globally oldest) entry is gone despite being nowhere
     * near IFUSE_FUSION_DISTANCE old. */
    assert(!retired_load_history_take_match(0x100008, IFUSE_RLB_CAPACITY + 2,
                                            &match));

    /* A recently inserted entry should still be resident and match. */
    Addr last_block = 0x100000 + (Addr)(IFUSE_RLB_CAPACITY - 1) * 0x1000;
    assert(retired_load_history_take_match(last_block + 8,
                                           IFUSE_RLB_CAPACITY + 2, &match));
    assert(match.pc == 0x5000 + (IFUSE_RLB_CAPACITY - 1));
}

static void test_plru(void) {
    uint8_t state[1] = {0};
    ifuse_plru_touch(state, 0, 0, 4);
    assert(ifuse_plru_victim(state, 0, 4) != 0);
    ifuse_plru_touch(state, 0, 7, 8);
    assert(ifuse_plru_victim(state, 0, 8) != 7);
}

int main(void) {
    test_micro_op_distance();
    test_most_recent_and_store_invalidation();
    test_capacity_forced_eviction();
    test_plru();
    puts("runtime I-Fuse history tests passed");
    return 0;
}
