#include "predictor.h"

void insert_into_predictor(uint64_t pc, uint8_t saturating_ctr, uint8_t micro_op_distance, Predictor* predictor) {
    // Check if the predictor is full
    if (is_predictor_full(predictor)) {
        // Remove the oldest entry
        predictor->tail = (predictor->tail + 1) % PREDICTOR_SIZE;
        predictor->size--;
    }

    // Insert the new entry
    predictor->entries[predictor->head].prog_ctr = pc;
    predictor->entries[predictor->head].saturating_ctr = saturating_ctr;
    predictor->entries[predictor->head].micro_op_distance = micro_op_distance;

    predictor->head = (predictor->head + 1) % PREDICTOR_SIZE;
    predictor->size++;
}

void update_saturating_ctr(uint64_t pc, Predictor* predictor) {
    // Search for the entry with the matching program counter
    for (int i = 0; i < predictor->size; i++) {
        if (predictor->entries[i].prog_ctr == pc) {
            
        }
    }
}