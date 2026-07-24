#include "ifuse_retired_load_history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../general.param.h"
#include "ifuse.param.h"

#define HISTORY_CAPACITY 512U
#define HISTORY_LINE_SIZE  64U

/*
 * Direct-mapped retired load buffer: one slot per cache block (modulo table
 * size). Index = (block_addr >> 6) & (CAPACITY - 1). Each slot holds the most
 * recent retired load to that 64-byte block (or the block that aliases into
 * the slot). Each entry stores a partial PC tag (IFUSE_RLB_PC_TAG_BITS,
 * default 32), 33-bit block tag, 6-bit line offset, 3-bit log2(size), 10-bit
 * micro-op timestamp, and valid — 85 bits/entry, 512 * 85 = 43,520 bits ≈ 5.3 KiB.
 */
typedef struct HistorySlot {
    RetiredLoadHistoryEntry load;
    Addr block;
    bool valid;
} HistorySlot;

static HistorySlot slots[HISTORY_CAPACITY];
static bool initialized;

static Addr rlb_pc_tag(Addr pc) {
    unsigned int bits = IFUSE_RLB_PC_TAG_BITS;
    if (bits >= 64U)
        return pc;
    if (bits == 0U)
        return 0;
    return pc & (Addr)((1ULL << bits) - 1ULL);
}

static Addr block_addr(Addr addr) {
    return addr & ~(Addr)(HISTORY_LINE_SIZE - 1U);
}

static unsigned int index_for_block(Addr block) {
    return (unsigned int)((block >> 6) & (HISTORY_CAPACITY - 1U));
}

static bool entry_within_distance(const HistorySlot* slot,
                                  Counter micro_op_num) {
    return slot->valid &&
           micro_op_num > slot->load.micro_op_num &&
           micro_op_num - slot->load.micro_op_num < IFUSE_FUSION_DISTANCE;
}

void retired_load_history_init(void) {
    if (!IFUSE_FUSION_DISTANCE || IFUSE_FUSION_DISTANCE > HISTORY_CAPACITY) {
        fprintf(stderr, "Runtime I-Fuse fusion distance must be 1-%u "
                        "micro-ops (got %u)\n",
                HISTORY_CAPACITY, IFUSE_FUSION_DISTANCE);
        exit(1);
    }
    if (IFUSE_RLB_PC_TAG_BITS == 0U || IFUSE_RLB_PC_TAG_BITS > 64U) {
        fprintf(stderr, "I-Fuse RLB PC tag bits must be 1-64\n");
        exit(1);
    }
    memset(slots, 0, sizeof(slots));
    initialized = true;
}

static void clear_slot(unsigned int idx) {
    memset(&slots[idx], 0, sizeof(slots[idx]));
}

static HistorySlot* slot_for_block(Addr block) {
    return &slots[index_for_block(block)];
}

void retired_load_history_insert(Addr pc, Addr effective_addr, uns mem_size,
                                 Counter micro_op_num) {
    if (!initialized)
        retired_load_history_init();

    Addr block = block_addr(effective_addr);
    HistorySlot* slot = slot_for_block(block);

    slot->load.pc = rlb_pc_tag(pc);
    slot->load.effective_addr = effective_addr;
    slot->load.mem_size = mem_size;
    slot->load.micro_op_num = micro_op_num;
    slot->block = block;
    slot->valid = true;
}

bool retired_load_history_take_match(Addr effective_addr,
                                     Counter micro_op_num,
                                     RetiredLoadHistoryEntry* match) {
    if (!initialized)
        retired_load_history_init();

    Addr block = block_addr(effective_addr);
    HistorySlot* slot = slot_for_block(block);
    if (!slot->valid || slot->block != block ||
        !entry_within_distance(slot, micro_op_num))
        return false;

    *match = slot->load;
    clear_slot(index_for_block(block));
    return true;
}

void retired_load_history_invalidate_block(Addr effective_addr) {
    if (!initialized)
        retired_load_history_init();

    Addr block = block_addr(effective_addr);
    HistorySlot* slot = slot_for_block(block);
    if (slot->valid && slot->block == block)
        clear_slot(index_for_block(block));
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
