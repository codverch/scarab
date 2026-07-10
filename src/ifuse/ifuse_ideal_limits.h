#ifndef IFUSE_IDEAL_LIMITS_H
#define IFUSE_IDEAL_LIMITS_H

/**
 * Compile-time upper bounds for tunable IFuse table parameters.
 * Runtime sizes come from ifuse.param.def (IFUSE_* knobs).
 */

#define IFUSE_IDEAL_FCT_MAX_HASH_BITS     24U
#define IFUSE_IDEAL_MAX_BUCKETS           65536U
#define IFUSE_IDEAL_MAX_NODES             (1U << 24)

#endif /* IFUSE_IDEAL_LIMITS_H */
