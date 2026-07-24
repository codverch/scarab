#include "ifuse_training_table.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../statistics.h"
#include "ifuse_fct.h"
#include "ifuse_plru.h"
#include "ifuse.param.h"

/*
 * Hardware-design storage model (the C fields below intentionally use normal
 * host types): each entry packs two 48-bit PC tags, a 14-bit observation
 * counter (supports insert thresholds up to 16383, e.g. 10/100/1000/10000),
 * a 6-bit cache-line offset delta, a 3-bit log2(LD2 access size), direction,
 * a partial cache-block tag, and valid. There are 32 sets * 4 ways = 128
 * entries. Three tree-PLRU bits per set add 96 bits. Sets are indexed by a
 * hash of the concatenated LD1/LD2 PC tags; ways distinguish offset delta,
 * direction, LD2 access size, and the partial cache-block tag. Observing the
 * same LD1/LD2 PC pair on a different cache block evicts the stale entry.
 */
#define IFUSE_TRAINING_OBS_COUNTER_MAX 16383U
#define IFUSE_TRAINING_LINE_SIZE       64U
typedef struct TrainingEntry {
    uint64_t ld1_tag;
    uint64_t ld2_tag;
    uint32_t block_tag;
    uint16_t observations;
    uint8_t offset_delta;
    uint8_t ld2_mem_size;
    bool direction;
    bool valid;
} TrainingEntry;

static TrainingEntry* entries;
static uint8_t* plru;
static unsigned int num_sets;
static unsigned int num_ways;
static uint64_t live_entries;
static uint64_t peak_live_entries;
static bool initialized;

static bool power_of_two(unsigned int value) {
    return value && !(value & (value - 1U));
}

static uint64_t pc_tag(Addr pc) {
    unsigned int bits = IFUSE_TRAINING_TABLE_PC_TAG_BITS;
    if (bits >= 64U)
        return (uint64_t)pc;
    if (bits == 0U)
        return 0U;
    return (uint64_t)pc & ((1ULL << bits) - 1ULL);
}

static Addr block_addr(Addr addr) {
    return addr & ~(Addr)(IFUSE_TRAINING_LINE_SIZE - 1U);
}

static uint32_t block_tag_for(Addr effective_addr) {
    unsigned int bits = IFUSE_TRAINING_TABLE_BLOCK_TAG_BITS;
    Addr block = block_addr(effective_addr);
    Addr line_index = block >> 6;
    if (bits == 0U)
        return 0U;
    if (bits >= 64U)
        return (uint32_t)line_index;
    return (uint32_t)(line_index & ((1ULL << bits) - 1ULL));
}

static uint64_t pair_pc_concat(uint64_t ld1_tag, uint64_t ld2_tag) {
    unsigned int tag_bits = IFUSE_TRAINING_TABLE_PC_TAG_BITS;
    uint64_t mask = (tag_bits >= 64U) ? ~0ULL : ((1ULL << tag_bits) - 1ULL);
    return ((ld1_tag & mask) << tag_bits) | (ld2_tag & mask);
}

static unsigned int set_for_pair(Addr ld1_pc, Addr ld2_pc) {
    uint64_t key = pair_pc_concat(pc_tag(ld1_pc), pc_tag(ld2_pc));
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return (unsigned int)(key & (num_sets - 1U));
}

static TrainingEntry* entry_at(unsigned int set, unsigned int way) {
    return &entries[set * num_ways + way];
}

void training_table_init(void) {
    if (initialized)
        return;

    num_sets = IFUSE_TRAINING_TABLE_SETS;
    num_ways = IFUSE_TRAINING_TABLE_WAYS;
    if (!power_of_two(num_sets) || (num_ways != 4U && num_ways != 8U)) {
        fprintf(stderr, "I-Fuse training table requires power-of-two sets "
                        "and 4 or 8 ways (got %u x %u)\n",
                num_sets, num_ways);
        exit(1);
    }
    if (IFUSE_TRAINING_TABLE_PC_TAG_BITS == 0U ||
        IFUSE_TRAINING_TABLE_PC_TAG_BITS > 64U) {
        fprintf(stderr, "I-Fuse training table PC tag bits must be 1-64\n");
        exit(1);
    }
    if (IFUSE_TRAINING_TABLE_BLOCK_TAG_BITS > 64U) {
        fprintf(stderr, "I-Fuse training table block tag bits must be 0-64\n");
        exit(1);
    }
    if (IFUSE_TRAINING_INSERT_THRESHOLD == 0U ||
        IFUSE_TRAINING_INSERT_THRESHOLD > IFUSE_TRAINING_OBS_COUNTER_MAX) {
        fprintf(stderr,
                "I-Fuse training threshold must be 1-%u for the modeled "
                "14-bit counter\n",
                IFUSE_TRAINING_OBS_COUNTER_MAX);
        exit(1);
    }

    entries = (TrainingEntry*)calloc((size_t)num_sets * num_ways,
                                     sizeof(*entries));
    plru = (uint8_t*)calloc(num_sets, sizeof(*plru));
    if (!entries || !plru) {
        fprintf(stderr, "Could not allocate I-Fuse runtime training table\n");
        exit(1);
    }
    initialized = true;
}

static bool pc_pair_match(const TrainingEntry* entry, uint64_t ld1_tag,
                          uint64_t ld2_tag) {
    return entry->valid && entry->ld1_tag == ld1_tag &&
           entry->ld2_tag == ld2_tag;
}

