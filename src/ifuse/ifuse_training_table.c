#include "ifuse_training_table.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../statistics.h"
#include "ifuse_fct.h"
#include "ifuse.param.h"

/*
 * Hardware-design storage model (the C fields below intentionally use normal
 * host types): each entry packs a 32-bit LD1 PC tag (IFUSE_TRAINING_TABLE_
 * PC_TAG_BITS -- with at most a few hundred live entries, aliasing
 * probability is ~3e-5, and a collision only ever merges two candidates'
 * observation counts, never corrupts a promoted PC; see the note on
 * find_or_allocate() below for why) plus a full 48-bit LD2 PC (kept
 * untruncated: it doubles as the value forwarded to FCT_Row.ld2_pc_addr,
 * which must be full precision -- see pc_tag()'s comment below), an
 * observation counter just wide enough for the insert threshold (10 bits at
 * the default 1000, 7 bits at the swept 100), a 6-bit cache-line offset
 * delta, a 3-bit log2(LD2 access size), direction, valid, and a 2-bit RRPV
 * replacement-priority field, totaling 103 bits at the default threshold.
 * There are 32 sets * 4 ways = 128 entries by default, so the packed
 * training table costs 128 * 103 = 13,184 bits = 1.61 KiB.
 *
 * Replacement is RRIP-style (SRRIP): a newly inserted candidate starts at
 * RRPV = IFUSE_TT_RRPV_INSERT (one below max), not at max and not at zero,
 * so it must be re-observed to earn protection instead of being either
 * evicted immediately or fully protected on arrival. Each repeat observation
 * decrements RRPV toward 0 (protected); eviction always takes the way with
 * the highest RRPV in the set (ties break to the lowest way index).
 */
#define IFUSE_TRAINING_OBS_COUNTER_MAX 2047U
#define IFUSE_TT_RRPV_BITS 2U
#define IFUSE_TT_RRPV_MAX ((1U << IFUSE_TT_RRPV_BITS) - 1U)
#define IFUSE_TT_RRPV_INSERT (IFUSE_TT_RRPV_MAX - 1U)
typedef struct TrainingEntry {
    uint64_t ld1_tag;
    uint64_t ld2_tag;
    uint16_t observations;
    uint8_t offset_delta;
    uint8_t ld2_mem_size;
    uint8_t rrpv;
    bool direction;
    bool valid;
} TrainingEntry;

static TrainingEntry* entries;
static unsigned int num_sets;
static unsigned int num_ways;
static uint64_t live_entries;
static uint64_t peak_live_entries;
static bool initialized;

static bool power_of_two(unsigned int value) {
    return value && !(value & (value - 1U));
}

/*
 * Truncates ld1_pc to IFUSE_TRAINING_TABLE_PC_TAG_BITS. Applied only to the
 * LD1 tag: ld1_pc is a pure lookup/disambiguation role in the TT (never read
 * back as a value), whereas ld2_pc doubles as the value forwarded to
 * fct_install_runtime_candidate() -> FCT_Row.ld2_pc_addr, which must be a
 * full-precision PC (same reasoning as FCT_Row.ld2_pc_addr in ifuse_fct.h),
 * so ld2_tag is stored and compared at full width, untouched by this.
 */
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
        IFUSE_TRAINING_INSERT_THRESHOLD > IFUSE_TRAINING_OBS_COUNTER_MAX) {
        fprintf(stderr,
                "I-Fuse training threshold must be 1-%u for the modeled "
                "11-bit counter\n",
                IFUSE_TRAINING_OBS_COUNTER_MAX);
        exit(1);
    }

    entries = (TrainingEntry*)calloc((size_t)num_sets * num_ways,
                                     sizeof(*entries));
    if (!entries) {
        fprintf(stderr, "Could not allocate I-Fuse runtime training table\n");
        exit(1);
    }
    initialized = true;
}

/* RRIP victim: the way with the highest RRPV (least protected) in the set,
 * ties breaking to the lowest way index. */
static unsigned int rrip_victim(unsigned int set) {
    unsigned int victim = 0;
    uint8_t victim_rrpv = entry_at(set, 0)->rrpv;
    for (unsigned int way = 1; way < num_ways; ++way) {
        uint8_t rrpv = entry_at(set, way)->rrpv;
        if (rrpv > victim_rrpv) {
            victim = way;
            victim_rrpv = rrpv;
        }
    }
    return victim;
}

static bool keys_match(const TrainingEntry* entry, uint64_t ld1_tag,
                       uint64_t ld2_tag, unsigned int delta, bool direction,
                       unsigned int mem_size) {
    return entry->valid && entry->ld1_tag == ld1_tag &&
           entry->ld2_tag == ld2_tag && entry->offset_delta == delta &&
           entry->direction == direction && entry->ld2_mem_size == mem_size;
}

/*
 * ld1_tag (truncated via pc_tag()) exists only to answer "is this the same
 * candidate as an existing entry" for the hit/miss/observation-count
 * decision below -- it is never read back out to reconstruct a PC.
 * training_table_observe() always forwards its own full-precision ld1_pc
 * argument (the caller's real dynamic PC) to
 * fct_install_runtime_candidate(), not this entry's stored ld1_tag. ld2_tag
 * is kept at full width (see pc_tag()'s comment above) precisely so that
 * even a stored-and-compared value, not just the call-site parameter,
 * carries the exact LD2 PC forward -- belt-and-suspenders alongside the
 * call-site guarantee, at essentially no extra risk since ld1_tag's 32 bits
 * already make an accidental match astronomically unlikely on their own.
 */
static TrainingEntry* find_or_allocate(Addr ld1_pc, Addr ld2_pc,
                                       unsigned int delta, bool direction,
                                       unsigned int mem_size, uns proc_id) {
    uint64_t hash = pair_hash(ld1_pc, ld2_pc, delta, direction, mem_size);
    unsigned int set = (unsigned int)(hash & (num_sets - 1U));
    uint64_t ld1_tag = pc_tag(ld1_pc);
    uint64_t ld2_tag = (uint64_t)ld2_pc;
    unsigned int invalid_way = num_ways;

    STAT_EVENT(proc_id, TRAINING_TABLE_LOOKUPS);
    for (unsigned int way = 0; way < num_ways; ++way) {
        TrainingEntry* entry = entry_at(set, way);
        if (keys_match(entry, ld1_tag, ld2_tag, delta, direction, mem_size)) {
            STAT_EVENT(proc_id, TRAINING_TABLE_HITS);
            if (entry->rrpv > 0)
                --entry->rrpv;
            return entry;
        }
        if (!entry->valid && invalid_way == num_ways)
            invalid_way = way;
    }

    STAT_EVENT(proc_id, TRAINING_TABLE_MISSES);
    unsigned int way = invalid_way;
    if (way == num_ways) {
        way = rrip_victim(set);
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
    entry->rrpv = (uint8_t)IFUSE_TT_RRPV_INSERT;
    entry->valid = true;
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
