#include "ifuse_rename.h"

#include "ifuse_apt.h"
#include "../map.h"
#include "../map_rename.h"

/**
 * IFuse speculative-register lifecycle:
 *
 * 1. LOAD1 reaches rename and allocates one extra physical register.
 * 2. The register ID is stored in LOAD1's APT entry.
 * 3. If the matching LOAD2 reaches rename within the allowed fusion distance,
 *    it consumes that register ID and the APT entry is removed.
 * 4. If no matching LOAD2 arrives in time, stale APT cleanup removes the entry
 *    and frees the extra register.
 *
 * Fusion distance counts dynamic on-path micro-ops, not loads alone.
 */

/**
 * Allocates one IFuse-only general-purpose physical register for a predicted
 * LD2 result. The register is intentionally not installed into the
 * architectural map.
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

    entry->ops->write(entry, ld1_op, REG_TABLE_REG_ID_INVALID);
    entry->num_refs++;

    return entry->self_reg_id;
}

/**
 * Releases an IFuse-only general-purpose physical register.
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
}

void ifuse_rename_op(Op* op) {
    if (op->ifuse_load_role == LOAD1) {
        int ld2_physical_reg_id = ifuse_alloc_ld2_physical_reg(op);

        // The live APT entry owns this extra register while LOAD1 waits for its
        // predicted LOAD2. Stale APT cleanup will free it if LOAD2 never arrives.
        if (ld2_physical_reg_id != REG_TABLE_REG_ID_INVALID &&
            !apt_set_ld2_physical_reg_id((unsigned int)op->op_num,
                                         ld2_physical_reg_id)) {
            ifuse_free_ld2_physical_reg(ld2_physical_reg_id);
        }
        return;
    }

    if (op->ifuse_load_role == LOAD2) {
        unsigned int ld2_physical_reg_id = REG_TABLE_REG_ID_INVALID;

        // LOAD2 takes ownership of the extra register allocated by LOAD1.
        // Removing the APT entry completes the successful rename-stage handoff.
        if (apt_take_ld2_physical_reg_id(op->inst_info->addr,
                                         (unsigned int)op->ifuse_partner_op_num,
                                         &ld2_physical_reg_id)) {
            op->ifuse_ld2_physical_reg_id = ld2_physical_reg_id;
        }
        return;
    }

    if (op->ifuse_load_role == PREDICTED_NOT_FUSED) {
        // LD2 arrived, but validation failed. It cannot consume LOAD1's extra
        // register, so remove the APT entry and return the register to the pool.
        apt_remove_entry_by_ld1_micro_op(
            op->inst_info->addr,
            (unsigned int)op->ifuse_partner_op_num);
    }
}

void ifuse_retire_op(Op* op) {
    if (op->ifuse_ld2_physical_reg_id == REG_TABLE_REG_ID_INVALID) {
        return;
    }

    // A successful LOAD2 owns LOAD1's extra register after rename. Once LOAD2
    // retires, the speculative fused result is no longer live.
    ifuse_free_ld2_physical_reg(op->ifuse_ld2_physical_reg_id);
    op->ifuse_ld2_physical_reg_id = REG_TABLE_REG_ID_INVALID;
}

void ifuse_recover_op(Op* op) {
    if (op->ifuse_ld2_physical_reg_id == REG_TABLE_REG_ID_INVALID) {
        return;
    }

    // LOAD2 took ownership during rename but was flushed before retirement.
    // Return LOAD1's extra physical register to the free list.
    ifuse_free_ld2_physical_reg(op->ifuse_ld2_physical_reg_id);
    op->ifuse_ld2_physical_reg_id = REG_TABLE_REG_ID_INVALID;
}
