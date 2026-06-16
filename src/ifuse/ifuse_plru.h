#ifndef IFUSE_PLRU_H
#define IFUSE_PLRU_H

#include <stdint.h>

/**
 * Tree PLRU replacement for 4- or 8-way set-associative IFuse tables.
 *
 * plru_state holds one state byte per set. Call ifuse_plru_touch on hit or
 * insert, and ifuse_plru_victim when evicting from a full set.
 */
void ifuse_plru_touch(uint8_t* plru_state, unsigned int set_idx,
                      unsigned int way, unsigned int num_ways);

unsigned int ifuse_plru_victim(const uint8_t* plru_state,
                               unsigned int set_idx,
                               unsigned int num_ways);

#endif /* IFUSE_PLRU_H */
