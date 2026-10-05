#include "../globals/global_types.h"

#include "ideal_fusion.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../general.param.h"
#include "../globals/global_vars.h"
#include "../map.h"
#include "../memory/memory.param.h"
#include "../op.h"
#include "../statistics.h"

/*
 * Pass 1 keeps recently fetched loads grouped by data-cache block. When a
 * later load arrives, the matching step can inspect only earlier loads from
 * the same block instead of scanning the full dynamic instruction history.
 */
#define IDEAL_FUSION_LOAD_CANDIDATE_BUCKETS 4096
#define IDEAL_FUSION_PAIR_INDEX_BUCKETS 1000003
#define IDEAL_FUSION_CSV_FIELDS 11
/* With --ideal_fusion_stores the log gains an is_store column. */
#define IDEAL_FUSION_CSV_FIELDS_STORES 12
#define IDEAL_FUSION_CSV_LINE_SIZE 1024
#define IDEAL_FUSION_CSV_HEADER                                                   \
  "load1_pc,load1_data_addr,load1_block_offset,load1_mem_size,"                  \
  "load1_micro_op_num,load2_pc,load2_data_addr,load2_block_offset,"              \
  "load2_mem_size,load2_micro_op_num,micro_op_distance"
#define IDEAL_FUSION_CSV_HEADER_STORES IDEAL_FUSION_CSV_HEADER ",is_store"

typedef struct Ideal_Fusion_Load_Candidate_struct {
  Addr pc;
  Addr virtual_addr;
  Addr cache_block_addr;
  Addr cache_block_offset;
  uns mem_size;
  Counter micro_op_num;
  Flag fused;
  struct Ideal_Fusion_Load_Candidate_struct* next;
} Ideal_Fusion_Load_Candidate;

typedef struct Ideal_Fusion_Pair_struct {
  Counter load1_micro_op_num;
  Counter load2_micro_op_num;
  Addr load1_pc;
  Addr load2_pc;
  Addr load1_data_addr;
  Addr load2_data_addr;
  Addr load1_block_offset;
  Addr load2_block_offset;
  uns load1_mem_size;
  uns load2_mem_size;
  Flag is_store; /* both ops are stores (STORE1/STORE2) */
  struct Ideal_Fusion_Pair_struct* next;
} Ideal_Fusion_Pair;

Load2BufferNode* load2_buffer_ht[LOAD2_BUFFER_HT_SIZE] = {NULL};

/*
 * Candidate files identify dynamic ops using the order in which on-path ops
 * are fetched. Off-path ops do not advance this counter.
 */
static Counter next_on_path_micro_op_num = 0;

static Ideal_Fusion_Load_Candidate*
  load_candidates[IDEAL_FUSION_LOAD_CANDIDATE_BUCKETS] = {NULL};
static Counter last_load_cleanup_micro_op_num = 0;
static FILE* candidate_log = NULL;
static Counter logged_candidate_pair_count = 0;

/*
 * Store pairs follow Helios (Singh et al., MICRO'22, Sec. IV-B4): two stores
 * may fuse only if no other store lies between them, since fusing across a
 * store risks violating store-store ordering. So the only STORE1 candidate is
 * the most recent on-path store, like the paper's single-entry store UCH.
 */
static Ideal_Fusion_Load_Candidate last_store;
static Flag last_store_valid = FALSE;

/*
 * A fetched op knows only its own sequence number. Store each pair in two
 * indexes so pass 2 can quickly recognize both LOAD1 and LOAD2 without
 * scanning every pair.
 */
static Ideal_Fusion_Pair*
  load1_pair_index[IDEAL_FUSION_PAIR_INDEX_BUCKETS] = {NULL};
static Ideal_Fusion_Pair*
  load2_pair_index[IDEAL_FUSION_PAIR_INDEX_BUCKETS] = {NULL};
static Flag pair_indexes_loaded = FALSE;

static Addr get_cache_block_addr(Addr addr) {
  return (addr / DCACHE_LINE_SIZE) * DCACHE_LINE_SIZE;
}

static uns get_candidate_bucket(Addr cache_block_addr) {
  uns64 key = cache_block_addr;
  key ^= key >> 33;
  key *= 0xff51afd7ed558ccdULL;
  key ^= key >> 33;
  return key % IDEAL_FUSION_LOAD_CANDIDATE_BUCKETS;
}

static uns get_pair_index_bucket(Counter micro_op_num) {
  uns64 key = micro_op_num;
  key ^= key >> 33;
  key *= 0xff51afd7ed558ccdULL;
  key ^= key >> 33;
  return key % IDEAL_FUSION_PAIR_INDEX_BUCKETS;
}

/*
 * Adds a precomputed LOAD1/LOAD2 relationship to both indexes so either
 * dynamic load can find its partner when it is fetched during pass 2.
 * These helpers are used by the pass-2 CSV loader and classifier added next.
 */
