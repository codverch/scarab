// STD headers
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Custom headers
#include "ifuse_fct.h"
#include "../general.param.h"
#include "../statistics.h"
#include "ifuse_ideal_limits.h"
#include "ifuse.param.h"
#include "ifuse_plru.h"

/**
 * Fusion Candidate Table (FCT) runtime policy.
 *
 * The FCT models a single LD2 candidate for each LD1 PC. 
 * It receives candidates from the retire-stage runtime training table. 
 * Correct predictions are promoted by the retire-stage runtime training table. 
 * Mispredictions are penalized.
 *
 * The table is organized like a cache: 2^IFUSE_FCT_HASH_BITS rows
 * split into IFUSE_FCT_WAYS ways, so 512 rows at the default 4 ways is 128
 * sets of 4. A hash of the LD1 PC picks one set, and only that set's tags are
 * compared. When a new LD1 needs a row and its set is full, tree-PLRU picks
 * the way to evict, the same replacement the APT and ACI use.
 */

static FCT_Row*     fct_rows = NULL;  // fct_num_sets * fct_num_ways rows, set-major
static uint8_t*     fct_plru = NULL;  // one tree-PLRU state byte per set
static unsigned int fct_num_sets = 0;
static unsigned int fct_num_ways = 0;
static bool         fct_is_initialized = false;

/* Modeled hardware width of confidence_score -- see the FCT_Row storage-model
 * comment in ifuse_fct.h. 10 bits gives 2x headroom over the current default
 * confidence ceiling of 500 (max of ifuse_fct_preload_conf and
 * ifuse_fct_runtime_insert_conf). */
#define IFUSE_FCT_CONFIDENCE_MAX 1023U

static void fct_preload_from_file(void);

/*
 * Truncates ld1_pc_addr to IFUSE_FCT_PC_TAG_BITS for storage/comparison, the
 * same way ifuse_training_table.c's pc_tag() truncates TT tags. Only the
 * stored/compared LD1 tag shrinks; fct_set_index() below still hashes the
 * full PC to pick the set, and ld2_pc_addr is never truncated -- see the
 * FCT_Row storage-model comment in ifuse_fct.h for why.
 */
static uint64_t fct_pc_tag(Addr pc) {
    unsigned int bits = IFUSE_FCT_PC_TAG_BITS;
    if (bits >= 64U)
        return (uint64_t)pc;
    if (bits == 0U)
        return 0U;
    return (uint64_t)pc & ((1ULL << bits) - 1ULL);
}

void fct_init(void) {
    if (fct_is_initialized) {
        return;
    }

    if (IFUSE_FCT_HASH_BITS == 0U ||
        IFUSE_FCT_HASH_BITS > IFUSE_IDEAL_FCT_MAX_HASH_BITS) {
        fprintf(stderr,
                "FCT: ifuse_fct_hash_bits must be in [1, %u]\n",
                IFUSE_IDEAL_FCT_MAX_HASH_BITS);
        exit(1);
    }
    if (IFUSE_FCT_PC_TAG_BITS == 0U || IFUSE_FCT_PC_TAG_BITS > 64U) {
        fprintf(stderr, "FCT: ifuse_fct_pc_tag_bits must be 1-64\n");
        exit(1);
    }
    if (IFUSE_FCT_PRELOAD_CONF > IFUSE_FCT_CONFIDENCE_MAX ||
        IFUSE_FCT_RUNTIME_INSERT_CONF > IFUSE_FCT_CONFIDENCE_MAX) {
        fprintf(stderr,
                "FCT: ifuse_fct_preload_conf and ifuse_fct_runtime_insert_conf "
                "must be <= %u for the modeled 10-bit confidence field\n",
                IFUSE_FCT_CONFIDENCE_MAX);
        exit(1);
    }

    const unsigned int num_rows = 1U << IFUSE_FCT_HASH_BITS;
    if ((IFUSE_FCT_WAYS != 4U && IFUSE_FCT_WAYS != 8U) ||
        num_rows < IFUSE_FCT_WAYS) {
        fprintf(stderr,
                "FCT: ifuse_fct_ways must be 4 or 8 (tree-PLRU) and no larger "
                "than 2^ifuse_fct_hash_bits\n");
        exit(1);
    }
    fct_num_ways = IFUSE_FCT_WAYS;
    fct_num_sets = num_rows / fct_num_ways;
    fct_rows = (FCT_Row*)calloc(num_rows, sizeof(FCT_Row));
    fct_plru = (uint8_t*)calloc(fct_num_sets, sizeof(*fct_plru));
    if (!fct_rows || !fct_plru) {
        fprintf(stderr, "FCT: calloc failed for %u rows\n", num_rows);
        exit(1);
    }
    fct_is_initialized = true;

    fct_preload_from_file();
}

