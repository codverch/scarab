#include "fusion_predictor.h"

// Constructor: Initializes fusion predictor with given sets and ways
FusionPredictor::FusionPredictor(uint32_t sets, uint32_t ways) 
    : num_sets(sets), num_ways(ways) {
    local_predictor.resize(sets * ways); 
    global_predictor.resize(sets * ways); 
    selector_table.resize(sets, 0); // Initialize selector table to prefer local predictor
}

// Computes the index for the global predictor using the PC and global history.
// - XORs the PC with the global history and mods by the number of sets to get the index.
uint32_t FusionPredictor::compute_index(uint64_t pc, uint64_t global_history) {
    return (pc ^ global_history) % num_sets;
}

// Predicts whether instruction fusion should occur based on stored entries.
// - Uses the selector table to decide between the local and global predictor.
// - Searches the selected predictor for a matching tag.
// - If a match is found, returns the distance and predicts fusion if confidence is high.
bool FusionPredictor::predict(uint64_t pc, uint64_t global_history, uint8_t& distance) {
    uint32_t local_index = pc % num_sets;
    uint32_t global_index = compute_index(pc, global_history);
    uint32_t selection_index = pc % 2048;
    
    // Use local predictor if selector counter is below 2, otherwise use global predictor
    bool use_local = (selector_table[selection_index] < 2);
    auto& predictor = use_local ? local_predictor : global_predictor;
    uint32_t index = use_local ? local_index : global_index;
    
    // Search for a matching tag in the selected predictor
    for (uint32_t i = 0; i < num_ways; ++i) {
        FusionPredEntry& entry = predictor[index * num_ways + i];
        if (entry.tag == (pc & 0xFF)) { // Use only the lower 8 bits as the tag
            distance = entry.micro_op_distance;
            return (entry.saturating_counter == 3); // Predict fusion if confidence is high
        }
    }
    return false;
}

// Updates the predictor based on the actual execution outcome.
// - Adjusts the selector table confidence based on whether the prediction was correct.
// - Searches for an existing entry to update. If found, updates the confidence or resets the distance.
// - If no matching entry is found, finds the LRU entry and replaces it with the new prediction.
void FusionPredictor::update(uint64_t pc, uint64_t global_history, uint8_t distance, bool correct) {
    uint32_t local_set_index = pc % num_sets;
    uint32_t global_set_index = compute_index(pc, global_history);
    uint32_t selection_index = pc % 2048;

    // Adjust selector table confidence
    if (correct) {
        selector_table[selection_index] = std::min(selector_table[selection_index] + 1, 3);
    } else {
        selector_table[selection_index] = std::max(selector_table[selection_index] - 1, 0);
    }

    auto& predictor = (selector_table[selection_index] < 2) ? local_predictor : global_predictor;
    uint32_t set_index = (selector_table[selection_index] < 2) ? local_set_index : global_set_index;

    // Search for an existing entry to update
    for (uint32_t i = 0; i < num_ways; ++i) {
        FusionPredEntry& entry = predictor[set_index * num_ways + i];
        if (entry.tag == (pc & 0xFF)) {
            if (entry.micro_op_distance == distance) {
                // Distance matches: update confidence
                if (correct) {
                    entry.saturating_counter = std::min(entry.saturating_counter + 1, 3);
                } else {
                    entry.saturating_counter = std::max(entry.saturating_counter - 1, 0);
                }
            } else {
                // Distance does not match: reset distance and confidence
                entry.micro_op_distance = distance;
                entry.saturating_counter = 1; // Reset confidence
            }
            entry.pseudo_lru_bit = 1; // Mark as recently used
            return;
        }
    }

    // If no matching entry is found, find the LRU entry and replace it
    FusionPredEntry* lru_entry = find_lru_entry(predictor, set_index, num_ways);

    // Replace the LRU entry with the new prediction
    lru_entry->tag = pc & 0xFF;
    lru_entry->micro_op_distance = distance;
    lru_entry->saturating_counter = correct ? 3 : 1; // Initialize confidence level
    lru_entry->pseudo_lru_bit = 1; // Mark as recently used
}

// Finds the least recently used (LRU) entry in a set using the 1-bit pseudo-LRU policy.
// - Searches for an entry with pseudo_lru_bit = 0 (least recently used).
// - If all entries have pseudo_lru_bit = 1, resets all bits and evicts the first entry.
FusionPredEntry* find_lru_entry(std::vector<FusionPredEntry>& predictor, uint32_t set_index, uint32_t num_ways) {
    FusionPredEntry* lru_entry = nullptr;
    for (uint32_t i = 0; i < num_ways; ++i) {
        FusionPredEntry& entry = predictor[set_index * num_ways + i];
        if (entry.pseudo_lru_bit == 0) {
            // Found an entry with pseudo_lru_bit = 0 (least recently used)
            lru_entry = &entry;
            break;
        }
    }
    if (lru_entry == nullptr) {
        // All entries have pseudo_lru_bit = 1, reset all bits and evict the first entry
        for (uint32_t i = 0; i < num_ways; ++i) {
            predictor[set_index * num_ways + i].pseudo_lru_bit = 0;
        }
        lru_entry = &predictor[set_index * num_ways]; // Evict the first entry
    }
    return lru_entry;
}