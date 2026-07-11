# Register File Prefetching - Scarab Fork

Fork of [litz-lab/scarab](https://github.com/litz-lab/scarab) with an implementation of Register File Prefetching (ISCA 2022).

## About

The main idea behind Register File Prefetching (RFP) is to speed up the processing of load instructions by making use of the time between the Rename and Issue stages in the pipeline to prefetch data from the L1 cache into the Physical Register File (PRF), so that when the load begins execution, we can access the data quickly.

Normally, a load would take multiple cycles (5 cycles in Intel Skylake) to access the L1 for data. Previously proposed methods such as load address prediction in conjunction with load value prediction attempt to speed up this process by predicting load address patterns and speculatively fetching them. However, this method has major penalties on incorrect predictions, resulting in pipeline stalls or flushes. RFP, on the other hand, is designed such that incorrect predictions do not result in penalties.

The RFP implementation in Scarab consists of three sub-units: the RFP prediction table (PT), the RFP FIFO in the LSU, and a PRF tracker.

### RFP Prediction Table (`rfp.c`, `rfp.h`)

The RFP prediction table (PT) is a 1K-entry table based on conventional stride predictors used by prefetchers. When a load retires, it is added to the PT, and the stride predictor attempts to predict its load address on successive retires to build confidence. If the predictor predicts the address correctly on successive attempts, the load is **eligible** for RFP prefetching. Once eligible, the load's address is predicted at fetch and an RFP prefetch packet is launched at rename.

> **Note:** Confidence is a 1-bit entry. It is incremented with 1/16 probability whenever the stride predictor accurately guesses the load address, and reset to 0 on a mispredict.

### RFP PRF Tracker (`rfp_prf.c`, `rfp_prf.h`)

At rename, the load instruction's destination is assigned a PRF register — this is where the load data will be written. In RFP, we prefetch data from the L1 and write it into this PRF register. The PRF tracker holds per-register state for loads, including:

- Validity and predicted address
- Whether the prefetch is inflight
- Whether the prefetch was dropped

### RFP FIFO (`lsq.cc`)

Once the RFP prefetch request is created at rename, it is enqueued into a 64-entry RFP FIFO ordered by age — older requests at the head, younger ones near the tail. RFP requests at the head are dequeued when attempting to access the L1.

The RFP prefetch request is similar to a load request in the load queue, with two differences:

- It carries the destination PRF register the result will be written to
- It is a low-priority request — a demand load of the same age always wins L1 port arbitration first

A given load can therefore have both a conventional load request in the LSQ and an RFP request in the FIFO at the same time. If the demand load is processed before its corresponding RFP request, the RFP request is dropped and the load executes normally.

When an RFP request is dequeued:

- **L1 hit:** data is written into the corresponding PRF register and the PRF tracker entry is marked valid and ready
- **L1 miss:** the RFP request is dropped — RFP hides L1-*hit* latency only. The paper's optional L1-miss-to-memory path is intentionally not modeled (a ~0.02% effect, §5.5.5; the same class of simplification as the DTLB-miss drop), which keeps RFP deadlock-free on every workload.

### Load Execution Stage (`dcache_stage.c`)

While the RFP request is processed in the background, the load moves through the pipeline normally until it is issued and begins execution in the LSU. The first step is calculating the actual load address, then comparing it with the predicted address from the RFP PT:

- **Mismatch:** clear the PRF entry, reset PT confidence, and process the load normally (5 cycles)
- **Match:** check whether the PRF entry is ready
  - **Not ready** (RFP request not processed in time): drop the RFP request and process the load normally (5 cycles)
  - **Ready:** 1-cycle fast access by reading from the PRF Register instead of the 5-cycle L1 path; wake dependents

In the best case, an RFP request saves up to 4 cycles. In the worst case, the load proceeds normally — there is no penalty.

### RFP flow for a given load

1. Train the RFP PT on a given load at retire.
   - Confidence is incremented with 1/16 probability; once confident, the load is RFP-eligible.
2. At rename, look up the load in the RFP PT and get the predicted address from the stride pattern. Create an RFP prefetch request and enqueue it into the RFP FIFO.
3. The RFP prefetch request moves up in the FIFO and is serviced at the head. On completion, data is written into the PRF register and the corresponding PRF tracker entry is marked valid.
4. Meanwhile, the load moves through rename, operand ready, pick, and dispatch. At execution, compare the computed load address with the predicted address:
   - **Mismatch:** drop RFP request; process load normally (5 cycles).
   - **Match:** check if the PRF entry is ready.
     - **Not ready:** drop RFP request; process load normally (5 cycles).
     - **Ready:** read data from the PRF register; 1-cycle fast access; wake dependents.

## Build

1. Install [Intel PIN 3.15](https://www.intel.com/content/www/us/en/developer/articles/tool/pin-a-binary-instrumentation-tool-downloads.html) and set:

```bash
export PIN_ROOT=/path/to/pin-3.15
export SCARAB_ENABLE_PT_MEMTRACE=1
```

2. Build the optimized binary:

```bash
cd src
make opt
```

The simulator binary is `src/scarab`. Use `make dbg` for a debug build.

## Run

Simulations use the memtrace frontend. From a run directory:

1. Copy a params file and rename it:

```bash
cp /path/to/scarab/src/PARAMS.golden_cove ./PARAMS.in
```

2. Run Scarab on a memtrace (`.zip` trace + matching `modules.log` in the trace directory):

```bash
/path/to/scarab/src/scarab \
  --frontend memtrace \
  --fetch_off_path_ops 1 \
  --inst_limit 100000000 \
  --cbp_trace_r0=/path/to/trace.zip
```

RFP is enabled by default. To compare against a no-RFP baseline:

```bash
--rfp_on=0          # disable RFP
--rfp_on=1          # enable RFP (default)
--rfp_model=1       # PRF model (default)
```

See `docs/memtrace.md` and `docs/compiling-scarab.md` for trace capture and additional options.

## Results

A baseline-vs-RFP sweep over 20 datacenter workloads (all SimPoints, no warmup) lives in
[`results/`](results/) — graphs, summary CSVs, the full stats capture, and self-contained plot
scripts. Headline: **+2.85% geomean IPC speedup** (paper +3.1%), 0 deadlocks. See
[`results/README.md`](results/README.md) for per-workload numbers and reproduction steps (the
scarab-infra sweep descriptor `rfp_sweep.json` is committed in the scarab-infra repo at
`json/rfp_sweep.json`).

## References

- Shukla, S., Bandishte, S., Gaur, J., and Subramoney, S. *Register File Prefetching.* ISCA 2022. [PDF](https://dl.acm.org/doi/pdf/10.1145/3470496.3527398)
