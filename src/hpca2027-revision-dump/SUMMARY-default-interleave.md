
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

