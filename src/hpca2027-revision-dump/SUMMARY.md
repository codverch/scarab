
## Default interleave (4 B banks) (speedup over baseline)

| App | ifuse (committed, free LD2 bank) | ifuse-bankport | 1-cycle-delayed-ifuse (committed, free LD2 bank) | 1-cycle-delayed-ifuse-bankport |
|---|---|---|---|---|
| bfs | +18.4% | +19.3% | +16.4% | +16.3% |
| dfs | +20.9% | +20.8% | +17.4% | +17.9% |
| pagerank | +18.9% | +20.5% | +16.8% | +17.4% |
| corebench | +22.9% | +23.0% | +19.1% | +18.8% |
| appworld | +10.1% | +9.5% | +8.5% | +8.5% |
| terminal_bench | +14.8% | +14.5% | +12.3% | +12.2% |
| cachebench | +15.1% | +17.0% | +11.5% | +11.5% |
| clickhouse | +22.9% | +3.7% | +22.1% | +3.2% |
| duckdb | +4.3% | +3.4% | +3.8% | +3.1% |
| leveldb | +17.9% | +15.6% | +15.8% | +12.6% |
| memcached | +7.6% | +7.6% | +7.3% | +7.2% |
| **Geomean** | +15.6% (11 apps) | +13.9% (11 apps) | +13.6% (11 apps) | +11.6% (11 apps) |

## 8 B banks (--dcache_interleave_factor 8) (speedup over baseline-8B)

| App | ifuse-bankport-8B | 1-cycle-delayed-ifuse-bankport-8B | helios-8B | rfp-8B |
|---|---|---|---|---|
| bfs | +20.2% | +16.6% | +2.7% | +1.1% |
| dfs | +21.3% | +18.2% | +2.3% | +1.1% |
| pagerank | +19.1% | +17.4% | +1.8% | +0.6% |
| corebench | +22.8% | +19.0% | +0.5% | +3.4% |
| appworld | +9.5% | +7.9% | -0.0% | +1.6% |
| terminal_bench | +14.2% | +11.7% | +2.7% | +8.2% |
| cachebench | +13.8% | +11.0% | +0.6% | +1.9% |
| clickhouse | +6.1% | +5.4% | +0.3% | +1.0% |
| duckdb | +3.5% | +3.3% | -0.0% | +1.7% |
| leveldb | +16.5% | +14.1% | +0.2% | +3.4% |
| memcached | +7.5% | +7.2% | +0.0% | +0.4% |
| **Geomean** | +13.9% (11 apps) | +11.9% (11 apps) | +1.0% (11 apps) | +2.2% (11 apps) |