/**
 * Returns the set that ld1_pc_addr maps to. Same PC hash as apt_set_index().
 */
static unsigned int fct_set_index(Addr ld1_pc_addr) {
    uint64_t h = (uint64_t)ld1_pc_addr;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (unsigned int)(h & (fct_num_sets - 1U));
}

static FCT_Row* fct_row_at(unsigned int set, unsigned int way) {
    return &fct_rows[set * fct_num_ways + way];
}

static unsigned int fct_row_way(const FCT_Row* row) {
    return (unsigned int)(row - fct_rows) % fct_num_ways;
}

/**
 * Returns the configured FCT confidence cap.
 *
 * The runtime policy treats the prediction threshold as the maximum useful
 * confidence. A row inserted at confidence 100 should not grow to 500 and keep
 * predicting through many penalties; once it reaches the prediction threshold,
 * it is fully trusted until a misprediction lowers it.
 */
static unsigned int fct_max_confidence_score(void) {
    return IFUSE_FCT_PRELOAD_CONF > IFUSE_FCT_RUNTIME_INSERT_CONF ?
        IFUSE_FCT_PRELOAD_CONF : IFUSE_FCT_RUNTIME_INSERT_CONF;
}

/**
 * Returns the confidence score after applying the configured confidence cap.
 */
static unsigned int fct_saturating_confidence_score(
    unsigned int confidence_score) {
    const unsigned int max_confidence_score = fct_max_confidence_score();
    return (confidence_score > max_confidence_score) ?
        max_confidence_score : confidence_score;
}

/**
 * Reinforces row without exceeding the configured prediction threshold.
 */
static void fct_increment_confidence_score(FCT_Row* row) {
    if (!row) {
        return;
    }

    row->confidence_score =
        fct_saturating_confidence_score(row->confidence_score + 1U);
}

static void fct_write_row(FCT_Row* row, Addr ld1_pc_addr, Addr ld2_pc_addr,
                          Addr ld1_effective_addr, Addr ld2_effective_addr,
                          unsigned int offset_delta,
                          bool direction, unsigned int ld2_mem_size,
                          Counter ld1_micro_op_num,
                          Counter ld2_micro_op_num,
                          unsigned int confidence_score) {
    row->ld1_pc_addr        = fct_pc_tag(ld1_pc_addr);
    row->ld2_pc_addr        = ld2_pc_addr;
    row->ld1_effective_addr = ld1_effective_addr;
    row->ld2_effective_addr = ld2_effective_addr;
    row->offset_delta       = offset_delta;
    row->direction          = direction;
    row->ld2_mem_size       = ld2_mem_size;
    row->ld1_micro_op_num   = ld1_micro_op_num;
    row->ld2_micro_op_num   = ld2_micro_op_num;
    row->valid              = true;
    row->confidence_score   = fct_saturating_confidence_score(confidence_score);
}

/**
 * Returns the row for ld1_pc_addr, or NULL. Only ld1_pc_addr's set is
 * searched, the way a cache compares tags within one set.
 */
