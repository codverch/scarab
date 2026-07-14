#include "ifuse_rename.h"

#include "ifuse_apt.h"
#include "../map.h"
#include "../map_rename.h"
#include "../globals/assert.h"
#include "../isa/isa.h"
#include "../isa/isa_macros.h"
#include "../statistics.h"
#include "ifuse_stats.h"

/**
 * IFuse speculative-register lifecycle:
 *
 * 1. LOAD1 reaches rename and allocates one physical GPR reserved for the
 *    predicted LOAD2 result. The register is held in APT, not yet installed as
 *    LOAD2's architectural destination.
 * 2. If the matching LOAD2 reaches rename and fusion is correct, LOAD2 does
 *    not keep the destination allocated by ordinary rename. Instead its
 *    architectural destination is rebound to LOAD1's reserved physical
 *    register, which is what dependents and reg_file_produce() use.
 * 3. After that handoff, lifetime follows normal rename (flush/commit). The
 *    reserved register is no longer tracked as an IFuse "extra".
 * 4. If LOAD2 never arrives, validation fails, or APT is cleaned up, the
 *    reserved register is returned to the free list.
 */

static Flag ifuse_is_gp_arch_reg(int arch_reg_id) {
  return (arch_reg_id >= REG_RAX && arch_reg_id < REG_CS) ||
         (arch_reg_id >= REG_TMP0 && arch_reg_id <= REG_TMP4) ||
         (arch_reg_id >= REG_ZPS && arch_reg_id < REG_ZMM0);
}

static int ifuse_find_gp_dest_index(Op* op) {
  for (uns ii = 0; ii < op->inst_info->table_info.num_dest_regs; ++ii) {
    int arch_reg_id = op->dst_reg_id[ii][REG_TABLE_TYPE_ARCHITECTURAL];
    int phys_reg_id = op->dst_reg_id[ii][REG_TABLE_TYPE_PHYSICAL];
    if (arch_reg_id == REG_TABLE_REG_ID_INVALID ||
        phys_reg_id == REG_TABLE_REG_ID_INVALID) {
      continue;
    }
    if (ifuse_is_gp_arch_reg(arch_reg_id)) {
      return (int)ii;
    }
  }
  return -1;
}

/**
 * Allocates one physical GPR reserved for a predicted LD2 result.
 */
static int ifuse_alloc_ld2_physical_reg(Op* ld1_op) {
  struct reg_table* physical_reg_table =
      map_data->reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]
          ->reg_table[REG_TABLE_TYPE_PHYSICAL];

  if (physical_reg_table->free_list->reg_free_num == 0) {
    return REG_TABLE_REG_ID_INVALID;
  }

  struct reg_table_entry* entry =
      physical_reg_table->free_list->ops->alloc(physical_reg_table->free_list);

  /* Held outside the arch map until LOAD2 rename installs it as LD2's dest. */
  entry->ops->write(entry, ld1_op, REG_TABLE_REG_ID_INVALID);
  entry->num_refs++;

  ifuse_extra_reg_note_alloc();
  return entry->self_reg_id;
}

/**
 * Releases an unconsumed IFuse-reserved physical register back to the pool.
 */
void ifuse_free_ld2_physical_reg(unsigned int ld2_physical_reg_id) {
  if (ld2_physical_reg_id == REG_TABLE_REG_ID_INVALID) {
    return;
  }

  struct reg_table* physical_reg_table =
      map_data->reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]
          ->reg_table[REG_TABLE_TYPE_PHYSICAL];
  struct reg_table_entry* entry =
      &physical_reg_table->entries[ld2_physical_reg_id];

  physical_reg_table->ops->free(physical_reg_table, entry);
  ifuse_extra_reg_note_free();
}

static void ifuse_release_phys_entry(struct reg_table* physical_reg_table,
                                    unsigned int phys_reg_id) {
  struct reg_table_entry* entry = &physical_reg_table->entries[phys_reg_id];

  ASSERT(map_data->proc_id, entry->num_refs > 0);
  entry->num_refs--;
  if (entry->num_refs > 0) {
    return;
  }

  ASSERT(map_data->proc_id, entry->reg_state != REG_TABLE_ENTRY_STATE_FREE);
  physical_reg_table->ops->free(physical_reg_table, entry);
}

/**
 * Replace LOAD2's ordinary rename destination with the physical register that
 * LOAD1 reserved. Returns TRUE on success.
 */
