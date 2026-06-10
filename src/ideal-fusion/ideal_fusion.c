#include "../globals/global_types.h"

#include "ideal_fusion.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../general.param.h"
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
#define IDEAL_FUSION_GROUP_INDEX_BUCKETS 1000003
#define IDEAL_FUSION_PAIR_CSV_FIELDS 11
#define IDEAL_FUSION_GROUP_LOAD_FIELDS 5
#define IDEAL_FUSION_CSV_LINE_SIZE 65536
#define IDEAL_FUSION_PAIR_CSV_HEADER                                              \
  "load1_pc,load1_data_addr,load1_block_offset,load1_mem_size,"                  \
  "load1_micro_op_num,load2_pc,load2_data_addr,load2_block_offset,"              \
  "load2_mem_size,load2_micro_op_num,micro_op_distance"

typedef struct Ideal_Fusion_Load_Info_struct {
  Addr pc;
  Addr virtual_addr;
  Addr cache_block_addr;
  Addr cache_block_offset;
  uns mem_size;
  Counter micro_op_num;
} Ideal_Fusion_Load_Info;

typedef struct Ideal_Fusion_Load_Candidate_struct {
  Ideal_Fusion_Load_Info info;
  Flag fused;
  struct Ideal_Fusion_Load_Candidate_struct* next;
} Ideal_Fusion_Load_Candidate;

typedef struct Ideal_Fusion_Group_struct {
  uns group_size;
  Ideal_Fusion_Load_Info* loads;
} Ideal_Fusion_Group;

typedef struct Ideal_Fusion_Group_Index_struct {
  Ideal_Fusion_Group* group;
  uns member_index;
  struct Ideal_Fusion_Group_Index_struct* next;
} Ideal_Fusion_Group_Index;

typedef struct Ideal_Fusion_Load2_Waiter_struct {
  Op* load2;
  Counter load2_unique_num;
  struct Ideal_Fusion_Load2_Waiter_struct* next;
} Ideal_Fusion_Load2_Waiter;

typedef struct Ideal_Fusion_Readiness_struct {
  Counter load1_micro_op_num;
  uns expected_load2_count;
  uns handled_load2_count;
  Flag load1_completed;
  Ideal_Fusion_Load2_Waiter* load2_waiters;
  struct Ideal_Fusion_Readiness_struct* next;
} Ideal_Fusion_Readiness;

/*
 * Candidate files identify dynamic ops using the order in which on-path ops
 * are fetched. Off-path ops do not advance this counter.
 */
static Counter next_on_path_micro_op_num = 0;

static Ideal_Fusion_Load_Candidate*
  load_candidates[IDEAL_FUSION_LOAD_CANDIDATE_BUCKETS] = {NULL};
static Counter last_load_cleanup_micro_op_num = 0;
static FILE* candidate_log = NULL;
static Counter logged_candidate_group_count = 0;

/*
 * A fetched op knows only its own sequence number. Store each group member in
 * an index so pass 2 can quickly recognize LOAD1 and every fused LOAD2 without
 * scanning every group.
 */
static Ideal_Fusion_Group_Index*
  group_member_index[IDEAL_FUSION_GROUP_INDEX_BUCKETS] = {NULL};
static Flag group_indexes_loaded = FALSE;
static Ideal_Fusion_Readiness*
  readiness_index[IDEAL_FUSION_GROUP_INDEX_BUCKETS] = {NULL};

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

static uns get_group_index_bucket(Counter micro_op_num) {
  uns64 key = micro_op_num;
  key ^= key >> 33;
  key *= 0xff51afd7ed558ccdULL;
  key ^= key >> 33;
  return key % IDEAL_FUSION_GROUP_INDEX_BUCKETS;
}

static uns ideal_fusion_group_size(void) {
  return IDEAL_FUSION_GROUP_SIZE < 2 ? 2 : IDEAL_FUSION_GROUP_SIZE;
}

