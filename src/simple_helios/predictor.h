#ifndef PREDICTOR_H
#define PREDICTOR_H

#ifdef __cplusplus
#include <vector>
#include <cstdint>
#include <stdio.h>
#else
#include <stdint.h>  
#endif

struct PredictorEntry {
    uint64_t pc;
    uint64_t distance;
    uint64_t counter;
};

#ifdef __cplusplus

// #ifdef __cplusplus
class PredictorImpl {
private:
    std::vector<PredictorEntry> entries;
    uint64_t global_commit = 0;
    size_t current_index = 0;
    uint64_t max_distance = 0;

public:
   PredictorImpl() {}



void insert_predictor(uint64_t prog_ctr, uint64_t head_nucl_distance) {
    printf("[insert_predictor] PC: %lx, Distance: %lx\n", prog_ctr, head_nucl_distance);

    if (pc_is_in_predictor(prog_ctr)) {
        return;
    }

    PredictorEntry new_entry = { prog_ctr, head_nucl_distance, 1 };
    entries.push_back(new_entry);
    printf("[insert_predictor] Added new entry: PC: %lx\n", prog_ctr);
}

bool pc_is_in_predictor(uint64_t prog_ctr) {
    printf("[pc_is_in_predictor] Checking PC: %lx\n", prog_ctr);

    for (auto& entry : entries) {
        if (entry.pc == prog_ctr) {
            if (entry.counter < 3) entry.counter++;
            printf("[pc_is_in_predictor] Found PC: %lx (counter now %lu)\n", prog_ctr, entry.counter);
            return true;
        }
    }
    return false;
}

void print_predictor() {
    printf("\nPredictor Table Contents (%zu entries):\n", entries.size());
    for (const auto& entry : entries) {
        printf("  PC: %-8lx | Distance: %-8lx | Counter: %lu\n",
              entry.pc, entry.distance, entry.counter);
    }
    printf("-----------------------------\n");
}

// extern "C" {
//     PredictorImpl* predictor_create() {
//         return new PredictorImpl();
//     }

//     void predictor_destroy(PredictorImpl* ctx) {
//         delete ctx;
//     }

//     void predictor_insert(PredictorImpl* ctx, uint64_t pc, uint64_t distance) {
//         if (ctx) ctx->insert_predictor(pc, distance);
//     }

//     bool predictor_pc_is_in(PredictorImpl* ctx, uint64_t prog_ctr) {
//         return ctx ? ctx->pc_is_in_predictor(prog_ctr) : false;
//     }

//     void predictor_print(PredictorImpl* ctx) {
//         if (ctx) ctx->print_predictor();
//     }
    
// };
// #endif 

}

#else
typedef struct PredictorImpl PredictorImpl;
#endif

// #ifdef __cplusplus
// extern "C" {
// #endif

//     PredictorImpl* predictor_create();
//     void predictor_destroy(PredictorImpl* ctx);
    
//     void predictor_insert(PredictorImpl* ctx, uint64_t prog_ctr, uint64_t head_nucl_distance);
//     bool predictor_pc_is_in(PredictorImpl* ctx, uint64_t prog_ctr);
//     void predictor_print(PredictorImpl* ctx);

// #ifdef __cplusplus
// }
// #endif


#endif  // PREDICTOR_H