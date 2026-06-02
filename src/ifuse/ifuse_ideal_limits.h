#ifndef IFUSE_IDEAL_LIMITS_H
#define IFUSE_IDEAL_LIMITS_H

/**
 * Fixed upper bounds for ideal (capacity-free) IFuse tables.
 *
 * Tables are fully allocated at init; there is no runtime growth, rehash, or
 * bump-chunk expansion. Exhaustion drops inserts (rare if limits are generous).
 */

/* Ideal FCT simulator hash table: 2^N row buckets (see IFUSE_IDEAL_FCT_HASH_BITS). */
#define IFUSE_IDEAL_FCT_DEFAULT_HASH_BITS 22U
#define IFUSE_IDEAL_FCT_MAX_HASH_BITS     24U

/* Live APT / ACI nodes (hash buckets are fixed; nodes come from these pools). */
#define IFUSE_IDEAL_APT_MAX_NODES  (1U << 19) /* 524288 */
#define IFUSE_IDEAL_ACI_MAX_NODES  (1U << 19)
#define IFUSE_IDEAL_EXEC_PAIR_MAX_NODES (1U << 19)

/*
 * Retire-time training-table entries (distinct fusible pair patterns).
 * Keep this deliberately large for ideal-limit studies. The training table
 * probes every row before reporting exhaustion, so collisions do not silently
 * discard observations while unused backing rows remain.
 */
#define IFUSE_IDEAL_TRAINING_TABLE_MAX_ENTRIES (1U << 24) /* 16777216 */

#endif /* IFUSE_IDEAL_LIMITS_H */
