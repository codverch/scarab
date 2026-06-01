#ifndef IFUSE_LOAD_TRACKER_H
#define IFUSE_LOAD_TRACKER_H

#include <stdint.h>

/**
 * Returns the next dynamic on-path load number.
 *
 * IFuse fusion distance is measured in loads, not in all fetched micro-ops.
 */
uint64_t ifuse_next_load_num(void);

#endif /* IFUSE_LOAD_TRACKER_H */
