#ifndef FUSION_PREDICTOR_
#define FUSION_PREDICTOR_

#include <vector>
#include <cstdint>
#include <algorithm>

// Structure representing an entry in the fusion predictor
struct FusionPredEntry {
    uint8_t tag;                    // 8-bit tag representing partial cacheline address
    uint8_t micro_op_distance;       // Distance (in uops) to the head nucleus (6-bits)
    uint8_t saturating_counter;      // 2-bit confidence counter (higher = more confident)
    uint8_t pseudo_lru_bit;          // Pseudo-LRU replacement bit to track usage (1-bit)
};

// Class implementing the fusion predictor
class FusionPredictor {
    private: 
        std::vector<FusionPredEntry> local_predictor; // Local predictor indexed by PC
        std::vector<FusionPredEntry> global_predictor; // Global predictor using (PC ⊕ history)
        std::vector<uint8_t> selector_table; // Tournament selector table (2-bit counters)
        uint32_t num_sets; // Number of sets in the predictor tables
        uint32_t num_ways; // Associativity (ways) of the predictor
        uint32_t compute_index(uint64_t pc, uint64_t global_history);
    
    public: 
        FusionPredictor(uint32_t sets, uint32_t ways);
        bool predict(uint64_t pc, uint64_t global_history, uint8_t& distance);
        void update(uint64_t pc, uint64_t global_history, uint8_t distance, bool correct);
        FusionPredEntry* find_lru_entry(std::vector<FusionPredEntry>& predictor, uint32_t index, uint32_t num_ways);
};

#endif // FUSION_PREDICTOR_