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
--ifuse_training_table_pc_tag_bits 48
--ifuse_fct_runtime_insert_conf 500
--ifuse_fct_preload_file <unset>
```

The default TT has 128 entries. The intended size sweep is 32, 64, 128, and
256 total entries while holding associativity at four ways.

## Storage Accounting

For the default direct encodings, one TT entry contains two 48-bit PC tags, a
6-bit offset magnitude, 1 direction bit, 7 access-size bits, a 10-bit
observation counter, and 1 valid bit: 121 bits per entry. A 128-entry TT is
15,488 bits (1,936 bytes), plus 3 PLRU bits per set (12 bytes), for 1,948 bytes.
The C structure is larger because of host alignment and is not the hardware
size estimate.

The retired-load history must be reported separately rather than hidden as
simulator bookkeeping. The current model holds up to 512 recent loads. Its
hardware organization, lookup bandwidth, and compressed PC/address widths are
still design parameters and must be included in the final area/power estimate.

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
