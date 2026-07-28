# HPCA 2027 Runtime I-Fuse

## Mechanism

Runtime I-Fuse removes the offline PGO candidate file from the required path.
At retirement, committed loads are matched with the most recent older load that:

- accessed the same 64-byte cache line;
- is less than `IFUSE_FUSION_DISTANCE` micro-ops older;
- has no intervening committed store to that line; and
- fits completely within the cache line.

The pair key is `(LD1 PC, LD2 PC, signed offset delta, LD2 access size)`. A
bounded set-associative Training Table (TT) counts observations. At the
configured threshold, the pair is inserted into the existing Fusion Candidate
Table (FCT) at prediction-eligible confidence. A trusted conflicting FCT row is
preserved; a row whose confidence has fallen below threshold may be replaced.

TT replacement is RRIP-style (SRRIP). Each entry carries a 2-bit RRPV
(re-reference prediction value, range 0-3). A newly inserted candidate starts
at RRPV 2, not at the fully-protected 0 and not at the immediately-evictable
max of 3 — it must be re-observed to earn protection. Each repeat observation
of an existing entry decrements its RRPV toward 0. On a TT miss with no
invalid way available, the victim is the way in the set with the highest RRPV
(ties break to the lowest way index), so a candidate that has proven itself
through repeat observations is not evicted ahead of a brand-new, unobserved
one.

The PGO preload path remains available for controlled comparisons, but runtime
experiments must leave `--ifuse_fct_preload_file` unset.

The current implementation is single-core training state. Multi-core studies
must partition the retired-load history and TT by core before use.

## Default Configuration

```text
--ifuse_runtime_training_enabled 1
--ifuse_fusion_distance 512
--ifuse_training_insert_threshold 1000
--ifuse_training_table_sets 32
--ifuse_training_table_ways 4
--ifuse_pc_tag_bits 32
--ifuse_fct_runtime_insert_conf 500
--ifuse_fct_preload_file <unset>
```

The default TT has 128 entries. The intended size sweep is 32, 64, 128, and
256 total entries while holding associativity at four ways.

## Storage Accounting

For the default direct encodings, one TT entry contains a 32-bit LD1 PC tag
(`ifuse_training_table_pc_tag_bits`) plus a full 48-bit LD2 PC -- ld2 is kept
untruncated because it doubles as the value forwarded to
`FCT_Row.ld2_pc_addr`, which must be full precision, the same reasoning as
FCT's own `ld2_pc_addr` -- a 6-bit offset magnitude, 1 direction bit, a 3-bit
log2(access size), an 11-bit observation counter (supports insert thresholds
up to 2047, 2x headroom over the highest threshold actually swept, 1000), a
2-bit RRPV, and 1 valid bit: 104 bits per entry. A 128-entry TT is 13,312
bits (1,664 bytes). The LD1 tag is truncated only for the TT's internal
hit/miss decision; a promoted candidate always carries the full-precision PC
observed at that call (see `find_or_allocate()` in
`ifuse_training_table.c`), so a narrower LD1 tag can only misattribute an
observation count between two colliding candidates, never promote a
truncated PC. The C structure is larger because of host alignment and is not
the hardware size estimate.

For the FCT, one row contains a 32-bit LD1 PC tag (`ifuse_fct_pc_tag_bits`,
same truncation reasoning as the TT), a 48-bit LD2 PC (kept full width: this
is a predicted value that must exact-match a real future op's PC, not a
disambiguation tag, so truncating it would raise the false-fusion-match rate
directly), a 6-bit offset magnitude, 1 direction bit, a 3-bit log2(access
size), a 10-bit confidence score (2x headroom over the current default
ceiling of 500), and 1 valid bit: 101 bits per row. A 512-row FCT
(`ifuse_fct_hash_bits = 9`) is 51,712 bits (6,464 bytes). `ld1_effective_addr`,
`ld2_effective_addr`, `ld1_micro_op_num`, and `ld2_micro_op_num` are kept on
`FCT_Row` for simulator-side address bookkeeping but are not part of the
modeled hardware row: the prediction path (`ft.cc`) computes the predicted
LD2 address from the live dynamic op plus this row's offset_delta/direction,
and ACI validates it against the real cache block, so a looked-up row's
stored addresses are never read back.