static FCT_Row* fct_lookup_row(Addr ld1_pc_addr) {
    if (!fct_rows) {
        return NULL;
    }

    uint64_t ld1_tag = fct_pc_tag(ld1_pc_addr);
    unsigned int set = fct_set_index(ld1_pc_addr);
    for (unsigned int way = 0; way < fct_num_ways; way++) {
        FCT_Row* row = fct_row_at(set, way);
        if (row->valid && row->ld1_pc_addr == ld1_tag) {
            return row;
        }
    }
    return NULL;
}

/**
 * Marks row as the most recently used way of ld1_pc_addr's set.
 */
static void fct_touch_row(Addr ld1_pc_addr, const FCT_Row* row) {
    ifuse_plru_touch(fct_plru, fct_set_index(ld1_pc_addr), fct_row_way(row),
                     fct_num_ways);
}

/**
 * Returns the FCT row for ld1_pc_addr, allocating one if needed: an invalid
 * way if the set has one, otherwise the tree-PLRU victim, which is evicted.
 */
static FCT_Row* fct_allocate_row_for_load1_pc(Addr ld1_pc_addr) {
    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (row) {
        return row;
    }

    unsigned int set = fct_set_index(ld1_pc_addr);
    for (unsigned int way = 0; way < fct_num_ways; way++) {
        if (!fct_row_at(set, way)->valid) {
            return fct_row_at(set, way);
        }
    }

    STAT_EVENT(0, FCT_EVICTIONS);
    row = fct_row_at(set, ifuse_plru_victim(fct_plru, set, fct_num_ways));
    memset(row, 0, sizeof(*row));
    return row;
}

/*
 * FCT preload from a PGO PC-pairs file
 * ====================================
 * Instead of learning PC-pairs at runtime, the FCT can be populated up front
 * from a profile-guided candidates file named by IFUSE_FCT_PRELOAD_FILE
 * (e.g. src/pgo-candidates/pgo-candidates-frequency-<N>/<workload>/<simpoint>.csv;
 * the frequency threshold is chosen by pointing this knob at the matching
 * frequency directory). The file is CSV with a mandatory header:
 *
 *   load1_pc,load2_pc,offset_delta,load2_mem_size
 *
 * - load1_pc / load2_pc: instruction addresses (0x-prefixed hex or decimal)
 * - offset_delta:        signed cache-line byte offset LD2 - LD1 (-63..63);
 *                        the sign carries the FCT direction bit
 * - load2_mem_size:      LD2 memory access size in bytes
 *
 * One LD2 candidate per LD1: the first row for an LD1 PC wins and later rows
 * for the same LD1 PC are skipped. Preloaded rows start at
 * IFUSE_FCT_PRELOAD_CONF so the ordinary confidence mechanism (threshold to
 * predict, misprediction penalty, correct-prediction reinforcement) still
 * governs them at runtime.
 */
#define FCT_PRELOAD_CSV_HEADER "load1_pc,load2_pc,offset_delta,load2_mem_size"
#define FCT_PRELOAD_CSV_FIELDS 4
#define FCT_PRELOAD_CSV_LINE_SIZE 256

static void fct_preload_error(uint64_t line_num, const char* message) {
    fprintf(stderr, "FCT preload: invalid pairs file '%s' at line %llu: %s\n",
            IFUSE_FCT_PRELOAD_FILE, (unsigned long long)line_num, message);
    exit(1);
}

static bool fct_preload_parse_number(const char* text, uint64_t* value) {
    char* end;

    if (!text || !text[0]) {
        return false;
    }

    errno = 0;
    *value = strtoull(text, &end, 0);
    return errno == 0 && end != text && *end == '\0';
}

static bool fct_preload_parse_signed_number(const char* text, int64_t* value) {
    char* end;

    if (!text || !text[0]) {
        return false;
    }

    errno = 0;
    *value = strtoll(text, &end, 0);
    return errno == 0 && end != text && *end == '\0';
}

