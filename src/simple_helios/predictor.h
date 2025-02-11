#ifndef PREDICTOR_H
#define PREDICTOR_H

#include <stdint.h>

#define PREDICTOR_SIZE 64

typedef struct {
    uint64_t prog_ctr;
    uint8_t saturating_ctr; // Confidence counter - stores values from 0-3 
    uint8_t micro_op_distance; // Distance to the head nucleus (tail nucleus entry)
} PredictorEntry;

typedef struct {
    PredictorEntry entries[PREDICTOR_SIZE];
    uint8_t head;
    uint8_t tail;
    uint8_t size;  
} Predictor;

// Function prototypes
void insert_into_predictor(uint64_t pc, uint8_t saturating_ctr, uint8_t micro_op_distance, Predictor* predictor);
void update_saturating_ctr(uint64_t pc, uint8_t micro_op_distance, Predictor* predictor);
int is_predictor_full(const Predictor* predictor);
int is_predictor_empty(const Predictor* predictor);

#endif // PREDICTOR_H
