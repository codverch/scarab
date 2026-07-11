# HELIOS Optimized Configs — Store-Store Fusion OFF (Golden Cove)

## What "T / I / D" means

HELIOS gates each fusion on a per-entry saturating confidence counter
(`I-Fuse/src/general.param.def`):

| Knob | Param | Role |
|------|-------|------|
| **T** | `--helios_confidence_threshold` | confidence a candidate must reach before it fuses (higher ⇒ more conservative) |
| **I** | `--helios_confidence_increment` | confidence gained by FP training from UCH |
| **D** | `--helios_confidence_decrement` | confidence lost on a misprediction |

Every HELIOS arm carries **store-store fusion OFF** (`--helios_fuse_stores 0`) plus the common
`--fetch_off_path_ops 0`. The **baseline** arm runs fusion fully disabled (no `--helios_do_fusion`).
Fixed: `I = 1`. Rule: `D = 3` when `T = 3`, else `D = 10`. Per-app tuning = the single knob **T**.

---

## Per-app optimized configs

12 of 16 apps beat baseline with store fusion off. The other 4 have no config that beats
baseline, so they run at **T = 65000** (the least-aggressive best-attempt arm, ~baseline).

| App | Baseline IPC | **T** | I | D | HELIOS IPC | Speedup | Config column |
|-----|-------------:|------:|--:|--:|-----------:|--------:|---------------|
| pagerank      | 3.250563 | **1000**  | 1 | 10 | 3.860023 | **+18.75%** | `helios_T1000_ns` |
| cc            | 3.213649 | **3**     | 1 | 3  | 3.515156 | **+9.38%**  | `helios_T3_ns` |
| cd            | 4.218458 | **3**     | 1 | 3  | 4.459243 | **+5.71%**  | `helios_T3_ns` |
| swe_agent     | 1.706983 | **3**     | 1 | 3  | 1.750935 | **+2.57%**  | `helios_T3_ns` |
| bfs           | 3.214943 | **3**     | 1 | 3  | 3.288240 | **+2.28%**  | `helios_T3_ns` |
| dfs           | 3.217929 | **3**     | 1 | 3  | 3.272187 | **+1.69%**  | `helios_T3_ns` |
| bc            | 0.707349 | **3**     | 1 | 3  | 0.711120 | **+0.53%**  | `helios_T3_ns` |
| mongodb       | 1.456794 | **10**    | 1 | 10 | 1.462535 | **+0.39%**  | `helios_T10_ns` |
| chemcrow      | 2.439545 | **3**     | 1 | 3  | 2.448632 | **+0.37%**  | `helios_T3_ns` |
| langchain_web | 1.772915 | **10000** | 1 | 10 | 1.773114 | **+0.01%**  | `helios_T10000_ns` |
| mysql         | 0.730450 | **10000** | 1 | 10 | 0.730493 | **+0.01%**  | `helios_T10000_ns` |
| postgres      | 2.527791 | **30000** | 1 | 10 | 2.527791 | **+0.00%**  | `helios_T30000_ns` |
| toolformer    | 1.999928 | 65000 | 1 | 10 | 1.999928 | −0.00% | `helios_T65000_ns` (no gain) |
| sssp_ego_fb   | 1.262102 | 65000 | 1 | 10 | 1.262028 | −0.01% | `helios_T65000_ns` (no gain) |
| rag_haystack  | 3.042680 | 65000 | 1 | 10 | 3.029571 | −0.43% | `helios_T65000_ns` (no gain) |
| tc            | 5.045666 | 65000 | 1 | 10 | 5.016995 | −0.57% | `helios_T65000_ns` (no gain) |

---
