#ifndef PREDICTOR_H
#define PREDICTOR_H

// Include C-compatible headers for uint64_t
#ifdef __cplusplus
#include <vector>
#include <cstdint>
#else
#include <stdint.h>  // For C compatibility
#endif

// Struct visible to both C and C++
struct PredictorEntry {
    uint64_t pc;
    uint64_t distance;
    uint64_t counter;
};

// Forward declaration for C compatibility
#ifdef __cplusplus
class PredictorImpl;
#else
typedef struct PredictorImpl PredictorImpl;
#endif

// C interface declarations (wrapped properly)
#ifdef __cplusplus
extern "C" {
#endif

    // Constructor/Destructor
    PredictorImpl* predictor_create();
    void predictor_destroy(PredictorImpl* ctx);
    
    // Core functions
    void predictor_insert(PredictorImpl* ctx, uint64_t prog_ctr, uint64_t head_nucl_distance);
    bool predictor_pc_is_in(PredictorImpl* ctx, uint64_t prog_ctr);
    void predictor_print(PredictorImpl* ctx);

#ifdef __cplusplus
}
#endif

// C++ class definition (hidden from C)
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
#endif  // __cplusplus

#endif  // PREDICTOR_H