The retired-load history (RLB) must be reported separately rather than hidden
as simulator bookkeeping. Each entry is a 48-bit PC, 48-bit effective address,
3-bit log2(access size), 10-bit modulo-1024 micro-op timestamp, and 1 valid
bit: 110 bits/entry. `ifuse_rlb_capacity` (default 128) is deliberately
smaller than `ifuse_fusion_distance` (default 512): entries only need to
cover the loads live within the distance window, not one slot per micro-op in
it, and measured load density (~20-30% of instructions across the workload
set) rarely fills more than ~150-160 slots in a 512-op window. At 128 entries
the RLB is 14,080 bits (1.71875 KiB), down from 6.875 KiB at 512. Capacity and
distance are independent: `RLB_AGE_EVICTIONS` counts entries that aged past
the distance window, `RLB_CAPACITY_EVICTIONS` counts entries forced out early
because the buffer was full; a rising `RLB_CAPACITY_EVICTIONS` share is the
signal that a workload's load density needs a larger `ifuse_rlb_capacity`.
Its hardware lookup bandwidth and compressed PC/address widths remain design
parameters for the final area/power estimate.

## Warmup And Measurement

Use full timing warmup, not Scarab's cache-only `WARMUP` mode:

```text
--full_warmup 10000000 --inst_limit 20000000
```

TT and FCT state persists across the 10M boundary. Use the post-warmup periodic
statistics (or subtract the `.warmup` checkpoint from cumulative counters) for
the measured result. A trace needs at least 20M instructions to provide 10M
training and 10M measurement. Shorter traces should be extended or reported
separately rather than silently using a different warmup.

## Required Validation

- Runtime runs have zero `FCT_PRELOAD_INSERTS`.
- Discovered pairs obey micro-op distance, cache-line, and store rules.
- Promotion occurs only after the configured number of identical observations.
- TT occupancy never exceeds sets times ways.
- Warmup statistics contain training activity and measured statistics show FCT
  lookups without resetting learned state.
- IPC and prediction accuracy are compared against baseline, ideal fusion, and
  PGO I-Fuse using the same measured instruction interval.

## Validation Status (2026-07-14)

TaoBench is excluded from the final HPCA 2027 workload set. The Tao results
below are retained only as engineering evidence that runtime training works;
they must not be included in final speedup plots or averages.

- Boundary/history tests pass for 511-versus-512 micro-op distance, most-recent
  matching, intervening-store invalidation, and 4/8-way tree PLRU behavior.
- The PT/memtrace `SCARABOPT` binary builds on the Utah CloudLab node with GCC
  9 and GNU `as`. Every runtime I-Fuse source file and retire integration point
  is included in that binary.
- A 1M-instruction Tao smoke run with threshold 10 discovered 1,933 pairs,
  promoted 148 FCT entries, and completed successfully with no PGO preload.
- The standard validation used a longer Tao capture, 10M full warmup, 10M
  measured instructions, and threshold 1,000. Warmup promoted 39 candidates;
  the measured interval promoted 17 more and fused 729,075 loads. The TT peak
  occupancy was 128 entries and `FCT_PRELOAD_INSERTS` remained zero.
- Measured IPC was 3.40930 for runtime I-Fuse and 3.40743 for the exact matching
  no-training baseline, a 0.055% speedup on this one Tao thread. This validates
  operation and measurement boundaries; it is not a full performance result.

Remote result directories:

```text
/proj/datacntr-effcy-PG0/Harry123/hpca2027_dcperf/simulations/runtime_ifuse_tao1000m_20m_warmup10m_threshold1000_20260714
/proj/datacntr-effcy-PG0/Harry123/hpca2027_dcperf/simulations/runtime_ifuse_tao1000m_baseline_20m_warmup10m_20260714
```
