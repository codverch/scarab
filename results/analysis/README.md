# How each evaluation number is computed

Every script here reads the per-simpoint stats under `results/<config>/<app>/<simpoint>/`
and weights simpoints with `scarab-infra/workloads/workloads_db.json`, as
`hpca2027-main-graphs/plot_ipc.py` does. Averages are arithmetic means over the
eleven applications.

## Runs

| Directory | Build | What it is |
|---|---|---|
| `results/baseline` | `cb2ec6957` | No fusion |
| `results/1-cycle-delayed-ifuse` | `36c4b3029` | I-Fuse in the submitted paper: true fusion (LD2 takes no ROB, LSQ, or RS entry), 1-cycle LD2 wake delay |
| `results/ideal-fusion` | | Ideal load µop fusion |
| `results/helios`, `results/rfp` | | Helios and RFP |

## Scripts

- `speedup.py BASE NAME=DIR ...`: per-app speedup, `weighted IPC(cfg) / weighted IPC(base) - 1`.
  Reproduces the paper exactly: I-Fuse 13.66%, Ideal 20.66% (`speedup_paper_runs.csv`).
- `retire_lat.py BASE LOADS_DIR NAME=DIR ...`: fetch-to-retire load latency reduction,
  `LD_RETIRE_MINUS_FETCH_LATENCY / IFUSE_ALL_LOADS` per simpoint (`retire_latency_paper_runs.csv`:
  I-Fuse 17.48%, RFP 3.05%).

## Known problems

- Fig. 16 in the submission mixed metrics: fetch-to-execute for I-Fuse and RFP,
  fetch-to-retire for Helios. It now uses fetch-to-retire for every scheme.
- `results/helios` has no load-latency counters, so Helios's latency reduction
  (3.2% in the paper) cannot be reproduced from it. Helios needs a rerun with
  `hpca2027-helios` at or after `522dc5777`.
- The backend-stall breakdown (8.4% to 4.2% of cycles with an L1-D hit at the
  ROB head) came from runs `rb-baseline` and `rb-ifuse1c`, built from
  `backend-stall-counters` (`74ae2974a`), which are no longer on disk. Recompute
  it from the `ifuse-ablation` runs, which carry the same counters.
- The BFS simpoints (17, 94, 96, 103) produce identical stats.
