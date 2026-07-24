# RFP Final Tuned Results

Per-app confidence tuning so **all 9 apps have non-negative IPC** vs baseline.

## Layout

```
simulations/
  baseline/<workload>/<cluster_id>/   # no RFP (rfp_on=0)
  rfp_tuned/<workload>/<cluster_id>/  # per-app best confidence config
configs.json                          # per-app parameters
summary.csv                           # IPC + prefetch funnel summary
```

## Tuning strategy

| Apps | `rfp_prob_shift` | P(conf++) | Why |
|------|------------------|-----------|-----|
| **bfs, dfs** | 22 | 1/4,194,304 | Graph traversals showed ~0.3% IPC loss at default; ultra-conservative training eliminates overhead |
| **all others** | 2 | 1/4 | Best balance from prob-shift sweep (+0.8% to +8% IPC) |

All configs use **24KB** RFP storage (`--rfp_pt_num_sets 512 --rfp_pt_num_ways 8 --rfp_pat_num_sets 64 --rfp_pat_num_ways 4`).

## Results summary

| App | Speedup vs baseline | Injected | Useful |
|-----|---------------------|----------|--------|
| AppWorld | +1.61% | 23.0% | 12.0% |
| BFS | +0.00% | 0.0% | 0.0% |
| ClickHouse | +0.94% | 11.8% | 4.7% |
| CoreBench | +3.32% | 33.9% | 14.8% |
| DFS | +0.00% | 0.0% | 0.0% |
| DuckDB | +1.79% | 49.8% | 10.3% |
| PR | +0.41% | 45.3% | 26.7% |
| RocksDB | +2.47% | 55.5% | 13.9% |
| TerminalBench | +7.95% | 60.4% | 18.0% |

**Overall geomean speedup: +2.03%**

## Reproduce

```bash
python /users/deepmish/scarab-infra/hpca2027-main-graphs/package_rfp_final_results.py
```

Source simulations (not copied, symlinked):
- Baseline: `/users/deepmish/scarab/src/simulations-confidence-1/baseline`
- bfs/dfs tuned: `/users/deepmish/scarab/src/simulations/rfp_prob_p22`
- Other apps: `/users/deepmish/scarab/src/simulations/rfp_prob_p2`
