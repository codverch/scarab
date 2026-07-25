# RFP Final Results Package

Per-app tuned RFP prefetcher results (9 workloads). Each app folder contains
`baseline/` and `rfp/` simpoint symlinks into the source simulation trees.

## Layout

```
rfp-final-results/
  <app>/
    baseline/<simpoint_id>/   -> simulations-confidence-1/baseline/<app>/<id>
    rfp/<simpoint_id>/        -> per-app best RFP config (see below)
  configs.json
  summary.csv
```

## Baseline

All workloads use `simulations-confidence-1/baseline/<app>/<simpoint_id>/`.

## Per-app RFP configs

| App | Config | prob_shift | stride_bits | stride_signed | conf_max |
|-----|--------|------------|-------------|---------------|----------|
| bfs | rfp_p3_s16 | 3 | 16 | signed (1) | 1 |
| dfs | rfp_p0_s16 | 0 | 16 | signed (1) | 1 |
| pagerank | rfp_p1_s16 | 1 | 16 | signed (1) | 1 |
| appworld | rfp_prob_p2 | 2 | 5 | signed (1) | 1 |
| clickhouse | rfp_prob_p2 | 2 | 5 | signed (1) | 1 |
| core_bench | rfp_prob_p2 | 2 | 5 | signed (1) | 1 |
| duckdb | rfp_prob_p2 | 2 | 5 | signed (1) | 1 |
| rocksdb | rfp_prob_p2 | 2 | 5 | signed (1) | 1 |
| terminal_bench | rfp_prob_p2 | 2 | 5 | signed (1) | 1 |

Graph workloads (bfs, dfs, pagerank) use 16-bit signed stride after per-app tuning
(`rfp-graph-stride` / `rfp-graph-stride2` sweeps). Other apps use the original
`rfp_prob_p2` config from the confidence-1 storage sweep.

## Common Scarab flags (all apps)

```
--rfp_pt_num_sets 512 --rfp_pt_num_ways 8 --rfp_pat_num_sets 64 --rfp_pat_num_ways 4
```

## Full CLI example (BFS)

```
--rfp_prob_shift 3 --rfp_conf_max 1 --rfp_stride_signed 1 --rfp_stride_bits 16 \
--rfp_pt_num_sets 512 --rfp_pt_num_ways 8 --rfp_pat_num_sets 64 --rfp_pat_num_ways 4
```

See `configs.json` for per-app `params` strings and simpoint counts.
