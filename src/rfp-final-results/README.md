# RFP Final Results (24KB storage)

Per-app tuned RFP prefetcher results for all 11 workloads. Each app folder
contains inlined `baseline/` and `rfp/` simpoint result files.

## Storage budget

- **PT**: 512 sets × 8 ways = 32 KiB (modeled in McPAT as `RFP_PT`)
- **PAT**: 64 sets × 4 ways = 2 KiB (modeled in McPAT as `RFP_PAT`)
- **Total RFP SRAM**: 24 KiB (PT+PAT structures; McPAT configs in `mcpat_infile.xml`)

Power modeling: `--power_intf_on 1` with RFP PT/PAT components in McPAT input.

## Layout

```
rfp-final-results/
  <app>/
    baseline/<simpoint_id>/
    rfp/<simpoint_id>/
  configs.json
  summary.csv
```

## Per-app configs

| App | Config | prob_shift | stride_bits | Speedup vs baseline | mcpat.out |
|-----|--------|------------|-------------|---------------------|-----------|
| appworld | p2/s5b/signed/24KB | 2 | 5 | +1.61 | 3/3 |
| bfs_web-google | p3/s16b/signed/24KB | 3 | 16 | +1.12 | 4/4 |
| clickhouse | p2/s5b/signed/24KB | 2 | 5 | +0.94 | 1/1 |
| core_bench | p2/s5b/signed/24KB | 2 | 5 | +2.20 | 4/4 |
| dfs_web-google | p0/s16b/signed/24KB | 0 | 16 | -0.00 | 3/3 |
| duckdb | p2/s5b/signed/24KB | 2 | 5 | +1.79 | 2/2 |
| leveldb | p2/s5b/signed/24KB | 2 | 5 | +3.69 | 2/2 |
| pagerank_gnutella31 | p1/s16b/signed/24KB | 1 | 16 | +0.42 | 5/5 |
| rocksdb | p2/s5b/signed/24KB | 2 | 5 | +2.47 | 3/3 |
| sssp_ego-facebook | p1/s16b/signed/24KB | 1 | 16 | +0.32 | 2/2 |
| terminal_bench | p2/s5b/signed/24KB | 2 | 5 | +7.93 | 2/2 |

## Common Scarab flags (all apps)

```
--rfp_conf_max 1 --rfp_pt_num_sets 512 --rfp_pt_num_ways 8 --rfp_pat_num_sets 64 --rfp_pat_num_ways 4
```

## Simulation window

- `full_warmup`: 20,000,000 instructions
- `inst_limit`: 30,000,000 instructions (20M warmup + 10M measured)
- Traces: `/dev/shm/baseline/simpoint_traces`

## All apps positive speedup

No — re-run `./json/hpca2027/rfp.sh --tune`

See `configs.json` for per-app `params` strings and simpoint counts.