static void index_fusion_pair(const Ideal_Fusion_Pair* pair) {
  Ideal_Fusion_Pair* load1_index_entry;
  Ideal_Fusion_Pair* load2_index_entry;
  uns load1_bucket = get_pair_index_bucket(pair->load1_micro_op_num);
  uns load2_bucket = get_pair_index_bucket(pair->load2_micro_op_num);

  load1_index_entry = (Ideal_Fusion_Pair*)malloc(sizeof(*load1_index_entry));
  load2_index_entry = (Ideal_Fusion_Pair*)malloc(sizeof(*load2_index_entry));
  if (!load1_index_entry || !load2_index_entry) {
    free(load1_index_entry);
    free(load2_index_entry);
    fprintf(stderr, "Ideal fusion: could not allocate pass-2 pair indexes.\n");
    exit(EXIT_FAILURE);
  }

  *load1_index_entry = *pair;
  load1_index_entry->next = load1_pair_index[load1_bucket];
  load1_pair_index[load1_bucket] = load1_index_entry;

  *load2_index_entry = *pair;
  load2_index_entry->next = load2_pair_index[load2_bucket];
  load2_pair_index[load2_bucket] = load2_index_entry;
}

/* Returns the fusion pair when this dynamic op is a precomputed LOAD1. */
static Ideal_Fusion_Pair* lookup_load1_pair(Counter micro_op_num) {
  Ideal_Fusion_Pair* pair;
  uns bucket = get_pair_index_bucket(micro_op_num);

  for (pair = load1_pair_index[bucket]; pair; pair = pair->next) {
    if (pair->load1_micro_op_num == micro_op_num)
      return pair;
  }

  return NULL;
}

/* Returns the fusion pair when this dynamic op is a precomputed LOAD2. */
static Ideal_Fusion_Pair* lookup_load2_pair(Counter micro_op_num) {
  Ideal_Fusion_Pair* pair;
  uns bucket = get_pair_index_bucket(micro_op_num);

  for (pair = load2_pair_index[bucket]; pair; pair = pair->next) {
    if (pair->load2_micro_op_num == micro_op_num)
      return pair;
  }

  return NULL;
}

/*
 * MEASUREMENT MODE (IDEAL_FUSION_PASS == 3): do not fuse anything. Load the
 * same pair set pass 2 would fuse, let both loads execute normally, and log the
 * real completion (wake/done) cycle of each LOAD1 and LOAD2 as it writes back.
 * Joining these rows by (load1,load2) offline shows whether the program-order
 * older LOAD1 actually finishes before the younger LOAD2 -- the assumption the
 * pass-2 forwarding relies on.
 */
static void load_pair_indexes(void);
static FILE* measure_log = NULL;

static void open_measure_log(void) {
  const char* path;

  if (measure_log)
    return;

  path = getenv("IDEAL_FUSION_MEASURE_OUT");
  if (!path || !path[0])
    path = "ideal_fusion_measure.csv";

  measure_log = fopen(path, "w");
  if (!measure_log) {
    fprintf(stderr, "Ideal fusion measure: could not open '%s': %s\n", path,
            strerror(errno));
    exit(EXIT_FAILURE);
  }
  fprintf(measure_log,
          "role,this_micro_op_num,load1_micro_op_num,load2_micro_op_num,"
          "done_cycle,wake_cycle,cur_cycle\n");
}

void ideal_fusion_measure_on_wake(Op* op) {
  Ideal_Fusion_Pair* pair;
  const char* role = NULL;

  if (!op || op->off_path || IDEAL_FUSION_PASS != 3)
    return;
  if (op->ideal_fusion_micro_op_num == 0 ||
      op->inst_info->table_info.mem_type != MEM_LD)
    return;

  load_pair_indexes();

  pair = lookup_load1_pair(op->ideal_fusion_micro_op_num);
  if (pair) {
    role = "LOAD1";
  } else {
    pair = lookup_load2_pair(op->ideal_fusion_micro_op_num);
    if (pair)
      role = "LOAD2";
  }
  if (!role)
    return;

  open_measure_log();
  fprintf(measure_log, "%s,%llu,%llu,%llu,%llu,%llu,%llu\n", role,
          op->ideal_fusion_micro_op_num, pair->load1_micro_op_num,
          pair->load2_micro_op_num, op->done_cycle, op->wake_cycle,
          cycle_count);
}

/*
 * Pass 2 only tags the fetched load in this step. Later pipeline changes will
 * use the role and partner number to model the fused LOAD1 and LOAD2 behavior.
 */