static Flag ifuse_bind_ld2_dest_to_reserved(Op* op, unsigned int reserved_id) {
  int dest_idx = ifuse_find_gp_dest_index(op);
  if (dest_idx < 0 || reserved_id == REG_TABLE_REG_ID_INVALID) {
    return FALSE;
  }

  struct reg_table* physical_reg_table =
      map_data->reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]
          ->reg_table[REG_TABLE_TYPE_PHYSICAL];

  unsigned int wrong_id = op->dst_reg_id[dest_idx][REG_TABLE_TYPE_PHYSICAL];
  int parent_arch_id = op->dst_reg_id[dest_idx][REG_TABLE_TYPE_ARCHITECTURAL];

  ASSERT(op->proc_id, parent_arch_id != REG_TABLE_REG_ID_INVALID);
  ASSERT(op->proc_id, wrong_id != REG_TABLE_REG_ID_INVALID);

  if (wrong_id != reserved_id) {
    /* Drop the destination that ordinary rename just allocated. */
    ifuse_release_phys_entry(physical_reg_table, wrong_id);
  }

  struct reg_table_entry* reserved =
      &physical_reg_table->entries[reserved_id];
  ASSERT(op->proc_id, reserved->reg_state != REG_TABLE_ENTRY_STATE_FREE);
  ASSERT(op->proc_id, reserved->num_refs > 0);

  /*
   * Re-associate the reserved entry with LOAD2 and install it in the arch map
   * so producers/consumers use the same physical register LOAD1 allocated.
   */
  reserved->ops->write(reserved, op, parent_arch_id);
  physical_reg_table->parent_reg_table->entries[parent_arch_id].child_reg_id =
      reserved_id;
  op->dst_reg_id[dest_idx][REG_TABLE_TYPE_PHYSICAL] = reserved_id;

  /*
   * The register is no longer an IFuse sidecar held outside rename; it is
   * LOAD2's normal destination. Occupancy stays correct because wrong_id was
   * freed above when it differed from reserved_id.
   */
  ifuse_extra_reg_note_free();
  return TRUE;
}

void ifuse_rename_op(Op* op) {
  if (op->ifuse_load_role == LOAD1) {
    int ld2_physical_reg_id = ifuse_alloc_ld2_physical_reg(op);

    /* APT owns the reserved register until LOAD2 rename or stale cleanup. */
    if (ld2_physical_reg_id != REG_TABLE_REG_ID_INVALID &&
        !apt_set_ld2_physical_reg_id((unsigned int)op->op_num,
                                     ld2_physical_reg_id)) {
      ifuse_free_ld2_physical_reg(ld2_physical_reg_id);
    }
    return;
  }

  if (op->ifuse_load_role == LOAD2) {
    unsigned int ld2_physical_reg_id = REG_TABLE_REG_ID_INVALID;

    if (apt_take_ld2_physical_reg_id(op->inst_info->addr,
                                     (unsigned int)op->ifuse_partner_op_num,
                                     &ld2_physical_reg_id)) {
      if (ifuse_bind_ld2_dest_to_reserved(op, ld2_physical_reg_id)) {
        /*
         * Ownership transferred into dst_reg_id. Normal flush/commit manage
         * lifetime; do not free again in ifuse_retire/recover.
         */
        op->ifuse_ld2_physical_reg_id = REG_TABLE_REG_ID_INVALID;
      } else {
        /* No GP dest to bind; return the reservation to avoid a leak. */
        ifuse_free_ld2_physical_reg(ld2_physical_reg_id);
        op->ifuse_ld2_physical_reg_id = REG_TABLE_REG_ID_INVALID;
      }
    }
    return;
  }

  if (op->ifuse_load_role == PREDICTED_NOT_FUSED) {
    /* Validation failed: LOAD2 keeps its ordinary dest; free the reservation. */
    apt_remove_entry_by_ld1_micro_op(op->inst_info->addr,
                                     (unsigned int)op->ifuse_partner_op_num);
  }
}

void ifuse_retire_op(Op* op) {
  if (op->ifuse_ld2_physical_reg_id == REG_TABLE_REG_ID_INVALID) {
    return;
  }

  /*
   * Only reached if a reserved register was still tracked as an IFuse sidecar
   * after rename. Successful fused handoff clears this ID so commit alone
   * manages the destination register.
   */
  ifuse_free_ld2_physical_reg(op->ifuse_ld2_physical_reg_id);
  op->ifuse_ld2_physical_reg_id = REG_TABLE_REG_ID_INVALID;
}

void ifuse_recover_op(Op* op) {
  if (op->ifuse_ld2_physical_reg_id == REG_TABLE_REG_ID_INVALID) {
    return;
  }

  /*
   * Same as retire: successful handoff clears this ID, and flush_mispredict
   * already frees dst_reg_id. Only free here if the register remained an
   * unbound IFuse reservation.
   */
  ifuse_free_ld2_physical_reg(op->ifuse_ld2_physical_reg_id);
  op->ifuse_ld2_physical_reg_id = REG_TABLE_REG_ID_INVALID;
}
