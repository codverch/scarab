#include "predictor.h"
#include <algorithm>
#include <cstdio>
#include <unordered_map>

class PredictorImpl {
private:
    std::vector<PredictorEntry> entries;
    uint64_t global_commit = 0;
    size_t current_index = 0;
    uint64_t max_distance = 0;

public:
    void insert_predictor(uint64_t prog_ctr, uint64_t head_nucl_distance) {
        printf("[insert_predictor] PC: %lx, Distance: %lx\n", prog_ctr, head_nucl_distance);

        // Use pc_is_in_predictor to check and update the entry if it exists
        if (pc_is_in_predictor(prog_ctr)) {
            return; // Return early if a matching entry is found and updated
        }

        // Otherwise, create a new entry in the predictor table
        PredictorEntry entry;
        entry.pc = prog_ctr;
        entry.distance = head_nucl_distance;
        entry.counter = 1;

        entries.push_back(entry);
        printf("[insert_predictor] Adding new entry: PC: %lx, Distance: %lx\n", prog_ctr, head_nucl_distance);
    }

    bool pc_is_in_predictor(uint64_t prog_ctr) {
        printf("[pc_is_in_predictor] PC: %lx\n", prog_ctr);

        // Check if the PC is in the predictor table
        for (auto& entry : entries) {
            if (entry.pc == prog_ctr) {
                // If it is, increment the counter by 1
                if (entry.counter < 3) {
                    entry.counter++;
                }
                printf("[pc_is_in_predictor] Found matching entry: PC: %lx, Distance: %lx, Counter: %lu\n", entry.pc, entry.distance, entry.counter);
                return true; // Return true if a matching entry is found and updated
            }
        }
        return false; // Return false if no matching entry is found
    }

    void print_predictor() {
        for (auto& entry : entries) {
            printf("PC: %lx, Distance: %lx, Counter: %lu\n", entry.pc, entry.distance, entry.counter);
        }
    }
};

/* C interface implementation */
extern "C" {
    PredictorImpl* predictor_create() {
        return new PredictorImpl();
    }

    void predictor_insert(PredictorImpl* ctx, uint64_t pc, uint64_t distance) {
        if (ctx) {
            ctx->insert_predictor(pc, distance);
        }
    }

    void pc_is_in_predictor(PredictorImpl* ctx, uint64_t prog_ctr) {
        if (ctx) {
            ctx->pc_is_in_predictor(prog_ctr);
        }
    }

    void predictor_print(PredictorImpl* ctx) {
        if (ctx) {
            ctx->print_predictor();
        }
    }
}