static void classify_op(Op* op) {
  Ideal_Fusion_Pair* pair;
  Mem_Type mem_type = op->inst_info->table_info.mem_type;
  Flag is_store = mem_type == MEM_ST;

  if ((mem_type != MEM_LD && !(is_store && IDEAL_FUSION_STORES)) ||
      op->oracle_info.va == 0 || op->oracle_info.mem_size == 0)
    return;

  pair = lookup_load1_pair(op->ideal_fusion_micro_op_num);
  if (pair && pair->is_store == is_store) {
    op->ideal_fusion_load_role = is_store ? IDEAL_FUSION_STORE1 : IDEAL_FUSION_LOAD1;
    op->ideal_fusion_partner_micro_op_num = pair->load2_micro_op_num;
    STAT_EVENT(op->proc_id, is_store ? IDEAL_FUSION_STORE1_TAGGED : IDEAL_FUSION_LOAD1_TAGGED);
    return;
  }

  pair = lookup_load2_pair(op->ideal_fusion_micro_op_num);
  if (pair && pair->is_store == is_store) {
    op->ideal_fusion_load_role = is_store ? IDEAL_FUSION_STORE2 : IDEAL_FUSION_LOAD2;
    op->ideal_fusion_partner_micro_op_num = pair->load1_micro_op_num;
    STAT_EVENT(op->proc_id, is_store ? IDEAL_FUSION_STORE2_TAGGED : IDEAL_FUSION_LOAD2_TAGGED);
  }
}

static Flag access_fits_in_cache_block(Addr addr, uns mem_size) {
  Addr cache_block_addr;

  if (mem_size == 0)
    return FALSE;

  cache_block_addr = get_cache_block_addr(addr);
  return addr + mem_size - 1 < cache_block_addr + DCACHE_LINE_SIZE;
}

static void pair_log_error(Counter line_num, const char* message) {
  fprintf(stderr, "Ideal fusion: invalid candidate log '%s' at line %llu: %s\n",
          IDEAL_FUSION_LOG, line_num, message);
  exit(EXIT_FAILURE);
}

static Flag parse_csv_number(const char* text, uns64* value) {
  char* end;

  if (!text || !text[0])
    return FALSE;

  errno = 0;
  *value = strtoull(text, &end, 0);
  return errno == 0 && end != text && *end == '\0';
}

/*
 * Candidate logs ending in ".gz" are written and read through gzip so a
 * full-length run's log stays a fraction of its plain-text size.
 */
static Flag log_is_gzip(void) {
  size_t len = strlen(IDEAL_FUSION_LOG);
  return len > 3 && strcmp(IDEAL_FUSION_LOG + len - 3, ".gz") == 0;
}

static FILE* open_log(const char* mode) {
  char cmd[IDEAL_FUSION_CSV_LINE_SIZE + 32];

  if (!log_is_gzip())
    return fopen(IDEAL_FUSION_LOG, mode);
  if (mode[0] == 'w')
    snprintf(cmd, sizeof(cmd), "gzip -1 > '%s'", IDEAL_FUSION_LOG);
  else
    snprintf(cmd, sizeof(cmd), "gzip -dc '%s'", IDEAL_FUSION_LOG);
  return popen(cmd, mode);
}

static int close_log(FILE* log) {
  return log_is_gzip() ? pclose(log) : fclose(log);
}

/*
 * Pass 2 reads the complete metadata recorded by pass 1. Keeping addresses,
 * offsets, and sizes alongside sequence numbers makes later LOAD1 modeling
 * explicit and keeps the candidate log useful for debugging.
 */
static FILE* pair_log = NULL;
static Counter pair_log_line_num = 0;
static Counter loaded_pair_count = 0;
static Counter skipped_distance_count = 0;
static Counter skipped_store_count = 0;
static uns pair_log_fields = IDEAL_FUSION_CSV_FIELDS;

