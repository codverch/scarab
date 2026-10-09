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

## Realistic LD2 and the fusion/early-data ablation (`ifuse-ablation`)

Build `ae923d935` (branch `ifuse-realistic-ld2`): the paper commit `36c4b3029`, plus
the knobs below and the backend-stall counters of `74ae2974a`. Descriptor:
`scarab-infra/json/hpca2027/ifuse-ablation.json`. 32 simpoints per config.

| Directory | Knob | Meaning |
|---|---|---|
| `results/ifuse-repro` | none | Same model as the paper; reproduces `1-cycle-delayed-ifuse` exactly on all 11 apps |
| `results/ifuse-realistic` | `--ifuse_ld2_realistic 1` | LD2 takes a ROB, LQ, and RS entry and an AGU slot; no L1-D access |
| `results/ifuse-fusion-only` | `--ifuse_ablate_no_early_data 1` | LD2 makes no L1-D access, but its data is ready no earlier than its own AGU plus the L1-D hit latency |
| `results/ifuse-early-only` | `--ifuse_ablate_no_fusion 1` | LD2's dependents wake on LD1's data, but LD2 also makes its own L1-D access |
| `results/nofuse`, `results/nofuse-prf2x` | training off | No fusion with the I-Fuse binary, at 280 and 560 integer registers |
| `results/ifuse-realistic-prf2x` | realistic + 560 registers | |

`speedup_ablation.csv` (vs `results/baseline`): repro 13.66, realistic 13.72, fusion-only 5.15,
early-only 7.51 (average %). Fusion alone gives nearly all of the gain in BFS, DFS, PR, and
ClickHouse; early data alone gives nearly all of it in the other seven apps.
`speedup_prf2x.csv`: realistic I-Fuse over no fusion, both at 560 registers: 11.16%.
`stalls_ablation.csv` (`stalls.py`): backend-stalled cycles as % of no-fusion cycles. The
cycles with an L1-D hit at the ROB head go from 8.43% to 4.21% (repro) and 3.94% (realistic),
which verifies the 8.4% to 4.2% in Section VI-B.

Each `sim.log` here omits its repeated "Patching gap in trace" lines and records their count.

## Helios rerun with load-latency counters (`helios-tuned`)

Build: branch `hpca2027-helios`. Descriptors `scarab-infra/json/hpca2027/helios-tuned-0..7.json`,
one per group of applications, with each application's Helios settings from the paper's `PARAMS.out`.
`speedup_helios_rerun.csv`: the rerun reproduces the paper's Helios speedups exactly (1.84% average).
`retire_latency_realistic.csv` (`retire_lat.py`, loads from `ifuse-realistic`): fetch-to-retire load
latency reduction is 3.16% for Helios, 18.18% for realistic I-Fuse, and 3.05% for RFP.
This fills Fig. 17's Helios bars and `\helioslatencyreduction` (3.2%).
