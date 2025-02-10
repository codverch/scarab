#ifndef PREDICTOR_H
#define PREDICTOR_H

#ifdef __cplusplus
#include <vector>
#include <cstdint>
#else
#include <stdint.h>  
#endif

struct PredictorEntry {
    uint64_t pc;
    uint64_t distance;
    uint64_t counter;
};

#ifdef __cplusplus
class PredictorImpl;
#else
typedef struct PredictorImpl PredictorImpl;
#endif

#ifdef __cplusplus
extern "C" {
#endif

    PredictorImpl* predictor_create();
    void predictor_destroy(PredictorImpl* ctx);
    
    void predictor_insert(PredictorImpl* ctx, uint64_t prog_ctr, uint64_t head_nucl_distance);
    bool predictor_pc_is_in(PredictorImpl* ctx, uint64_t prog_ctr);
    void predictor_print(PredictorImpl* ctx);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
class PredictorImpl {
private:
    std::vector<PredictorEntry> entries;
    uint64_t global_commit = 0;
    size_t current_index = 0;
    uint64_t max_distance = 0;

public:
    PredictorImpl() = default;
    ~PredictorImpl() = default;
    
    void insert_predictor(uint64_t prog_ctr, uint64_t head_nucl_distance);
    bool pc_is_in_predictor(uint64_t prog_ctr);
    void print_predictor();
};
#endif 

#endif  // PREDICTOR_H