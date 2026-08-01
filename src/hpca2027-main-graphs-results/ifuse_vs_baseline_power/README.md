# I-Fuse vs baseline McPAT power / area

## Sources
- **Baseline power:** `origin/hpca2027-baseline` `src/simulations/baseline/<app>/<sp>/mcpat.out`
  (copied under `../baseline_mcpat_power/`). All 10 apps, `--dcache_assoc 12`.
- **I-Fuse power:** latest `src/simulations/ifuse` assoc-12 runs, with I-Fuse tables
  injected into `mcpat_infile.xml` and re-run through McPAT `ifuse` branch
  (`../ifuse_mcpat_power/`). Geometry: RLB=128, FCT=512, TT=32×4, APT=32×8, ACI=64×4.

## Headline overheads (10-app mean)
- **Storage (bit packing):** ~13.8 KiB
- **Area:** 0.080 mm² ≈ **0.37%** of baseline core area
- **I-Fuse structure runtime power:** ≈ **1.0%** of baseline core runtime dynamic
- **Total core runtime Δ vs baseline:** ≈ **+3.8%** mean (mix of I-Fuse tables +
  activity differences between campaigns — see `StrOH%` for structure-only)

See `overhead_summary.txt` and `overhead_by_app.csv`.
