#include "ifuse_training_table.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../statistics.h"
#include "ifuse_fct.h"
#include "ifuse_plru.h"
#include "ifuse.param.h"

typedef struct TrainingEntry {
    uint64_t ld1_tag;
    uint64_t ld2_tag;
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

static uint64_t pair_hash(Addr ld1_pc, Addr ld2_pc, unsigned int delta,
                          bool direction, unsigned int mem_size) {
    uint64_t key = (uint64_t)ld1_pc ^ ((uint64_t)ld2_pc << 17U) ^
                   ((uint64_t)ld2_pc >> 11U);
    key ^= (uint64_t)delta << 7U;
    key ^= (uint64_t)mem_size << 14U;
    key ^= direction ? 0x9e3779b97f4a7c15ULL : 0U;
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return key;
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
    if (IFUSE_TRAINING_INSERT_THRESHOLD == 0U ||
        IFUSE_TRAINING_INSERT_THRESHOLD > 1023U) {
        fprintf(stderr, "I-Fuse training threshold must be 1-1023 for the "
                        "modeled 10-bit counter\n");
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

static bool keys_match(const TrainingEntry* entry, uint64_t ld1_tag,
                       uint64_t ld2_tag, unsigned int delta, bool direction,
                       unsigned int mem_size) {
    return entry->valid && entry->ld1_tag == ld1_tag &&
           entry->ld2_tag == ld2_tag && entry->offset_delta == delta &&
           entry->direction == direction && entry->ld2_mem_size == mem_size;
}

static TrainingEntry* find_or_allocate(Addr ld1_pc, Addr ld2_pc,
                                       unsigned int delta, bool direction,
                                       unsigned int mem_size, uns proc_id) {
    uint64_t hash = pair_hash(ld1_pc, ld2_pc, delta, direction, mem_size);
    unsigned int set = (unsigned int)(hash & (num_sets - 1U));
    uint64_t ld1_tag = pc_tag(ld1_pc);
    uint64_t ld2_tag = pc_tag(ld2_pc);
    unsigned int invalid_way = num_ways;

    STAT_EVENT(proc_id, TRAINING_TABLE_LOOKUPS);
    for (unsigned int way = 0; way < num_ways; ++way) {
        TrainingEntry* entry = entry_at(set, way);
        if (keys_match(entry, ld1_tag, ld2_tag, delta, direction, mem_size)) {
            STAT_EVENT(proc_id, TRAINING_TABLE_HITS);
            ifuse_plru_touch(plru, set, way, num_ways);
            return entry;
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

    TrainingEntry* entry = find_or_allocate(ld1_pc, ld2_pc, delta, direction,
                                             ld2_mem_size, proc_id);
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