static uns ideal_fusion_group_csv_fields(uns group_size) {
  return 1 + group_size * IDEAL_FUSION_GROUP_LOAD_FIELDS + 1;
}

static void fill_load_info_from_op(Ideal_Fusion_Load_Info* info, const Op* op) {
  info->pc = op->inst_info->addr;
  info->virtual_addr = op->oracle_info.va;
  info->cache_block_addr = get_cache_block_addr(op->oracle_info.va);
  info->cache_block_offset = op->oracle_info.va - info->cache_block_addr;
  info->mem_size = op->oracle_info.mem_size;
  info->micro_op_num = op->ideal_fusion_micro_op_num;
}

static Ideal_Fusion_Group* allocate_fusion_group(
  const Ideal_Fusion_Load_Info* loads, uns group_size) {
  Ideal_Fusion_Group* group =
    (Ideal_Fusion_Group*)malloc(sizeof(*group));

  if (!group) {
    fprintf(stderr, "Ideal fusion: could not allocate fusion group.\n");
    exit(EXIT_FAILURE);
  }

  group->loads =
    (Ideal_Fusion_Load_Info*)malloc(sizeof(*group->loads) * group_size);
  if (!group->loads) {
    free(group);
    fprintf(stderr, "Ideal fusion: could not allocate fusion group loads.\n");
    exit(EXIT_FAILURE);
  }

  group->group_size = group_size;
  memcpy(group->loads, loads, sizeof(*group->loads) * group_size);
  return group;
}

static void index_fusion_group(const Ideal_Fusion_Load_Info* loads,
                               uns group_size) {
  Ideal_Fusion_Group* group = allocate_fusion_group(loads, group_size);
  uns ii;

  for (ii = 0; ii < group_size; ii++) {
    Ideal_Fusion_Group_Index* index_entry =
      (Ideal_Fusion_Group_Index*)malloc(sizeof(*index_entry));
    uns bucket;

    if (!index_entry) {
      fprintf(stderr, "Ideal fusion: could not allocate pass-2 group index.\n");
      exit(EXIT_FAILURE);
    }

    bucket = get_group_index_bucket(loads[ii].micro_op_num);
    index_entry->group = group;
    index_entry->member_index = ii;
    index_entry->next = group_member_index[bucket];
    group_member_index[bucket] = index_entry;
  }
}

static Ideal_Fusion_Group_Index* lookup_group_member(Counter micro_op_num) {
  Ideal_Fusion_Group_Index* index_entry;
  uns bucket = get_group_index_bucket(micro_op_num);

  for (index_entry = group_member_index[bucket]; index_entry;
       index_entry = index_entry->next) {
    if (index_entry->group->loads[index_entry->member_index].micro_op_num ==
        micro_op_num)
      return index_entry;
  }

  return NULL;
}

static uns get_expected_load2_count(Counter load1_micro_op_num) {
  Ideal_Fusion_Group_Index* index_entry =
    lookup_group_member(load1_micro_op_num);

  if (index_entry && index_entry->member_index == 0 &&
      index_entry->group->group_size > 1)
    return index_entry->group->group_size - 1;

  return 1;
}

static Ideal_Fusion_Readiness* lookup_readiness(Counter load1_micro_op_num) {
  Ideal_Fusion_Readiness* readiness;
  uns bucket = get_group_index_bucket(load1_micro_op_num);

  for (readiness = readiness_index[bucket]; readiness;
       readiness = readiness->next) {
    if (readiness->load1_micro_op_num == load1_micro_op_num)
      return readiness;
  }

  return NULL;
}

