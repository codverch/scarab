#ifndef PREDICTOR_H
#define PREDICTOR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif 

// the predictor will have a PC value, a distance to the head nucleus to fuse with, and a saturating counter 
typedef struct {
    uint64_t pc;
    uint64_t distance;
    uint64_t counter;
} PredictorEntry;

#ifdef __cplusplus
/* Forward declaration for C++ */
class PredictorImpl; 
#else
typedef struct PredictorImpl PredictorImpl;
#endif

/* C interface functions */
PredictorImpl* predictor_create();
void predictor_insert(PredictorImpl* ctx, uint64_t pc, uint64_t distance);
void predictor_print(PredictorImpl* ctx);


#ifdef __cplusplus
}
#endif 

#endif /* PREDICTOR_H */