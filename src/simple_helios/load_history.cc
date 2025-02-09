#include "load_history.h"
#include <algorithm>
#include <unordered_map>
#include <cstdio>

class LoadHistoryImpl {
private:
    std::vector<LoadEntry> entries;
    uint64_t global_commit = 0;
    size_t current_index = 0;

public:
    void insert_load(uint64_t pc, uint64_t eff_addr, bool is_mem_load) {
        printf("[insert_load] PC: %lx, Effective Address: %lx, Is Mem Load: %d\n", pc, eff_addr, is_mem_load);

        // Check if an entry with the same effective address is already in the history table
        for (auto& entry : entries) {
            if ((entry.effective_addr == eff_addr) && (entry.is_mem_load == is_mem_load) 
                && (!entry.is_fused)) {
                // train_predictor(entry.pc, pc); // Uncomment if needed
                printf("[insert_load] Found fusion pair: PC: %lx, Effective Address: %lx\n", entry.pc, eff_addr);
                return;
            }
        }

        // If no fusion pair is found, add a new entry
        if (entries.size() < HISTORY_SIZE) {
            entries.push_back({pc, eff_addr, global_commit++, false, is_mem_load});
        } else {
            entries[current_index] = {pc, eff_addr, global_commit++, false, is_mem_load};
            current_index = (current_index + 1) % HISTORY_SIZE;
        }

        printf("[insert_load] Adding new entry: PC: %lx, Effective Address: %lx\n", pc, eff_addr);
    }

    void print_load_history() {
        printf("Print Load History:\n");
        // print number of entries in the history table
        printf("Number of entries in the history table: %lu\n", entries.size());

        for (const auto& entry : entries) {
            printf("PC: %lx, Effective Address: %lx, Commit Number: %lu, Is Fused: %d, Is Mem Load: %d\n",
                   entry.pc, entry.effective_addr, entry.commit_num, entry.is_fused, entry.is_mem_load);
        }
    }
};

/* C interface implementation */
extern "C" {
    LoadHistoryImpl* load_history_create() {
        return new LoadHistoryImpl();
    }

    void load_history_insert(LoadHistoryImpl* ctx, uint64_t pc, uint64_t eff_addr, bool is_mem_load) {
        if (ctx) {
            ctx->insert_load(pc, eff_addr, is_mem_load);
        }
    }

    void print_load_history(LoadHistoryImpl* ctx) {
        printf("Load History:\n");
        if (ctx) {
            ctx->print_load_history();
        }
    }
}