static void fct_preload_from_file(void) {
    FILE*    pairs_file;
    char     line[FCT_PRELOAD_CSV_LINE_SIZE];
    uint64_t line_num = 0;
    uint64_t loaded_count = 0;
    uint64_t duplicate_count = 0;

    if (!IFUSE_FCT_PRELOAD_FILE || !IFUSE_FCT_PRELOAD_FILE[0]) {
        return;
    }

    pairs_file = fopen(IFUSE_FCT_PRELOAD_FILE, "r");
    if (!pairs_file) {
        fprintf(stderr, "FCT preload: could not open pairs file '%s': %s\n",
                IFUSE_FCT_PRELOAD_FILE, strerror(errno));
        exit(1);
    }

    if (!fgets(line, sizeof(line), pairs_file)) {
        fct_preload_error(1, "missing CSV header");
    }
    line_num++;
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, FCT_PRELOAD_CSV_HEADER) != 0) {
        fct_preload_error(line_num, "unexpected CSV header");
    }

    while (fgets(line, sizeof(line), pairs_file)) {
        char*        fields[FCT_PRELOAD_CSV_FIELDS];
        char*        saveptr = NULL;
        char*        field;
        unsigned int field_count = 0;
        uint64_t     ld1_pc_value;
        uint64_t     ld2_pc_value;
        int64_t      signed_offset_delta;
        uint64_t     mem_size_value;
        Addr         ld1_pc_addr;
        Addr         ld2_pc_addr;
        unsigned int offset_delta;
        bool         direction;
        unsigned int ld2_mem_size;
        FCT_Row*     row;

        line_num++;
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) {
            continue;
        }

        for (field = strtok_r(line, ",", &saveptr); field;
             field = strtok_r(NULL, ",", &saveptr)) {
            if (field_count == FCT_PRELOAD_CSV_FIELDS) {
                fct_preload_error(line_num, "too many CSV fields");
            }
            fields[field_count++] = field;
        }
        if (field_count != FCT_PRELOAD_CSV_FIELDS) {
            fct_preload_error(line_num, "wrong number of CSV fields");
        }

        if (!fct_preload_parse_number(fields[0], &ld1_pc_value) ||
            !fct_preload_parse_number(fields[1], &ld2_pc_value) ||
            !fct_preload_parse_signed_number(fields[2],
                                             &signed_offset_delta) ||
            !fct_preload_parse_number(fields[3], &mem_size_value)) {
            fct_preload_error(line_num, "invalid numeric field");
        }

        ld1_pc_addr = (Addr)ld1_pc_value;
        ld2_pc_addr = (Addr)ld2_pc_value;

        // The candidates file stores LD2 - LD1 as a signed byte delta; the FCT
        // keeps the magnitude and a direction bit (true when LD2 >= LD1).
        direction    = signed_offset_delta >= 0;
        offset_delta = (unsigned int)(direction ? signed_offset_delta :
                                                  -signed_offset_delta);
        ld2_mem_size = (unsigned int)mem_size_value;

        if (ld1_pc_addr == 0 || ld2_pc_addr == 0) {
            fct_preload_error(line_num, "PC must not be zero");
        }
        if (offset_delta > 63U) {
            fct_preload_error(line_num, "offset_delta must be -63..63");
        }
        if (ld2_mem_size == 0 || mem_size_value > 64U) {
            fct_preload_error(line_num, "load2_mem_size must be 1-64");
        }

        // One LD2 per LD1: the first preloaded row for an LD1 PC wins.
        if (fct_lookup_row(ld1_pc_addr)) {
            duplicate_count++;
            STAT_EVENT(0, FCT_PRELOAD_DUPLICATE_LD1_SKIPS);
            continue;
        }

        row = fct_allocate_row_for_load1_pc(ld1_pc_addr);
        fct_write_row(row, ld1_pc_addr, ld2_pc_addr, /*ld1_effective_addr=*/0,
                      /*ld2_effective_addr=*/0, offset_delta, direction,
                      ld2_mem_size, /*ld1_micro_op_num=*/0,
                      /*ld2_micro_op_num=*/0, IFUSE_FCT_PRELOAD_CONF);
        fct_touch_row(ld1_pc_addr, row);
        loaded_count++;
        STAT_EVENT(0, FCT_PRELOAD_INSERTS);
    }

    if (ferror(pairs_file)) {
        fprintf(stderr, "FCT preload: could not read pairs file '%s': %s\n",
                IFUSE_FCT_PRELOAD_FILE, strerror(errno));
        exit(1);
    }
    fclose(pairs_file);

    printf("FCT preload: loaded %llu PC-pair(s) from '%s' at confidence %u "
           "(skipped %llu duplicate LD1 PCs)\n",
           (unsigned long long)loaded_count, IFUSE_FCT_PRELOAD_FILE,
           IFUSE_FCT_PRELOAD_CONF, (unsigned long long)duplicate_count);
    fflush(stdout);
}

