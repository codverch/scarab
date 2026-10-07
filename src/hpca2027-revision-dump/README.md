# HPCA 2027 revision: LD2 bank-port results

These runs answer the reviewer point that a fused access must not get a free
L1-D bank. Since commit 4e42b21dd ("Charge each fused LD2 word its own L1-D
bank port"), LD2's word asks for a read port on its own bank, like a separate
load. If that port is busy, only LD2's word waits.

## Configurations

All runs: 11 apps, 32 simpoints, 20M warmup + 10M measured instructions,
Golden Cove config, 1 read port per L1-D bank, 8 banks.

| Directory | Binary (commit) | Source params | Change |
|---|---|---|---|
| `ifuse-bankport` | 4e42b21dd | committed 1-cycle-delayed I-Fuse runs | `--ifuse_ld2_wake_delay 0` |
| `1-cycle-delayed-ifuse-bankport` | 4e42b21dd | same | `--ifuse_ld2_wake_delay 1` |
| `ifuse-bankport-8B` | 4e42b21dd | same | delay 0, `--dcache_interleave_factor 8` |
| `1-cycle-delayed-ifuse-bankport-8B` | 4e42b21dd | same | delay 1, `--dcache_interleave_factor 8` |
| `baseline-8B` | cb2ec6957 | committed baseline (revision-main) | `--dcache_interleave_factor 8` |
| `helios-8B` | a88031dcd | committed Helios (revision-main), per-app knobs | `--dcache_interleave_factor 8` |
| `rfp-8B` | 57cfe922e | committed RFP (revision-main) | `--dcache_interleave_factor 8` |

The binaries are the commits recorded in the committed runs' sim.log. Each run
takes its committed run's PARAMS.out ("--" section) and changes only the
flags above. The default interleave is 4 B.

`validate/` holds reproduction checks. Unchanged baseline, Helios, and RFP
configs on bfs/94 and clickhouse/17 match the committed IPCs exactly, which
shows the rebuilt binaries and traces are the same.

## Notes

- `--power_intf_on 0`: no McPAT output. Power does not affect timing.
- The baseline, Helios, and RFP binaries print `Scarab gitrev: cb2ec6957`
  because they were built straight from CMake (to skip the PIN frontend), and
  the gitrev file was not regenerated. The table above lists the real commits.
- New counters in `ifuse.stat.0.out`: `IFUSE_LD2_WORD_READS`,
  `IFUSE_LD2_WORD_DELAYED`, `IFUSE_LD2_WORD_BANK_CONFLICT_CYCLES`,
  `IFUSE_LD2_WORD_AHEAD_OF_LD1`.

## Reproduce

```bash
python3 run_campaign.py --set bankport --out .
python3 run_campaign.py --set interleave8 --out .
python3 summarize.py > SUMMARY.md
```