static Ideal_Fusion_Readiness* get_or_create_readiness(
  Counter load1_micro_op_num, uns expected_load2_count) {
  Ideal_Fusion_Readiness* readiness = lookup_readiness(load1_micro_op_num);
  uns bucket;

  if (readiness) {
    if (expected_load2_count > readiness->expected_load2_count)
      readiness->expected_load2_count = expected_load2_count;
    return readiness;
  }

  readiness = (Ideal_Fusion_Readiness*)calloc(1, sizeof(*readiness));
  if (!readiness) {
    fprintf(stderr, "Ideal fusion: could not allocate LOAD2 readiness entry.\n");
    exit(EXIT_FAILURE);
  }

  readiness->load1_micro_op_num = load1_micro_op_num;
  readiness->expected_load2_count = expected_load2_count;
  bucket = get_group_index_bucket(load1_micro_op_num);
  readiness->next = readiness_index[bucket];
  readiness_index[bucket] = readiness;
  return readiness;
}

static void remove_readiness(Ideal_Fusion_Readiness* readiness) {
  Ideal_Fusion_Readiness** entry;
  Ideal_Fusion_Load2_Waiter* waiter = readiness->load2_waiters;
  uns bucket = get_group_index_bucket(readiness->load1_micro_op_num);

  while (waiter) {
    Ideal_Fusion_Load2_Waiter* next = waiter->next;
    free(waiter);
    waiter = next;
  }

  for (entry = &readiness_index[bucket]; *entry; entry = &(*entry)->next) {
    if (*entry == readiness) {
      *entry = readiness->next;
      free(readiness);
      return;
    }
  }
}

/*
 * Pass 2 only tags the fetched load in this step. Later pipeline changes will
 * use the role and partner number to model the fused LOAD1 and LOAD2 behavior.
 */
