#include "load_history.h"
#include "predictor.h"
#include <algorithm>
#include <unordered_map>
#include <cstdio>

class LoadHistoryImpl {
private:
    std::vector<LoadEntry> entries;
    uint64_t global_commit = 0;
    size_t current_index = 0;
    PredictorImpl *predictor;

public:
    LoadHistoryImpl(PredictorImpl* predictor_table) : predictor(predictor_table) {}
    void insert_load(uint64_t pc, uint64_t eff_addr, bool is_mem_load) {
        printf("[insert_load] PC: %lx, Effective Address: %lx, Is Mem Load: %d\n", pc, eff_addr, is_mem_load);

         for (auto& entry : entries) {
            if ((entry.effective_addr == eff_addr) && (entry.is_mem_load == is_mem_load) && (!entry.is_fused)) {
                // Use the external predictor table
                if (predictor && predictor->pc_is_in_predictor(entry.pc)) {
                    printf("[insert_load] Found fusion pair: PC: %lx, Effective Address: %lx\n", entry.pc, eff_addr);
                    return;
                }
            }
        }

        if (entries.size() < HISTORY_SIZE) {
            entries.push_back({pc, eff_addr, global_commit++, false, is_mem_load});
        } else {
            entries[current_index] = {pc, eff_addr, global_commit++, false, is_mem_load};
            current_index = (current_index + 1) % HISTORY_SIZE;
        }
    }

    void print_load_history() {
        for (const auto& entry : entries) {
            printf("PC: %lx, Effective Address: %lx, Commit Number: %lu, Is Fused: %d, Is Mem Load: %d\n",
                   entry.pc, entry.effective_addr, entry.commit_num, entry.is_fused, entry.is_mem_load);
        }
    }
};

extern "C" {
    LoadHistoryImpl* load_history_create(PredictorImpl* predictor_table) {
        return new LoadHistoryImpl(predictor_table);
    }

    void load_history_destroy(LoadHistoryImpl* ctx) {
        delete ctx;
    }

    void load_history_insert(LoadHistoryImpl* ctx, uint64_t pc, uint64_t eff_addr, bool is_mem_load) {
        if (ctx) ctx->insert_load(pc, eff_addr, is_mem_load);
    }

    void print_load_history(LoadHistoryImpl* ctx) {
        if (ctx) ctx->print_load_history();
    }
}
