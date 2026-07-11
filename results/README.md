# RFP Results — baseline vs Register File Prefetching

Results of the baseline-vs-RFP sweep on 20 datacenter workloads (all simpoints, no warmup,
`PARAMS.golden_cove`), run via scarab-infra's SimPoint-weighted flow.

## Layout

```
results/
├── graphs/                          # the current graphs (PNG)
│   ├── rfp_ipc_speedup.png          # per-workload IPC speedup vs baseline
│   ├── rfp_coverage_combined.png    # Injected/Executed/Useful, all 20 + Mean
│   ├── rfp_coverage_graph.png       # by category: graph analytics
│   ├── rfp_coverage_databases.png   #              databases & KV stores
│   ├── rfp_coverage_llm.png         #              LLM / agent
│   └── rfp_coverage_ml.png          #              ML frameworks
├── data/
│   ├── ipc_speedup_summary.csv      # per-workload simpoint-weighted base/rfp IPC + speedup%
│   ├── rfp_coverage_summary.csv     # per-workload injected/executed/useful (% of all loads)
│   ├── hostdirect_ipc_results.csv   # independent host-direct sweep (cross-check, +2.93% geomean)
│   └── collected_stats.csv.gz       # FULL scarab-infra stats capture (every counter, every simpoint)
└── scripts/
    ├── rfp_sweep.json               # copy of the scarab-infra descriptor for this sweep
    ├── gen_summaries.py             # distill a sweep dir -> the two summary CSVs
    └── plot_results.py              # regenerate every graph from the summary CSVs (self-contained)
```

## Headline numbers

- **IPC geomean speedup: +2.85%** (paper +3.1%). Load-bound winners: sweRepair +15.9%, bfs +9.6%,
  dfs +9.0%, cd +4.9%, cc +4.8%, haystack +4.5%. Miss-bound apps ~0 (memcached, mysql, postgres).
- **Timeliness/accuracy (fraction of all loads), Mean:** Injected 61%, Executed 46%,
  Useful/Coverage 44% (paper Fig 13 Mean: 72 / 48 / 43).

## Reproduce

**Just the graphs, from the committed summaries (no sweep needed):**
```
python3 scripts/plot_results.py        # reads data/*.csv -> graphs/*.png
```

**Re-run the full sweep (scarab-infra, SimPoint-weighted):**
1. Get the traces — `setup-scarab-rfp.sh` pulls `vedlaksh/datacenter-traces` (HuggingFace) into
   `/dev/shm/baseline/simpoint_traces`.
2. Clone scarab-infra (branch `ideal-fusion`). The sweep descriptor `rfp_sweep.json` is committed
   in that repo at `json/rfp_sweep.json` (this `scripts/rfp_sweep.json` is just an archival copy —
   `./sci` only reads descriptors from `<scarab-infra>/json/`).
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
   python3 scripts/plot_results.py
   ```

**No-Docker host-direct fallback** (cross-check, instruction-weighted): the `rfp_run/driver.sh`
sweep runs `build/opt/scarab --rfp_on {0,1}` per trace; its IPC results are captured in
`data/hostdirect_ipc_results.csv`.

Workload categories used in the grouped graphs: **Graph** (bfs, dfs, cc, cd, tc, pagerank,
sssp_ego_fb, bc); **Databases/KV** (memcached, mongodb, mysql, postgres, redis); **LLM/Agent**
(chemcrow, haystack, langchain, swe=sweRepair, toolformer); **ML** (pytorch, tensorflow).