static void evict_entry(unsigned int set, unsigned int way, uns proc_id) {
    TrainingEntry* entry = entry_at(set, way);
    if (!entry->valid)
        return;
    memset(entry, 0, sizeof(*entry));
    if (live_entries)
        --live_entries;
    STAT_EVENT(proc_id, TRAINING_TABLE_BLOCK_EVICTIONS);
}

static bool signature_match(const TrainingEntry* entry, uint64_t ld1_tag,
                            uint64_t ld2_tag, unsigned int delta, bool direction,
                            unsigned int mem_size) {
    return entry->valid && entry->ld1_tag == ld1_tag &&
           entry->ld2_tag == ld2_tag && entry->offset_delta == delta &&
           entry->direction == direction && entry->ld2_mem_size == mem_size;
}

static bool full_match(const TrainingEntry* entry, uint64_t ld1_tag,
                       uint64_t ld2_tag, uint32_t block_tag,
                       unsigned int delta, bool direction,
                       unsigned int mem_size) {
    return signature_match(entry, ld1_tag, ld2_tag, delta, direction,
                           mem_size) && entry->block_tag == block_tag;
}

static TrainingEntry* find_or_allocate(Addr ld1_pc, Addr ld2_pc,
                                       Addr ld2_effective_addr,
                                       unsigned int delta, bool direction,
                                       unsigned int mem_size, uns proc_id) {
    uint64_t ld1_tag = pc_tag(ld1_pc);
    uint64_t ld2_tag = pc_tag(ld2_pc);
    uint32_t block_tag = block_tag_for(ld2_effective_addr);
    unsigned int set = set_for_pair(ld1_pc, ld2_pc);
    unsigned int invalid_way = num_ways;

    STAT_EVENT(proc_id, TRAINING_TABLE_LOOKUPS);
    for (unsigned int way = 0; way < num_ways; ++way) {
        TrainingEntry* entry = entry_at(set, way);
        if (full_match(entry, ld1_tag, ld2_tag, block_tag, delta, direction,
                       mem_size)) {
            STAT_EVENT(proc_id, TRAINING_TABLE_HITS);
            ifuse_plru_touch(plru, set, way, num_ways);
            return entry;
        }
        if (pc_pair_match(entry, ld1_tag, ld2_tag) &&
            entry->block_tag != block_tag) {
            evict_entry(set, way, proc_id);
            if (invalid_way == num_ways)
                invalid_way = way;
            continue;
        }
        if (!entry->valid && invalid_way == num_ways)
            invalid_way = way;
    }

    STAT_EVENT(proc_id, TRAINING_TABLE_MISSES);
    unsigned int way = invalid_way;
    if (way == num_ways) {
        way = ifuse_plru_victim(plru, set, num_ways);
        STAT_EVENT(proc_id, TRAINING_TABLE_EVICTIONS);
    } else {
        ++live_entries;
        if (live_entries > peak_live_entries) {
            INC_STAT_EVENT(proc_id, TRAINING_TABLE_PEAK_LIVE,
                           live_entries - peak_live_entries);
            peak_live_entries = live_entries;
        }
    }

    TrainingEntry* entry = entry_at(set, way);
    entry->ld1_tag = ld1_tag;
    entry->ld2_tag = ld2_tag;
    entry->block_tag = block_tag;
    entry->offset_delta = (uint8_t)delta;
    entry->ld2_mem_size = (uint8_t)mem_size;
    entry->direction = direction;
    entry->observations = 0;
    entry->valid = true;
    ifuse_plru_touch(plru, set, way, num_ways);
    STAT_EVENT(proc_id, TRAINING_TABLE_INSERTS);
    return entry;
}

void training_table_observe(Addr ld1_pc, Addr ld2_pc,
                            Addr ld1_effective_addr,
                            Addr ld2_effective_addr, uns ld2_mem_size,
                            Counter ld1_micro_op_num,
                            Counter ld2_micro_op_num, uns proc_id) {
    if (!IFUSE_RUNTIME_TRAINING_ENABLED)
        return;
    if (!initialized)
        training_table_init();

    unsigned int ld1_offset = (unsigned int)(ld1_effective_addr & 63U);
    unsigned int ld2_offset = (unsigned int)(ld2_effective_addr & 63U);
    bool direction = ld2_offset >= ld1_offset;
    unsigned int delta = direction ? ld2_offset - ld1_offset :
                                     ld1_offset - ld2_offset;

    TrainingEntry* entry = find_or_allocate(ld1_pc, ld2_pc, ld2_effective_addr,
                                             delta, direction, ld2_mem_size,
                                             proc_id);
    if (entry->observations < IFUSE_TRAINING_INSERT_THRESHOLD)
        ++entry->observations;
    STAT_EVENT(proc_id, TRAINING_TABLE_OBSERVATIONS);

    if (entry->observations < IFUSE_TRAINING_INSERT_THRESHOLD)
        return;

    if (fct_install_runtime_candidate(ld1_pc, ld2_pc, ld1_effective_addr,
                                      ld2_effective_addr, delta, direction,
                                      ld2_mem_size, ld1_micro_op_num,
                                      ld2_micro_op_num, proc_id)) {
        STAT_EVENT(proc_id, TRAINING_TABLE_PROMOTIONS);
        entry->valid = false;
        if (live_entries)
            --live_entries;
    } else {
        STAT_EVENT(proc_id, TRAINING_TABLE_PROMOTION_REJECTS);
    }
}
