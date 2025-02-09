#ifndef LOAD_HISTORY_H
#define LOAD_HISTORY_H

#include <stdint.h>
#include <stdbool.h>

#define CACHE_LINE_SIZE 64
#define HISTORY_SIZE 6  /* Matches paper's 6-entry UCH */
#define MAX_FUSION_DISTANCE 64

#ifdef __cplusplus
#include <vector>
extern "C" {
#endif

/* C-compatible struct */
typedef struct {
    uint64_t pc;
    uint64_t effective_addr;  
    uint64_t commit_num;
    bool is_fused;
    bool is_mem_load;
} LoadEntry;

#ifdef __cplusplus
/* Forward declaration for C++ */
class LoadHistoryImpl;
#else
/* Opaque type for C */
typedef struct LoadHistoryImpl LoadHistoryImpl;
#endif

/* C interface functions */
LoadHistoryImpl* load_history_create();
void load_history_insert(LoadHistoryImpl* ctx, uint64_t pc, uint64_t eff_addr, bool is_mem_load);
void print_load_history(LoadHistoryImpl* ctx);

#ifdef __cplusplus
}
#endif

#endif /* LOAD_HISTORY_H */

/**************************************************************************************/