FCT_Row* fct_lookup(Addr ld1_pc_addr) {
    if (!fct_is_initialized || ld1_pc_addr == 0) {
        return NULL;
    }
    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (row) {
        fct_touch_row(ld1_pc_addr, row);
    }
    return row;
}

void fct_update_confidence(Addr ld1_pc_addr, bool prediction_correct) {
    if (!fct_is_initialized) {
        return;
    }

    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (!row) {
        return;
    }

    if (prediction_correct) {
        fct_increment_confidence_score(row);
        return;
    }

    const unsigned int confidence_penalty =
        IFUSE_FCT_MISPRED_CONF_PENALTY;
    row->confidence_score = (row->confidence_score <= confidence_penalty) ?
        0 : row->confidence_score - confidence_penalty;
}

Flag fct_has_load1_pc_entry(Addr ld1_pc_addr) {
    if (!fct_is_initialized) {
        return FALSE;
    }
    return fct_lookup_row(ld1_pc_addr) ? TRUE : FALSE;
}

Flag fct_install_runtime_candidate(Addr ld1_pc_addr, Addr ld2_pc_addr,
                                   Addr ld1_effective_addr,
                                   Addr ld2_effective_addr,
                                   unsigned int offset_delta, bool direction,
                                   unsigned int ld2_mem_size,
                                   Counter ld1_micro_op_num,
                                   Counter ld2_micro_op_num,
                                   unsigned int proc_id) {
    if (!fct_is_initialized)
        fct_init();
    if (!ld1_pc_addr || !ld2_pc_addr || offset_delta > 63U ||
        !ld2_mem_size || ld2_mem_size > 64U)
        return FALSE;

    FCT_Row* row = fct_lookup_row(ld1_pc_addr);
    if (row) {
        bool same_candidate = row->ld2_pc_addr == ld2_pc_addr &&
                              row->offset_delta == offset_delta &&
                              row->direction == direction &&
                              row->ld2_mem_size == ld2_mem_size;
        if (same_candidate) {
            if (row->confidence_score < IFUSE_FCT_RUNTIME_INSERT_CONF)
                row->confidence_score = fct_saturating_confidence_score(
                    IFUSE_FCT_RUNTIME_INSERT_CONF);
            STAT_EVENT(proc_id, FCT_RUNTIME_REINFORCEMENTS);
            return TRUE;
        }

        /* A trusted row keeps its LD2; a suppressed row may be relearned. */
        if (row->confidence_score > IFUSE_FCT_CONF_THRESHOLD) {
            STAT_EVENT(proc_id, FCT_RUNTIME_CONFLICTS);
            return FALSE;
        }
        STAT_EVENT(proc_id, FCT_RUNTIME_REPLACEMENTS);
    } else {
        row = fct_allocate_row_for_load1_pc(ld1_pc_addr);
        STAT_EVENT(proc_id, FCT_RUNTIME_INSERTS);
    }

    fct_write_row(row, ld1_pc_addr, ld2_pc_addr, ld1_effective_addr,
                  ld2_effective_addr, offset_delta, direction, ld2_mem_size,
                  ld1_micro_op_num, ld2_micro_op_num,
                  IFUSE_FCT_RUNTIME_INSERT_CONF);
    fct_touch_row(ld1_pc_addr, row);
    return TRUE;
}
