# RFP Results — baseline vs Register File Prefetching (L1 DCache assoc = 8)

Results of the baseline-vs-RFP sweep on 16 datacenter workloads (all simpoints, no warmup,
`PARAMS.golden_cove` with **`--dcache_assoc 8`**), run via scarab-infra's SimPoint-weighted flow.

> **Scope note:** this run uses an 8-way L1 data cache (down from the default 12-way).
> 16 of the original 20 workloads are included — `memcached`, `pytorch`, `redis`, and
> `tensorflow` are absent from the available HuggingFace trace dataset
> (`harry1332/ifuse-final-datacenter-traces-20260624`) and were dropped from the sweep.
> Consequently the "ML frameworks" coverage graph is empty and the "Databases & KV" graph
> covers 3 apps (mongodb, mysql, postgres).

## Layout

```
results/
├── graphs/                          # regenerated PNGs (this run: 8-way L1 DCache, 16 workloads)
│   ├── rfp_ipc_speedup.png          # per-workload IPC speedup vs baseline (all 16)
│   ├── rfp_ipc_speedup_graph.png    # IPC speedup for the 8 graph-analytics apps + Geomean
│   ├── rfp_coverage_combined.png    # Injected/Executed/Useful, all 16 + Mean
│   ├── rfp_coverage_graph.png       # by category: graph analytics
│   ├── rfp_coverage_databases.png   #              databases & KV stores (memcached/redis absent)
│   ├── rfp_coverage_llm.png         #              LLM / agent
│   └── rfp_coverage_ml.png          #              ML frameworks (empty — no traces this run)
├── data/
│   ├── ipc_speedup_summary.csv      # per-workload simpoint-weighted base/rfp IPC + speedup%
│   ├── rfp_coverage_summary.csv     # per-workload injected/executed/useful (% of all loads)
│   ├── hostdirect_ipc_results.csv   # PRIOR 12-way host-direct cross-check (+2.93%); NOT regenerated
│   └── collected_stats.csv.gz       # FULL scarab-infra stats capture (every counter, every simpoint)
└── scripts/
    ├── rfp_sweep.json               # copy of the scarab-infra descriptor for this sweep (16 workloads)
    ├── gen_summaries.py             # distill a sweep dir -> the two summary CSVs
    ├── plot_results.py              # regenerate the 6 standard graphs from the summary CSVs
    └── plot_ipc_graph_apps.py       # IPC benefit graph for the graph-analytics apps (+ Geomean)
```

## Headline numbers

- **IPC geomean speedup: +3.48%** (16 workloads, 8-way L1 DCache). Load-bound winners:
  sweRepair +15.7%, bfs +9.3%, dfs +8.7%, cd +4.9%, cc +4.7%, haystack +4.5%.
  Miss-bound apps ~0 (mysql +0.1%, postgres +0.1%, mongodb +0.3%).
- **Graph-analytics apps only (bfs, dfs, cc, cd, tc, pagerank, sssp_ego_fb, bc): geomean +4.20%**
  — the load-bound category benefits most; see `graphs/rfp_ipc_speedup_graph.png`.
- **Timeliness/accuracy (fraction of all loads), Mean:** Injected 59%, Executed 45%,
  Useful/Coverage 43%.

Directionally consistent with the prior 12-way run (20 workloads, geomean +2.85%): the same
load-bound winners lead, and reducing L1 DCache associativity 12→8 has only a modest effect.

## Reproduce

**Just the graphs, from the committed summaries (no sweep needed):**
```
python3 scripts/plot_results.py        # reads data/*.csv -> graphs/*.png
```

**Re-run the full sweep (scarab-infra, SimPoint-weighted):**
1. Get the traces — `setup-scarab-rfp.sh` pulls HuggingFace traces into
   `/dev/shm/baseline/simpoint_traces`.
2. Clone scarab-infra (branch `rfp`). The sweep descriptor `rfp_sweep.json` is committed
   in that repo at `json/rfp_sweep.json`.
3. Bootstrap + register the local traces (one-time):
   ```
   ./sci --init                                  # installs Docker + conda; requires ASLR=0:
                                                  #   echo 0 | sudo tee /proc/sys/kernel/randomize_va_space
   python3 scripts/register_local_traces.py      # registers /dev/shm traces into workloads_db.json (warmup 0)
   ```
4. Build, run, collect (point the descriptor's `scarab_path` at this RFP checkout):
   ```
   ./sci --build-scarab rfp_sweep
   ./sci --sim rfp_sweep
   ./sci --collect-stats rfp_sweep
   ./sci --visualize rfp_sweep
   ```
5. Redistill + replot into this package:
   ```
   python3 scripts/gen_summaries.py <root_dir>/simulations/rfp_sweep <scarab-infra>/workloads/workloads_db.json data/
   python3 scripts/plot_results.py          # the 6 standard graphs
   python3 scripts/plot_ipc_graph_apps.py   # graph-analytics IPC benefit graph
   gzip -c <root_dir>/simulations/rfp_sweep/collected_stats.csv > data/collected_stats.csv.gz
   ```

**No-Docker host-direct fallback** (cross-check, instruction-weighted): the `rfp_run/driver.sh`
sweep runs `build/opt/scarab --rfp_on {0,1}` per trace; its IPC results are captured in
`data/hostdirect_ipc_results.csv`.

Workload categories used in the grouped graphs (workloads absent from this run's dataset are
struck through): **Graph** (bfs, dfs, cc, cd, tc, pagerank, sssp_ego_fb, bc); **Databases/KV**
(mongodb, mysql, postgres; ~~memcached~~, ~~redis~~); **LLM/Agent** (chemcrow, haystack,
langchain, swe=sweRepair, toolformer); **ML** (~~pytorch~~, ~~tensorflow~~ — none available).
