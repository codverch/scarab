# ROB varying-size results

Restored simulation results for baseline and runtime I-Fuse across ROB sizes 128 / 192 / 256.

## Source commits

- `52ac78f5a` — Add baseline ROB size sweep results (128/192/256) with trimmed sim.logs.
- `eaea6ca73` — Add ifuse ROB sweep results (128/192/256) with trimmed sim.log noise.

Originally under `src/simulations/{baseline,ifuse}-rob{128,192,256}` and
`{baseline,ifuse}-rob-sweep/collected_stats.csv`.

## Layout

- `baseline-rob128/`, `baseline-rob192/`, `baseline-rob256/` — per-app simpoint stats
- `ifuse-rob128/`, `ifuse-rob192/`, `ifuse-rob256/` — runtime I-Fuse per-app simpoint stats
- `baseline-rob-sweep/collected_stats.csv` — aggregated baseline sweep stats
- `ifuse-rob-sweep/collected_stats.csv` — aggregated I-Fuse sweep stats