static void classify_load(Op* op) {
  Ideal_Fusion_Group_Index* index_entry;
  Ideal_Fusion_Group* group;

  if (op->inst_info->table_info.mem_type != MEM_LD ||
      op->oracle_info.va == 0 || op->oracle_info.mem_size == 0)
    return;

  index_entry = lookup_group_member(op->ideal_fusion_micro_op_num);
  if (!index_entry)
    return;

  group = index_entry->group;
  if (index_entry->member_index == 0) {
    op->ideal_fusion_load_role = IDEAL_FUSION_LOAD1;
    op->ideal_fusion_partner_micro_op_num =
      group->group_size > 1 ? group->loads[1].micro_op_num : 0;
    STAT_EVENT(op->proc_id, IDEAL_FUSION_LOAD1_TAGGED);
    return;
  }

  op->ideal_fusion_load_role = IDEAL_FUSION_LOAD2;
  op->ideal_fusion_partner_micro_op_num = group->loads[0].micro_op_num;
  STAT_EVENT(op->proc_id, IDEAL_FUSION_LOAD2_TAGGED);
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

static void build_group_csv_header(char* buffer, size_t buffer_size,
                                   uns group_size) {
  size_t used;
  uns ii;
  int written;

  written = snprintf(buffer, buffer_size, "group_size");
  if (written < 0 || (size_t)written >= buffer_size) {
    fprintf(stderr, "Ideal fusion: group CSV header buffer is too small.\n");
    exit(EXIT_FAILURE);
  }
  used = (size_t)written;

  for (ii = 0; ii < group_size; ii++) {
    written = snprintf(buffer + used, buffer_size - used,
                       ",load%u_pc,load%u_data_addr,load%u_block_offset,"
                       "load%u_mem_size,load%u_micro_op_num",
                       ii + 1, ii + 1, ii + 1, ii + 1, ii + 1);
    if (written < 0 || (size_t)written >= buffer_size - used) {
      fprintf(stderr, "Ideal fusion: group CSV header buffer is too small.\n");
      exit(EXIT_FAILURE);
    }
    used += (size_t)written;
  }

  written = snprintf(buffer + used, buffer_size - used, ",micro_op_span");
  if (written < 0 || (size_t)written >= buffer_size - used) {
    fprintf(stderr, "Ideal fusion: group CSV header buffer is too small.\n");
    exit(EXIT_FAILURE);
  }
}

static void validate_group_metadata(const Ideal_Fusion_Load_Info* loads,
                                    uns group_size, Counter recorded_span,
                                    FILE* group_log, Counter line_num) {
  Addr cache_block_addr = loads[0].cache_block_addr;
  uns ii;

  if (loads[0].micro_op_num == 0) {
    fclose(group_log);
    pair_log_error(line_num, "invalid first micro-op sequence number");
  }

  for (ii = 0; ii < group_size; ii++) {
    if (loads[ii].mem_size > UINT_MAX) {
      fclose(group_log);
      pair_log_error(line_num, "memory size is out of range");
    }

    if (loads[ii].cache_block_addr != cache_block_addr ||
        loads[ii].cache_block_addr !=
          get_cache_block_addr(loads[ii].virtual_addr) ||
        loads[ii].cache_block_offset !=
          loads[ii].virtual_addr - loads[ii].cache_block_addr ||
        !access_fits_in_cache_block(loads[ii].virtual_addr,
                                    loads[ii].mem_size)) {
      fclose(group_log);
      pair_log_error(line_num, "inconsistent cache-block metadata");
    }

    if (ii > 0 &&
        loads[ii].micro_op_num <= loads[ii - 1].micro_op_num) {
      fclose(group_log);
      pair_log_error(line_num, "group micro-op numbers are not increasing");
    }
  }

  if (loads[group_size - 1].micro_op_num - loads[0].micro_op_num !=
      recorded_span) {
    fclose(group_log);
    pair_log_error(line_num, "invalid micro-op span");
  }
}

/*
 * Pass 2 reads the complete metadata recorded by pass 1. Keeping addresses,
 * offsets, and sizes alongside sequence numbers makes LOAD1 modeling explicit
 * and keeps the candidate log useful for debugging.
 */
static void load_group_indexes(void) {
  FILE* group_log;
  char line[IDEAL_FUSION_CSV_LINE_SIZE];
  Counter line_num = 0;
  uns group_size = ideal_fusion_group_size();
  uns expected_fields = group_size == 2 ? IDEAL_FUSION_PAIR_CSV_FIELDS :
    ideal_fusion_group_csv_fields(group_size);

  if (group_indexes_loaded)
    return;

  if (!IDEAL_FUSION_LOG || !IDEAL_FUSION_LOG[0]) {
    fprintf(stderr, "Ideal fusion: candidate log path must not be empty.\n");
    exit(EXIT_FAILURE);
  }

  group_log = fopen(IDEAL_FUSION_LOG, "r");
  if (!group_log) {
    fprintf(stderr, "Ideal fusion: could not open candidate log '%s': %s\n",
            IDEAL_FUSION_LOG, strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (!fgets(line, sizeof(line), group_log)) {
    fclose(group_log);
    pair_log_error(1, "missing CSV header");
  }
  line_num++;
  line[strcspn(line, "\r\n")] = '\0';
  if (group_size == 2) {
    if (strcmp(line, IDEAL_FUSION_PAIR_CSV_HEADER) != 0) {
      fclose(group_log);
      pair_log_error(line_num, "unexpected CSV header");
    }
  } else {
    char expected_header[IDEAL_FUSION_CSV_LINE_SIZE];
    build_group_csv_header(expected_header, sizeof(expected_header),
                           group_size);
    if (strcmp(line, expected_header) != 0) {
      fclose(group_log);
      pair_log_error(line_num, "unexpected CSV header");
    }
  }

  while (fgets(line, sizeof(line), group_log)) {
    Ideal_Fusion_Load_Info* loads;
    uns64* values;
    char* saveptr = NULL;
    char* field;
    uns field_count = 0;
    Counter recorded_span;
    uns ii;

    line_num++;
    line[strcspn(line, "\r\n")] = '\0';
    if (!line[0])
      continue;

    values = (uns64*)calloc(expected_fields, sizeof(*values));
    loads = (Ideal_Fusion_Load_Info*)calloc(group_size, sizeof(*loads));
    if (!values || !loads) {
      free(values);
      free(loads);
      fclose(group_log);
      fprintf(stderr, "Ideal fusion: could not allocate CSV parse buffers.\n");
      exit(EXIT_FAILURE);
    }

    for (field = strtok_r(line, ",", &saveptr); field;
         field = strtok_r(NULL, ",", &saveptr)) {
      if (field_count == expected_fields) {
        free(values);
        free(loads);
        fclose(group_log);
        pair_log_error(line_num, "too many CSV fields");
      }
      if (!parse_csv_number(field, &values[field_count])) {
        free(values);
        free(loads);
        fclose(group_log);
        pair_log_error(line_num, "invalid numeric field");
      }
      field_count++;
    }

    if (field_count != expected_fields) {
      free(values);
      free(loads);
      fclose(group_log);
      pair_log_error(line_num, "wrong number of CSV fields");
    }

    if (group_size == 2) {
      if (values[3] > UINT_MAX || values[8] > UINT_MAX) {
        free(values);
        free(loads);
        fclose(group_log);
        pair_log_error(line_num, "memory size is out of range");
      }

      loads[0].pc = values[0];
      loads[0].virtual_addr = values[1];
      loads[0].cache_block_addr = get_cache_block_addr(values[1]);
      loads[0].cache_block_offset = values[2];
      loads[0].mem_size = values[3];
      loads[0].micro_op_num = values[4];
      loads[1].pc = values[5];
      loads[1].virtual_addr = values[6];
      loads[1].cache_block_addr = get_cache_block_addr(values[6]);
      loads[1].cache_block_offset = values[7];
      loads[1].mem_size = values[8];
      loads[1].micro_op_num = values[9];
      recorded_span = values[10];
    } else {
      if (values[0] != group_size) {
        free(values);
        free(loads);
        fclose(group_log);
        pair_log_error(line_num, "CSV group size does not match parameter");
      }

      for (ii = 0; ii < group_size; ii++) {
        uns offset = 1 + ii * IDEAL_FUSION_GROUP_LOAD_FIELDS;
        if (values[offset + 3] > UINT_MAX) {
          free(values);
          free(loads);
          fclose(group_log);
          pair_log_error(line_num, "memory size is out of range");
        }

        loads[ii].pc = values[offset];
        loads[ii].virtual_addr = values[offset + 1];
        loads[ii].cache_block_addr = get_cache_block_addr(values[offset + 1]);
        loads[ii].cache_block_offset = values[offset + 2];
        loads[ii].mem_size = values[offset + 3];
        loads[ii].micro_op_num = values[offset + 4];
      }
      recorded_span = values[1 + group_size * IDEAL_FUSION_GROUP_LOAD_FIELDS];
    }

    validate_group_metadata(loads, group_size, recorded_span, group_log,
                            line_num);
    index_fusion_group(loads, group_size);
    free(values);
    free(loads);
  }

  if (ferror(group_log)) {
    fclose(group_log);
    fprintf(stderr, "Ideal fusion: could not read candidate log '%s': %s\n",
            IDEAL_FUSION_LOG, strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (fclose(group_log) != 0) {
    fprintf(stderr, "Ideal fusion: could not close candidate log '%s': %s\n",
            IDEAL_FUSION_LOG, strerror(errno));
    exit(EXIT_FAILURE);
  }

  group_indexes_loaded = TRUE;
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
  if (fclose(candidate_log) != 0)
    fprintf(stderr, "Ideal fusion: could not close candidate output file '%s': %s\n",
            IDEAL_FUSION_LOG, strerror(errno));
  candidate_log = NULL;
}

static void open_candidate_log(void) {
  uns group_size = ideal_fusion_group_size();

  if (candidate_log)
    return;

  if (!IDEAL_FUSION_LOG || !IDEAL_FUSION_LOG[0]) {
    fprintf(stderr, "Ideal fusion: candidate output path must not be empty.\n");
    exit(EXIT_FAILURE);
  }

  candidate_log = fopen(IDEAL_FUSION_LOG, "w");
  if (!candidate_log)
    candidate_log_error("open");

  if (group_size == 2) {
    if (fprintf(candidate_log, "%s\n", IDEAL_FUSION_PAIR_CSV_HEADER) < 0)
      candidate_log_error("initialize");
  } else {
    char header[IDEAL_FUSION_CSV_LINE_SIZE];
    build_group_csv_header(header, sizeof(header), group_size);
    if (fprintf(candidate_log, "%s\n", header) < 0)
      candidate_log_error("initialize");
  }

  if (fflush(candidate_log) != 0)
    candidate_log_error("initialize");
  atexit(close_candidate_log);
}

static void log_matched_group(const Ideal_Fusion_Load_Info* loads,
                              uns group_size, uns proc_id) {
  Counter span =
    loads[group_size - 1].micro_op_num - loads[0].micro_op_num;
  uns ii;

  open_candidate_log();
  if (group_size == 2) {
    if (fprintf(candidate_log,
                "0x%llx,0x%llx,%llu,%u,%llu,0x%llx,0x%llx,%llu,%u,%llu,%llu\n",
                loads[0].pc, loads[0].virtual_addr,
                loads[0].cache_block_offset, loads[0].mem_size,
                loads[0].micro_op_num, loads[1].pc, loads[1].virtual_addr,
                loads[1].cache_block_offset, loads[1].mem_size,
                loads[1].micro_op_num, span) < 0)
      candidate_log_error("write to");
  } else {
    if (fprintf(candidate_log, "%u", group_size) < 0)
      candidate_log_error("write to");

    for (ii = 0; ii < group_size; ii++) {
      if (fprintf(candidate_log, ",0x%llx,0x%llx,%llu,%u,%llu",
                  loads[ii].pc, loads[ii].virtual_addr,
                  loads[ii].cache_block_offset, loads[ii].mem_size,
                  loads[ii].micro_op_num) < 0)
        candidate_log_error("write to");
    }

    if (fprintf(candidate_log, ",%llu\n", span) < 0)
      candidate_log_error("write to");
  }

  STAT_EVENT(proc_id, IDEAL_FUSION_CANDIDATE_GROUPS_LOGGED);
  INC_STAT_EVENT(proc_id, IDEAL_FUSION_CANDIDATE_LOADS_LOGGED,
                 group_size);

  if (++logged_candidate_group_count % 1000 == 0 &&
      fflush(candidate_log) != 0)
    candidate_log_error("flush");
}

static Flag is_eligible_load(const Op* op) {
  return op->inst_info->table_info.mem_type == MEM_LD &&
    op->inst_info->table_info.num_dest_regs > 0 &&
    op->oracle_info.va != 0 && op->oracle_info.mem_size != 0 &&
    access_fits_in_cache_block(op->oracle_info.va, op->oracle_info.mem_size);
}

static Flag is_eligible_candidate(const Ideal_Fusion_Load_Candidate* load,
                                  Addr cache_block_addr,
                                  Counter current_micro_op_num) {
  return load->info.cache_block_addr == cache_block_addr &&
    !load->fused &&
    current_micro_op_num > load->info.micro_op_num &&
    current_micro_op_num - load->info.micro_op_num <=
      IDEAL_FUSION_DISTANCE &&
    access_fits_in_cache_block(load->info.virtual_addr,
                               load->info.mem_size);
}

static void insert_oldest_candidate(Ideal_Fusion_Load_Candidate** selected,
                                    uns* selected_count, uns needed,
                                    Ideal_Fusion_Load_Candidate* candidate) {
  uns pos;

  for (pos = 0; pos < *selected_count; pos++) {
    if (candidate->info.micro_op_num < selected[pos]->info.micro_op_num)
      break;
  }

  if (*selected_count < needed) {
    uns move;
    for (move = *selected_count; move > pos; move--)
      selected[move] = selected[move - 1];
    selected[pos] = candidate;
    (*selected_count)++;
    return;
  }

  if (pos < needed) {
    uns move;
    for (move = needed - 1; move > pos; move--)
      selected[move] = selected[move - 1];
    selected[pos] = candidate;
  }
}

static Flag find_matching_load_group(Op* newest_load,
                                     Ideal_Fusion_Load_Candidate*** out_members,
                                     Ideal_Fusion_Load_Info** out_loads,
                                     uns* out_group_size) {
  Ideal_Fusion_Load_Candidate* candidate;
  Addr cache_block_addr;
  Ideal_Fusion_Load_Candidate** selected;
  Ideal_Fusion_Load_Info* loads;
  uns group_size = ideal_fusion_group_size();
  uns needed = group_size - 1;
  uns selected_count = 0;
  uns ii;
  uns bucket;

  *out_members = NULL;
  *out_loads = NULL;
  *out_group_size = group_size;

  if (!is_eligible_load(newest_load))
    return FALSE;

  selected = (Ideal_Fusion_Load_Candidate**)calloc(needed,
                                                   sizeof(*selected));
  if (!selected)
    return FALSE;

  cache_block_addr = get_cache_block_addr(newest_load->oracle_info.va);
  bucket = get_candidate_bucket(cache_block_addr);

  for (candidate = load_candidates[bucket]; candidate;
       candidate = candidate->next) {
    if (is_eligible_candidate(candidate, cache_block_addr,
                              newest_load->ideal_fusion_micro_op_num))
      insert_oldest_candidate(selected, &selected_count, needed, candidate);
  }

  if (selected_count != needed) {
    free(selected);
    return FALSE;
  }

  loads = (Ideal_Fusion_Load_Info*)calloc(group_size, sizeof(*loads));
  if (!loads) {
    free(selected);
    return FALSE;
  }

  for (ii = 0; ii < needed; ii++)
    loads[ii] = selected[ii]->info;
  fill_load_info_from_op(&loads[group_size - 1], newest_load);

  *out_members = selected;
  *out_loads = loads;
  return TRUE;
}

static void track_load(Op* op) {
  Ideal_Fusion_Load_Candidate* candidate;
  uns bucket;

  if (!is_eligible_load(op))
    return;

  candidate = (Ideal_Fusion_Load_Candidate*)malloc(sizeof(*candidate));
  if (!candidate)
    return;

  fill_load_info_from_op(&candidate->info, op);
  candidate->fused = FALSE;

  bucket = get_candidate_bucket(candidate->info.cache_block_addr);
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
    if ((*candidate)->info.cache_block_addr == cache_block_addr) {
      Ideal_Fusion_Load_Candidate* invalidated_candidate = *candidate;
      *candidate = invalidated_candidate->next;
      free(invalidated_candidate);
    } else {
      candidate = &(*candidate)->next;
    }
  }
}

static void cleanup_stale_loads(Counter current_micro_op_num) {
  uns bucket;

  for (bucket = 0; bucket < IDEAL_FUSION_LOAD_CANDIDATE_BUCKETS; bucket++) {
    Ideal_Fusion_Load_Candidate** candidate = &load_candidates[bucket];

    while (*candidate) {
      if ((*candidate)->fused ||
          current_micro_op_num - (*candidate)->info.micro_op_num >
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
  Ideal_Fusion_Load_Candidate** group_members;
  Ideal_Fusion_Load_Info* group_loads;
  uns group_size;
  uns ii;

  if (!op || op->off_path)
    return;

  op->ideal_fusion_micro_op_num = ++next_on_path_micro_op_num;

  if (op->inst_info->table_info.mem_type == MEM_LD) {
    STAT_EVENT(op->proc_id, IDEAL_FUSION_ON_PATH_MEM_LOADS);
    if (is_eligible_load(op))
      STAT_EVENT(op->proc_id, IDEAL_FUSION_ELIGIBLE_ON_PATH_MEM_LOADS);
  }

  if (IDEAL_FUSION_PASS == 2) {
    load_group_indexes();
    classify_load(op);
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
    return;
  }

  if (find_matching_load_group(op, &group_members, &group_loads,
                               &group_size)) {
    log_matched_group(group_loads, group_size, op->proc_id);
    for (ii = 0; ii < group_size - 1; ii++)
      group_members[ii]->fused = TRUE;
    free(group_members);
    free(group_loads);
  } else {
    track_load(op);
  }
}

Flag ideal_fusion_load2_is_nop(const Op* op) {
  return op && op->ideal_fusion_load_role == IDEAL_FUSION_LOAD2;
}

static void mark_load2_handled(Ideal_Fusion_Readiness* readiness) {
  if (readiness->handled_load2_count < readiness->expected_load2_count)
    readiness->handled_load2_count++;

  if (readiness->handled_load2_count >= readiness->expected_load2_count)
    remove_readiness(readiness);
}

static void add_load2_waiter(Ideal_Fusion_Readiness* readiness, Op* load2) {
  Ideal_Fusion_Load2_Waiter* waiter =
    (Ideal_Fusion_Load2_Waiter*)malloc(sizeof(*waiter));

  if (!waiter) {
    fprintf(stderr, "Ideal fusion: could not allocate LOAD2 waiter.\n");
    exit(EXIT_FAILURE);
  }

  waiter->load2 = load2;
  waiter->load2_unique_num = load2->unique_num;
  waiter->next = readiness->load2_waiters;
  readiness->load2_waiters = waiter;
}

void ideal_fusion_on_rename(Op* op, void (*wake_action)(Op*, Op*, uns)) {
  Ideal_Fusion_Readiness* readiness;
  uns expected_load2_count;

  if (!op || op->off_path || IDEAL_FUSION_PASS != 2)
    return;

  if (op->ideal_fusion_load_role == IDEAL_FUSION_LOAD1) {
    expected_load2_count =
      get_expected_load2_count(op->ideal_fusion_micro_op_num);
    get_or_create_readiness(op->ideal_fusion_micro_op_num,
                            expected_load2_count);
    return;
  }

  if (!ideal_fusion_load2_is_nop(op))
    return;

  expected_load2_count =
    get_expected_load2_count(op->ideal_fusion_partner_micro_op_num);
  readiness = get_or_create_readiness(
    op->ideal_fusion_partner_micro_op_num, expected_load2_count);

  /*
   * LOAD1 may have completed before LOAD2 reached rename. In that case,
   * release LOAD2's dependents immediately after their wake-up links exist.
   */
  if (readiness->load1_completed) {
    wake_up_ops(op, REG_DATA_DEP, wake_action);
    mark_load2_handled(readiness);
    return;
  }

  add_load2_waiter(readiness, op);
}

void ideal_fusion_on_load_complete(Op* op,
                                   void (*wake_action)(Op*, Op*, uns)) {
  Ideal_Fusion_Readiness* readiness;
  Ideal_Fusion_Load2_Waiter* waiter;
  uns handled_count = 0;
  uns expected_load2_count;

  if (!op || op->off_path || IDEAL_FUSION_PASS != 2 ||
      op->ideal_fusion_load_role != IDEAL_FUSION_LOAD1)
    return;

  expected_load2_count =
    get_expected_load2_count(op->ideal_fusion_micro_op_num);
  readiness = get_or_create_readiness(op->ideal_fusion_micro_op_num,
                                      expected_load2_count);
  readiness->load1_completed = TRUE;

  /*
   * LOAD2 may already be waiting after rename. Its memory access is skipped,
   * so LOAD1 completion is the event that releases LOAD2's dependents.
   */
  waiter = readiness->load2_waiters;
  while (waiter) {
    Ideal_Fusion_Load2_Waiter* next = waiter->next;
    if (waiter->load2 &&
        waiter->load2->unique_num == waiter->load2_unique_num &&
        waiter->load2->op_pool_valid)
      wake_up_ops(waiter->load2, REG_DATA_DEP, wake_action);
    handled_count++;
    free(waiter);
    waiter = next;
  }
  readiness->load2_waiters = NULL;

  readiness->handled_load2_count += handled_count;
  if (readiness->handled_load2_count >= readiness->expected_load2_count)
    remove_readiness(readiness);
}