static void open_pair_log(void) {
  char line[IDEAL_FUSION_CSV_LINE_SIZE];

  if (!IDEAL_FUSION_LOG || !IDEAL_FUSION_LOG[0]) {
    fprintf(stderr, "Ideal fusion: candidate log path must not be empty.\n");
    exit(EXIT_FAILURE);
  }

  pair_log = open_log("r");
  if (!pair_log) {
    fprintf(stderr, "Ideal fusion: could not open candidate log '%s': %s\n",
            IDEAL_FUSION_LOG, strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (!fgets(line, sizeof(line), pair_log))
    pair_log_error(1, "missing CSV header");
  pair_log_line_num++;
  line[strcspn(line, "\r\n")] = '\0';
  if (strcmp(line, IDEAL_FUSION_CSV_HEADER_STORES) == 0)
    pair_log_fields = IDEAL_FUSION_CSV_FIELDS_STORES;
  else if (strcmp(line, IDEAL_FUSION_CSV_HEADER) == 0)
    pair_log_fields = IDEAL_FUSION_CSV_FIELDS;
  else
    pair_log_error(pair_log_line_num, "unexpected CSV header");
}

static void close_pair_log(void) {
  if (ferror(pair_log)) {
    fprintf(stderr, "Ideal fusion: could not read candidate log '%s': %s\n",
            IDEAL_FUSION_LOG, strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (close_log(pair_log) != 0) {
    fprintf(stderr, "Ideal fusion: could not close candidate log '%s'\n",
            IDEAL_FUSION_LOG);
    exit(EXIT_FAILURE);
  }
  pair_log = NULL;

  printf("Ideal fusion pass %u: loaded %llu candidate pair(s) from '%s' "
         "(distance < %u, skipped %llu; store pairs skipped %llu)\n",
         IDEAL_FUSION_PASS, loaded_pair_count, IDEAL_FUSION_LOG,
         IDEAL_FUSION_DISTANCE, skipped_distance_count, skipped_store_count);
  fflush(stdout);
}

/*
 * Reads the next pair within the distance window into *pair. Returns FALSE at
 * the end of the log.
 */
static Flag read_next_pair(Ideal_Fusion_Pair* pair) {
  char line[IDEAL_FUSION_CSV_LINE_SIZE];

  while (fgets(line, sizeof(line), pair_log)) {
    char* fields[IDEAL_FUSION_CSV_FIELDS_STORES];
    char* saveptr = NULL;
    char* field;
    uns64 values[IDEAL_FUSION_CSV_FIELDS_STORES];
    uns field_count = 0;
    Counter recorded_distance;
    Counter line_num = ++pair_log_line_num;

    line[strcspn(line, "\r\n")] = '\0';
    if (!line[0])
      continue;

    for (field = strtok_r(line, ",", &saveptr); field;
         field = strtok_r(NULL, ",", &saveptr)) {
      if (field_count == pair_log_fields)
        pair_log_error(line_num, "too many CSV fields");
      fields[field_count++] = field;
    }

    if (field_count != pair_log_fields)
      pair_log_error(line_num, "wrong number of CSV fields");

    for (field_count = 0; field_count < pair_log_fields; field_count++) {
      if (!parse_csv_number(fields[field_count], &values[field_count]))
        pair_log_error(line_num, "invalid numeric field");
    }

    if (values[3] > UINT_MAX || values[8] > UINT_MAX)
      pair_log_error(line_num, "memory size is out of range");

    memset(pair, 0, sizeof(*pair));
    pair->load1_pc = values[0];
    pair->load1_data_addr = values[1];
    pair->load1_block_offset = values[2];
    pair->load1_mem_size = values[3];
    pair->load1_micro_op_num = values[4];
    pair->load2_pc = values[5];
    pair->load2_data_addr = values[6];
    pair->load2_block_offset = values[7];
    pair->load2_mem_size = values[8];
    pair->load2_micro_op_num = values[9];
    recorded_distance = values[10];
    pair->is_store = pair_log_fields == IDEAL_FUSION_CSV_FIELDS_STORES && values[11] != 0;

    if (pair->load1_micro_op_num == 0 ||
        pair->load2_micro_op_num <= pair->load1_micro_op_num ||
        pair->load2_micro_op_num - pair->load1_micro_op_num !=
          recorded_distance)
      pair_log_error(line_num, "invalid micro-op sequence numbers");

    if (get_cache_block_addr(pair->load1_data_addr) !=
          get_cache_block_addr(pair->load2_data_addr) ||
        pair->load1_block_offset !=
          pair->load1_data_addr - get_cache_block_addr(pair->load1_data_addr) ||
        pair->load2_block_offset !=
          pair->load2_data_addr - get_cache_block_addr(pair->load2_data_addr) ||
        !access_fits_in_cache_block(pair->load1_data_addr,
                                    pair->load1_mem_size) ||
        !access_fits_in_cache_block(pair->load2_data_addr,
                                    pair->load2_mem_size))
      pair_log_error(line_num, "inconsistent cache-block metadata");

    /* Apply fusion only for pairs within the distance window. */
    if (recorded_distance >= IDEAL_FUSION_DISTANCE) {
      skipped_distance_count++;
      continue;
    }

    /* Store pairs are fused only when --ideal_fusion_stores is on. */
    if (pair->is_store && !IDEAL_FUSION_STORES) {
      skipped_store_count++;
      continue;
    }

    loaded_pair_count++;
    return TRUE;
  }

  return FALSE;
}

/* Measurement mode looks pairs up at wake time, so it indexes the whole log. */
static void load_pair_indexes(void) {
  Ideal_Fusion_Pair pair;

  if (pair_indexes_loaded)
    return;

  open_pair_log();
  while (read_next_pair(&pair))
    index_fusion_pair(&pair);
  close_pair_log();
  pair_indexes_loaded = TRUE;
}

/*
 * Pass 2 looks pairs up only when an on-path load is fetched, and fetch
 * sequence numbers only grow. Pass 1 logs pairs in LOAD2 order, so pass 2
 * streams the log and keeps just the pairs a nearby op can still match:
 * those whose LOAD2 lies within IDEAL_FUSION_DISTANCE ahead of the current op
 * (any LOAD1 at op n has its LOAD2 before n + distance) and not yet behind it.
 * This bounds memory to roughly one window of pairs instead of the full log.
 */
typedef struct Ideal_Fusion_Window_Entry_struct {
  Counter load1_micro_op_num;
  Counter load2_micro_op_num;
  struct Ideal_Fusion_Window_Entry_struct* next;
} Ideal_Fusion_Window_Entry;

static Ideal_Fusion_Window_Entry* window_head = NULL;
static Ideal_Fusion_Window_Entry* window_tail = NULL;
static Ideal_Fusion_Pair next_pair;
static Flag next_pair_valid = FALSE;
static Flag pair_stream_done = FALSE;
static Counter last_load2_micro_op_num = 0;

static void unindex_pair(Ideal_Fusion_Pair** index, Counter key,
                         Counter load1_micro_op_num) {
  Ideal_Fusion_Pair** slot = &index[get_pair_index_bucket(key)];

  for (; *slot; slot = &(*slot)->next) {
    if ((*slot)->load1_micro_op_num == load1_micro_op_num) {
      Ideal_Fusion_Pair* stale = *slot;
      *slot = stale->next;
      free(stale);
      return;
    }
  }
}

static void advance_pair_window(Counter micro_op_num) {
  if (!pair_log && !pair_stream_done)
    open_pair_log();

  while (!pair_stream_done) {
    if (!next_pair_valid) {
      if (!read_next_pair(&next_pair)) {
        close_pair_log();
        pair_stream_done = TRUE;
        break;
      }
      if (next_pair.load2_micro_op_num < last_load2_micro_op_num)
        pair_log_error(pair_log_line_num, "pairs are not in LOAD2 order");
      last_load2_micro_op_num = next_pair.load2_micro_op_num;
      next_pair_valid = TRUE;
    }
    if (next_pair.load2_micro_op_num > micro_op_num + IDEAL_FUSION_DISTANCE)
      break;

    Ideal_Fusion_Window_Entry* entry = (Ideal_Fusion_Window_Entry*)malloc(sizeof(*entry));
    if (!entry) {
      fprintf(stderr, "Ideal fusion: could not allocate pass-2 window entry.\n");
      exit(EXIT_FAILURE);
    }
    entry->load1_micro_op_num = next_pair.load1_micro_op_num;
    entry->load2_micro_op_num = next_pair.load2_micro_op_num;
    entry->next = NULL;
    if (window_tail)
      window_tail->next = entry;
    else
      window_head = entry;
    window_tail = entry;
    index_fusion_pair(&next_pair);
    next_pair_valid = FALSE;
  }

  /* Drop pairs whose LOAD2 has already been fetched. */
  while (window_head && window_head->load2_micro_op_num < micro_op_num) {
    Ideal_Fusion_Window_Entry* stale = window_head;
    unindex_pair(load1_pair_index, stale->load1_micro_op_num, stale->load1_micro_op_num);
    unindex_pair(load2_pair_index, stale->load2_micro_op_num, stale->load1_micro_op_num);
    window_head = stale->next;
    if (!window_head)
      window_tail = NULL;
    free(stale);
  }
}

static void candidate_log_error(const char* action) {
  fprintf(stderr, "Ideal fusion: could not %s candidate output file '%s': %s\n",
          action, IDEAL_FUSION_LOG, strerror(errno));
  exit(EXIT_FAILURE);
}

static void close_candidate_log(void) {
  if (!candidate_log)
    return;

  if (fflush(candidate_log) != 0)
    fprintf(stderr, "Ideal fusion: could not flush candidate output file '%s': %s\n",
            IDEAL_FUSION_LOG, strerror(errno));
  if (close_log(candidate_log) != 0)
    fprintf(stderr, "Ideal fusion: could not close candidate output file '%s'\n",
            IDEAL_FUSION_LOG);
  candidate_log = NULL;
}

static void open_candidate_log(void) {
  if (candidate_log)
    return;

  if (!IDEAL_FUSION_LOG || !IDEAL_FUSION_LOG[0]) {
    fprintf(stderr, "Ideal fusion: candidate output path must not be empty.\n");
    exit(EXIT_FAILURE);
  }

  candidate_log = open_log("w");
  if (!candidate_log)
    candidate_log_error("open");

  if (fprintf(candidate_log, "%s\n",
              IDEAL_FUSION_STORES ? IDEAL_FUSION_CSV_HEADER_STORES : IDEAL_FUSION_CSV_HEADER) < 0 ||
      fflush(candidate_log) != 0)
    candidate_log_error("initialize");
  atexit(close_candidate_log);
}

static void log_matched_pair(Ideal_Fusion_Load_Candidate* load1, Op* load2, Flag is_store) {
  Counter distance = load2->ideal_fusion_micro_op_num - load1->micro_op_num;
  Addr load2_block_addr = get_cache_block_addr(load2->oracle_info.va);

  open_candidate_log();
  if (fprintf(candidate_log,
              "0x%llx,0x%llx,%llu,%u,%llu,0x%llx,0x%llx,%llu,%u,%llu,%llu",
              load1->pc, load1->virtual_addr, load1->cache_block_offset,
              load1->mem_size, load1->micro_op_num, load2->inst_info->addr,
              load2->oracle_info.va, load2->oracle_info.va - load2_block_addr,
              load2->oracle_info.mem_size, load2->ideal_fusion_micro_op_num,
              distance) < 0 ||
      (IDEAL_FUSION_STORES && fprintf(candidate_log, ",%d", is_store ? 1 : 0) < 0) ||
      fputc('\n', candidate_log) == EOF)
    candidate_log_error("write to");

  if (++logged_candidate_pair_count % 1000 == 0 &&
      fflush(candidate_log) != 0)
    candidate_log_error("flush");
}

static Flag load1_matches_load2(const Ideal_Fusion_Load_Candidate* load1,
                                Op* load2, Addr cache_block_addr) {
  return load1->cache_block_addr == cache_block_addr &&
         !load1->fused &&
         access_fits_in_cache_block(load1->virtual_addr, load1->mem_size) &&
         access_fits_in_cache_block(load2->oracle_info.va,
                                    load2->oracle_info.mem_size) &&
         load2->ideal_fusion_micro_op_num - load1->micro_op_num <
           IDEAL_FUSION_DISTANCE;
}

static Ideal_Fusion_Load_Candidate* find_matching_load1(Op* load2) {
  Ideal_Fusion_Load_Candidate* load1;
  Ideal_Fusion_Load_Candidate* best_match = NULL;
  Addr cache_block_addr;
  uns bucket;
  uns match_count = 0;

  if (load2->inst_info->table_info.mem_type != MEM_LD ||
      load2->inst_info->table_info.num_dest_regs == 0 ||
      load2->oracle_info.va == 0 || load2->oracle_info.mem_size == 0)
    return NULL;

  cache_block_addr = get_cache_block_addr(load2->oracle_info.va);
  bucket = get_candidate_bucket(cache_block_addr);

  for (load1 = load_candidates[bucket]; load1; load1 = load1->next) {
    if (!load1_matches_load2(load1, load2, cache_block_addr))
      continue;

    match_count++;
    /* DEBUG: list every qualifying LOAD1 the policy gets to choose between. */
    // printf("[IDEAL_FUSION] candidate load1 micro_op=%llu (block=0x%llx) for "
    //        "load2 micro_op=%llu\n",
    //        load1->micro_op_num, (uns64)cache_block_addr,
    //        load2->ideal_fusion_micro_op_num);

    if (!best_match) {
      best_match = load1;
      continue;
    }

    if (IDEAL_FUSION_TYPE == IDEAL_FUSION_MOST_RECENT) {
      if (load1->micro_op_num > best_match->micro_op_num)
        best_match = load1;
    } else if (load1->micro_op_num < best_match->micro_op_num) {
      best_match = load1;
    }
  }

  /* DEBUG: only interesting when the policy actually had a choice to make. */
  if (match_count > 1 && best_match) {
    // printf("[IDEAL_FUSION] policy=%s chose load1 micro_op=%llu out of %u "
    //        "candidates for load2 micro_op=%llu\n",
    //        IDEAL_FUSION_TYPE == IDEAL_FUSION_MOST_RECENT ? "most-recent"
    //                                                      : "oldest-first",
    //        best_match->micro_op_num, match_count,
    //        load2->ideal_fusion_micro_op_num);
    // fflush(stdout);
  }

  return best_match;
}

static void track_load(Op* op) {
  Ideal_Fusion_Load_Candidate* candidate;
  uns bucket;

  if (op->inst_info->table_info.mem_type != MEM_LD ||
      op->inst_info->table_info.num_dest_regs == 0 ||
      op->oracle_info.va == 0 || op->oracle_info.mem_size == 0)
    return;

  candidate = (Ideal_Fusion_Load_Candidate*)malloc(sizeof(*candidate));
  if (!candidate)
    return;

  candidate->pc = op->inst_info->addr;
  candidate->virtual_addr = op->oracle_info.va;
  candidate->cache_block_addr = get_cache_block_addr(op->oracle_info.va);
  candidate->cache_block_offset =
    op->oracle_info.va - candidate->cache_block_addr;
  candidate->mem_size = op->oracle_info.mem_size;
  candidate->micro_op_num = op->ideal_fusion_micro_op_num;
  candidate->fused = FALSE;

  bucket = get_candidate_bucket(candidate->cache_block_addr);
  candidate->next = load_candidates[bucket];
  load_candidates[bucket] = candidate;
}

/*
 * A store between LOAD1 and LOAD2 to the same cache block may change the data that LOAD2 observes.
 * Conservatively discard every earlier LOAD1 candidate from the store's cache
 * block, matching the original ideal-fusion implementation.
 */
static void invalidate_loads_for_store(Op* store) {
  Ideal_Fusion_Load_Candidate** candidate;
  Addr cache_block_addr;
  uns bucket;

  if (store->inst_info->table_info.mem_type != MEM_ST ||
      store->oracle_info.va == 0)
    return;

  cache_block_addr = get_cache_block_addr(store->oracle_info.va);
  bucket = get_candidate_bucket(cache_block_addr);
  candidate = &load_candidates[bucket];

  while (*candidate) {
    if ((*candidate)->cache_block_addr == cache_block_addr) {
      Ideal_Fusion_Load_Candidate* invalidated_candidate = *candidate;
      *candidate = invalidated_candidate->next;
      free(invalidated_candidate);
    } else {
      candidate = &(*candidate)->next;
    }
  }
}

static void fill_candidate(Ideal_Fusion_Load_Candidate* candidate, Op* op) {
  candidate->pc = op->inst_info->addr;
  candidate->virtual_addr = op->oracle_info.va;
  candidate->cache_block_addr = get_cache_block_addr(op->oracle_info.va);
  candidate->cache_block_offset = op->oracle_info.va - candidate->cache_block_addr;
  candidate->mem_size = op->oracle_info.mem_size;
  candidate->micro_op_num = op->ideal_fusion_micro_op_num;
  candidate->fused = FALSE;
  candidate->next = NULL;
}

/*
 * Pairs a store with the immediately preceding on-path store when both access
 * the same cache block within the fusion window. Any store, fused or not,
 * replaces the candidate, so no pair spans an intermediate store, and a fused
 * STORE2 never becomes a STORE1.
 */
static void match_store(Op* store) {
  if (store->oracle_info.va == 0 || store->oracle_info.mem_size == 0) {
    last_store_valid = FALSE;
    return;
  }

  if (last_store_valid &&
      last_store.cache_block_addr == get_cache_block_addr(store->oracle_info.va) &&
      access_fits_in_cache_block(last_store.virtual_addr, last_store.mem_size) &&
      access_fits_in_cache_block(store->oracle_info.va, store->oracle_info.mem_size) &&
      store->ideal_fusion_micro_op_num - last_store.micro_op_num < IDEAL_FUSION_DISTANCE) {
    log_matched_pair(&last_store, store, TRUE);
    last_store_valid = FALSE;
    return;
  }

  fill_candidate(&last_store, store);
  last_store_valid = TRUE;
}

static void cleanup_stale_loads(Counter current_micro_op_num) {
  uns bucket;

  for (bucket = 0; bucket < IDEAL_FUSION_LOAD_CANDIDATE_BUCKETS; bucket++) {
    Ideal_Fusion_Load_Candidate** candidate = &load_candidates[bucket];

    while (*candidate) {
      if ((*candidate)->fused ||
          current_micro_op_num - (*candidate)->micro_op_num >=
            IDEAL_FUSION_DISTANCE) {
        Ideal_Fusion_Load_Candidate* stale_candidate = *candidate;
        *candidate = stale_candidate->next;
        free(stale_candidate);
      } else {
        candidate = &(*candidate)->next;
      }
    }
  }
}

void ideal_fusion_on_fetch_op(Op* op) {
  if (!op || op->off_path)
    return;

  op->ideal_fusion_micro_op_num = ++next_on_path_micro_op_num;

  if (IDEAL_FUSION_PASS == 2) {
    advance_pair_window(op->ideal_fusion_micro_op_num);
    classify_op(op);
    return;
  }

  if (IDEAL_FUSION_PASS != 1 || IDEAL_FUSION_DISTANCE == 0)
    return;

  if (op->ideal_fusion_micro_op_num - last_load_cleanup_micro_op_num >=
      IDEAL_FUSION_DISTANCE) {
    cleanup_stale_loads(op->ideal_fusion_micro_op_num);
    last_load_cleanup_micro_op_num = op->ideal_fusion_micro_op_num;
  }

  if (op->inst_info->table_info.mem_type == MEM_ST) {
    invalidate_loads_for_store(op);
    if (IDEAL_FUSION_STORES)
      match_store(op);
    return;
  }

  Ideal_Fusion_Load_Candidate* load1 = find_matching_load1(op);

  if (load1) {
    log_matched_pair(load1, op, FALSE);
    load1->fused = TRUE;
  } else {
    track_load(op);
  }
}

Flag ideal_fusion_tail_is_nop(const Op* op) {
  return op && (op->ideal_fusion_load_role == IDEAL_FUSION_LOAD2 ||
                op->ideal_fusion_load_role == IDEAL_FUSION_STORE2);
}

Load2BufferNode* ideal_fusion_find_load2_buffer(Counter load1_micro_op_num) {
  uns bucket = get_pair_index_bucket(load1_micro_op_num);
  Load2BufferNode* node;

  for (node = load2_buffer_ht[bucket]; node; node = node->next) {
    if (node->entry.load1_micro_op_num == load1_micro_op_num)
      return node;
  }
  return NULL;
}

Load2BufferNode* ideal_fusion_create_load2_buffer(Counter load1_micro_op_num) {
  Load2BufferNode* node = (Load2BufferNode*)calloc(1, sizeof(*node));
  uns bucket;

  if (!node) {
    fprintf(stderr, "Ideal fusion: could not allocate Load2 buffer entry.\n");
    exit(EXIT_FAILURE);
  }

  node->entry.load1_micro_op_num = load1_micro_op_num;
  bucket = get_pair_index_bucket(load1_micro_op_num);
  node->next = load2_buffer_ht[bucket];
  load2_buffer_ht[bucket] = node;
  return node;
}

void ideal_fusion_remove_load2_buffer(Load2BufferNode* node) {
  Load2BufferNode** slot;
  uns bucket;

  if (!node)
    return;

  bucket = get_pair_index_bucket(node->entry.load1_micro_op_num);
  for (slot = &load2_buffer_ht[bucket]; *slot; slot = &(*slot)->next) {
    if (*slot == node) {
      *slot = node->next;
      free(node);
      return;
    }
  }
}

/*
 * A LOAD2's register dependents get the value when LOAD1 completes. A STORE2
 * writes through the fused STORE1, so loads that depend on it in memory wake
 * when STORE1 executes.
 */
static void ideal_fusion_complete_load2(Op* load2, Counter load1_wake_cycle,
                                        Counter load1_done_cycle,
                                        void (*wake_action)(Op*, Op*, uns)) {
  load2->wake_cycle = load1_wake_cycle;
  load2->done_cycle = load1_done_cycle;
  if (load2->ideal_fusion_load_role == IDEAL_FUSION_STORE2) {
    wake_up_ops(load2, MEM_ADDR_DEP, wake_action);
    wake_up_ops(load2, MEM_DATA_DEP, wake_action);
  } else {
    wake_up_ops(load2, REG_DATA_DEP, wake_action);
  }
}

void ideal_fusion_on_map(Op* op, void (*wake_action)(Op*, Op*, uns)) {
  Load2BufferNode* node;

  if (!op || op->off_path || IDEAL_FUSION_PASS != 2)
    return;

  if (op->ideal_fusion_load_role == IDEAL_FUSION_LOAD1 ||
      op->ideal_fusion_load_role == IDEAL_FUSION_STORE1) {
    node = ideal_fusion_find_load2_buffer(op->ideal_fusion_micro_op_num);
    if (!node)
      node = ideal_fusion_create_load2_buffer(op->ideal_fusion_micro_op_num);
    return;
  }

  if (!ideal_fusion_tail_is_nop(op))
    return;

  node = ideal_fusion_find_load2_buffer(op->ideal_fusion_partner_micro_op_num);
  if (!node)
    node = ideal_fusion_create_load2_buffer(op->ideal_fusion_partner_micro_op_num);

  node->entry.load2 = op;
  node->entry.load2_unique_num = op->unique_num;
  node->entry.load2_micro_op_num = op->ideal_fusion_micro_op_num;

  if (node->entry.load1_completed && !node->entry.pair_completed) {
    ideal_fusion_complete_load2(op, node->entry.load1_wake_cycle,
                                node->entry.load1_done_cycle, wake_action);
    node->entry.pair_completed = TRUE;
    ideal_fusion_remove_load2_buffer(node);
  } else {
    node->entry.load2_waiting = TRUE;
    node->entry.pair_completed = FALSE;
  }
}

void ideal_fusion_on_head_wake(Op* load1, Dep_Type type, void (*wake_action)(Op*, Op*, uns)) {
  Load2BufferNode* node;
  Op* load2;

  if (!load1 || load1->off_path || IDEAL_FUSION_PASS != 2)
    return;
  /* LOAD1 completes on its register wake, STORE1 when it executes. */
  if (!(load1->ideal_fusion_load_role == IDEAL_FUSION_LOAD1 && type == REG_DATA_DEP) &&
      !(load1->ideal_fusion_load_role == IDEAL_FUSION_STORE1 && type == MEM_DATA_DEP))
    return;

  node = ideal_fusion_find_load2_buffer(load1->ideal_fusion_micro_op_num);
  if (!node)
    node = ideal_fusion_create_load2_buffer(load1->ideal_fusion_micro_op_num);

  node->entry.load1_completed = TRUE;
  node->entry.load1_wake_cycle = load1->wake_cycle;
  node->entry.load1_done_cycle = load1->done_cycle;

  if (!node->entry.load2 || !node->entry.load2_waiting ||
      node->entry.pair_completed)
    return;

  load2 = node->entry.load2;
  if (!load2->op_pool_valid ||
      load2->unique_num != node->entry.load2_unique_num ||
      !ideal_fusion_tail_is_nop(load2)) {
    ideal_fusion_remove_load2_buffer(node);
    return;
  }

  ideal_fusion_complete_load2(load2, node->entry.load1_wake_cycle,
                              node->entry.load1_done_cycle, wake_action);
  node->entry.pair_completed = TRUE;
  ideal_fusion_remove_load2_buffer(node);
}
