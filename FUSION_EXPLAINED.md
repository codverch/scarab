# Instruction Fusion in Scarab — From The Ground Up

> Working notes for branch `hpca2027-ideal-store-fusion`.
>
> **Base paper:** Sawan Singh, Arthur Perais, Alexandra Jimborean, Alberto Ros,
> *"Exploring Instruction Fusion Opportunities in General Purpose Processors"*,
> MICRO 2022 — the **Helios** microarchitecture.
>
> **Code under study:** `src/ideal-fusion/` (774 lines of `ideal_fusion.c`), plus
> hooks in `ft.cc`, `map_stage.c`, `map_rename.c`, `map.c`, `node_stage.c`,
> `issue_queue.cc`.

---

## Table of contents

- [Part 0 — Prerequisites: what a core does with an instruction](#part-0)
- [Part 1 — What instruction fusion is](#part-1)
- [Part 2 — Load-load fusion: when we can, when we can't](#part-2)
- [Part 3 — Store-store fusion: when we can, when we can't](#part-3)
- [Part 4 — The ideal-fusion code in this repo, line by line](#part-4)
- [Part 5 — What store-store fusion will need](#part-5)
- [Glossary](#glossary)

---

<a name="part-0"></a>
## Part 0 — Prerequisites: what a core does with an instruction

Skip this if you already think in ROB/IQ/LSQ terms. Everything later depends on
knowing exactly *which resource* fusion saves, so it is worth being precise.

### 0.1 Cracking: architectural instruction → μ-ops

The instructions in the binary are **architectural instructions** (what the ISA
manual documents). Modern cores do not execute those directly. The decoder
**cracks** each one into one or more **μ-ops** (micro-operations) — simple,
fixed-format internal operations the backend knows how to handle.

```
x86:  add [rax], rbx      →  μ1: load  t0 ← [rax]
                             μ2: add   t1 ← t0, rbx
                             μ3: store [rax] ← t1
```

After decode, **everything in flight is a μ-op**. Fusion is the *dual* of
cracking: cracking splits one too-complex instruction into several simple μ-ops;
fusion merges several too-simple μ-ops into one that is "just complex enough".

### 0.2 The pipeline stages that matter

```
Fetch → Decode → Rename → Dispatch/Allocate → Issue → Execute → Writeback → Commit
                    │           │               │                              │
                    │           │               └─ out-of-order, when ready    │
                    │           └─ claims ROB / IQ / LSQ entries               │
                    └─ maps architectural regs → physical regs        in program order
```

Two properties matter for everything below:

- **Execution is out-of-order.** A μ-op issues as soon as its inputs are ready
  and a functional unit is free, regardless of program order.
- **Commit (retire) is strictly in program order.** This is what makes
  speculation recoverable: nothing becomes architecturally visible until it
  retires, and if we mispredicted, we simply throw away everything younger.

### 0.3 The resources an in-flight μ-op holds

This is the heart of why fusion is worth doing. Every μ-op in flight occupies:

| Resource | What it is | Scarab param (default) |
|---|---|---|
| **ROB** entry (Reorder Buffer) | one slot per in-flight μ-op, holds it until in-order retire | `NODE_TABLE_SIZE` = 256 |
| **IQ / RS** entry (Issue Queue / Reservation Station) | where a μ-op waits for its operands before issuing | per-queue, `ISSUE_WIDTH` = 4 |
| **LQ** entry (Load Queue) | one per in-flight load; used to detect load-order violations | `LOAD_QUEUE_ENTRY_NUM` = 128 |
| **SQ** entry (Store Queue) | one per in-flight store; holds address+data until commit | `STORE_QUEUE_ENTRY_NUM` = 72 |
| **Physical register** | the renamed destination | physical register file |
| **Cache port** | one D-cache access | `DCACHE_LINE_SIZE` = 64 B |

When any of these fills up, the frontend **stalls** — the machine stops fetching
new work even though the functional units may be idle. That stall is the thing
fusion attacks. If two μ-ops become one, you free one ROB entry, one IQ entry,
one LQ/SQ entry, and one cache access.

### 0.4 Speculation, on-path and off-path

The core predicts branches and keeps fetching down the predicted path. Scarab
calls μ-ops on the correctly-predicted path **on-path** and μ-ops on a
wrong-path **off-path** (`op->off_path`). Off-path μ-ops eventually get
**flushed** (squashed) when the misprediction resolves. The ideal-fusion code
only ever considers on-path μ-ops.

---

<a name="part-1"></a>
## Part 1 — What instruction fusion is

### 1.1 Architectural vs. microarchitectural fusion

**Architectural fusion** means the ISA provides a single instruction that does
the work of two. Armv8 has `ldp` (load pair) and `stp` (store pair): one
instruction that loads/stores two registers' worth of contiguous memory.

That approach has costs (paper, §III-C):
- It requires the two accesses to be **exactly contiguous**.
- It requires the two accesses to be **the same size**.
- Every compliant implementation must support it forever — ISA bloat, more
  design and validation work.

**Microarchitectural fusion** means the ISA stays simple (RISC-V, x86 have no
`ldp`) and the *hardware* merges pairs of ordinary μ-ops on the fly. This is
strictly more powerful, because the cache circuit's access granularity is
typically a whole 64-byte line, so the hardware can fuse accesses that are:
- **overlapping** (share bytes),
- **asymmetric** (different sizes),
- **near but not adjacent** (any two accesses inside one line).

None of those are expressible as `ldp`. This is what Helios — and this repo —
study.

### 1.2 The taxonomy (paper §II-A) — learn these four terms

The paper borrows nuclear-fusion vocabulary. You will see these names all over
the code and the paper:

- **Head nucleus** — the **older** μ-op (in program order) of the pair.
  In this repo's code it is called **LOAD1**.
- **Tail nucleus** — the **younger** μ-op of the pair.
  In this repo it is **LOAD2**.
- **Catalyst** — the μ-ops sitting *in between* the head and tail nucleus in
  program order. Empty if the two are back-to-back.
- **Fused μ-op** — the single μ-op that replaces the pair. Convention: the fused
  μ-op takes the **head nucleus's position**; the tail nucleus disappears.

```
program order ──────────────────────────────────────────────▶

   ld x4, 0(x2)       ◀── head nucleus  (LOAD1)
   add x7, x8, x9     ┐
   sub x10, x11, x12  ├── catalyst
   or  x13, x1, x3    ┘
   ld x5, 8(x2)       ◀── tail nucleus  (LOAD2)

                 ↓ fuse ↓

   ldp x4, x5, 0(x2)  ◀── one fused μ-op: one ROB entry, one LQ entry,
                          one IQ entry, ONE cache access, two destinations
```

### 1.3 The four fusion categories

Two independent axes, giving four combinations:

| | **Contiguous (CTF)** | **Non-contiguous (NCTF)** |
|---|---|---|
| **Consecutive (CSF)** | the classic `ldp` case — commercially shipping today | two adjacent loads anywhere in the same line |
| **Non-consecutive (NCSF)** | pair separated by a catalyst, but contiguous | **Helios's target**: separated *and* anywhere in the line |

- **ConSecutive Fusion (CSF)** — the two μ-ops are back-to-back in the dynamic
  instruction stream (empty catalyst).
- **Non-ConSecutive Fusion (NCSF)** — there is a catalyst between them.
- **ConTiguous Fusion (CTF)** — the two accesses touch adjacent,
  non-overlapping bytes.
- **Non-ConTiguous Fusion (NCTF)** — the accesses are not adjacent, but still
  land inside one cache-line-sized region.

Everything commercially shipping today is CSF+CTF only. The paper's whole
contribution is showing NCSF and NCTF are worth a lot more:
- RISC-V idiom fusion alone: **+7%** performance.
- Memory-only fusion captures **86%** of total fusion potential.
- Helios (NCSF + NCTF) fuses an extra **5.5%** of dynamic instructions →
  **+14.2%** over no fusion, **+8.2%** over baseline CSF+CTF fusion.

### 1.4 The fusion window

Fusion has to happen **before Rename** (once registers are renamed and entries
allocated, it is too late to collapse two μ-ops into one). So the two candidates
must be simultaneously visible in some frontend structure — a decode group, or
an Allocation Queue (AQ) placed between Decode and Rename. That structure's
size is the **fusion window**, and it bounds how far apart head and tail can be.

In this repo the window is the parameter `IDEAL_FUSION_DISTANCE` (default
**512** μ-ops), which is deliberately much larger than any real AQ — because
this is an *idealized* study measuring an upper bound.

---

<a name="part-2"></a>
## Part 2 — Load-load fusion: when we can, when we can't

### 2.1 The idea

Two loads that read data inside the same 64-byte cache line can be served by
**one** cache access. The line comes back once; the two requested byte ranges
are extracted from it and written to two destination registers.

The saving is real and multi-dimensional:
- one D-cache access instead of two (cache bandwidth + power),
- one ROB entry instead of two,
- one LQ entry instead of two,
- one IQ entry instead of two,
- the tail load's **latency effectively becomes zero** — it gets its data at the
  same moment the head load does.

### 2.2 The baseline (static) fusion predicate

Classic hardware decides using only **static** information — register names,
immediates, and sizes visible at decode. For RISC-V load pair:

```
fuse(op0, op1) =  (op0 == ld) ∧ (op1 == ld)
                ∧ (breg0 == breg1)                    // same base register
                ∧ (mem_size0 == mem_size1)            // same access size
                ∧ (|imm0 − imm1| == mem_size0)        // exactly adjacent
```

### 2.3 When we CAN fuse two loads

All of the following must hold:

1. **Both are loads.** (Never mix a load and a store.)
2. **Both effective addresses land inside one cache-access granule** — one
   64-byte line here. This is the *microarchitectural* relaxation of "exactly
   contiguous": overlapping, asymmetric, and merely-nearby accesses all qualify.
3. **Neither access itself crosses a line boundary.** (See 2.4.1.)
4. **The tail does not depend on the head**, directly or through the catalyst.
   Both addresses must be computable independently so the pair can issue as one.
5. **The head nucleus is not already a fused μ-op.** (No 3-way chains — this
   work considers only 2-μop fusion.)
6. **They are close enough** to be co-resident in the fusion window.
7. **The hardware can retire two destination registers from one μ-op.** Fused
   memory μ-ops need 3 source and 2 destination registers, which the paper takes
   as a given from baseline CSF+CTF fusion support.

### 2.4 When we CANNOT fuse two loads

#### 2.4.1 Cacheline crossers

```asm
ld x4, 0(x1)
ld x5, 8(x1)
```

This is a textbook load-pair idiom. But **static information cannot prove both
land in the same line**. If `x1` happens to be `0x...38`, the first load takes
bytes 0x38–0x3F and the second takes 0x40–0x47 — two different lines.

The fused μ-op then needs **two serialized cache accesses**, so it does not
reduce latency. It is still *correct* (the hardware already handles single
accesses that straddle two lines), just not profitable. On AMD the penalty is
about one cycle (paper cites [1], §2.6.2).

Consequence noted in the paper: for load-pair fusion to behave optimally, **the
two destination registers must be delivered to dependents independently**, so a
consumer of the first half is not stalled waiting for the second half's line.

#### 2.4.2 Dependent loads

```asm
ld x1, 0(x1)     ← produces x1
ld x5, 0(x1)     ← consumes x1
```

Superficially a perfect idiom: same base register, same size. But the second
load's *address* depends on the first load's *result*. They can never compute
their effective addresses in the same cycle, so they cannot share a cache
access. **Not fusible.**

Generalized: fusion is illegal if there is a **RaW (read-after-write) register
dependency** from the head nucleus to the tail nucleus, or from anything in the
**catalyst** to the tail nucleus. The latter is subtle and NCSF-specific: if a
catalyst μ-op overwrites the tail's base register, the fused μ-op captured the
*wrong* physical register name at fusion time. Helios detects this at Rename and
**unfuses** (repair Case 1).

#### 2.4.3 Different base registers (DBR)

There exist load pairs that access the same cache line through **different**
base registers. Statically the hardware cannot tell — it would have to know the
effective addresses, which are not available until execute.

The paper measures this: DBR pairs are ~**1.5%** of dynamic μ-ops. Real potential
left on the table by any purely static scheme. Helios recovers it with a
**predictor** (see §4 of the paper) rather than static matching.

#### 2.4.4 The head is already fused

Only 2-μop fusion is considered. A μ-op that is already a tail nucleus cannot
become a head nucleus of another pair.

#### 2.4.5 Distance beyond the fusion window

If the head has already left the Allocation Queue by the time the tail decodes,
there is nothing to fuse with.

#### 2.4.6 An intervening store to the same line *(value correctness)*

If a store between the two loads writes bytes the tail load reads, the two loads
would legitimately observe **different values**. Serving both from one access
returns stale data to the tail.

Note this is a *value* hazard, not an ordering hazard, and a real machine's
store-to-load forwarding handles it. This repo's ideal model takes the
conservative route and simply drops the candidate
(`invalidate_loads_for_store()`, see §4.5).

### 2.5 Why loads are the forgiving case

This one paragraph is the key to Part 3, so it is worth stating plainly.

**A load is a query; it has no side effects.** Executing a load early, late,
twice, or speculatively changes nothing architecturally. Modern cores therefore
*already* execute loads aggressively out of order, and already carry the
machinery to clean up when that goes wrong:
- out-of-order w.r.t. **other loads**, with load-order violation detection via
  LQ snooping,
- out-of-order w.r.t. **stores**, with store-to-load forwarding, memory
  dependence prediction, and squash-on-violation.

Because the recovery machinery exists, the paper can say (§IV-B4) that NCS
**load** pair fusion may have *loads and stores in the catalyst* — no new
correctness problem is introduced.

**Load misspeculation is recoverable. That is the whole reason loads are easy.**

---

<a name="part-3"></a>
## Part 3 — Store-store fusion: when we can, when we can't

### 3.1 Why stores are fundamentally harder

**A store is a mutation.** Once a store becomes visible to other cores, you
cannot take it back. There is no "squash" for a value another core already read.

And crucially, the *order* in which stores become visible is not a private
implementation detail — it is a **contract** with software, defined by the
**memory consistency model**. Fusing two stores changes that order. That is the
entire difficulty.

### 3.2 Memory consistency, from scratch

You need exactly four ideas.

**(a) Program order vs. visibility order.** Program order is the order
instructions appear in the thread. Visibility (or *coherence*) order is the
order in which other cores can observe the writes. A consistency model is a set
of rules constraining how much these two may differ.

**(b) The store buffer.** Stores do not write the cache when they execute. They
compute their address and data, park in the **store queue / store buffer**, and
only **drain to the cache at (or after) retirement**. Since retirement is in
program order, stores naturally become visible in program order — unless the
hardware deliberately reorders them.

**(c) Store→store ordering.** Some models require stores to become visible in
program order:
- **TSO** (Total Store Order — x86, SPARC, and **the model this paper
  simulates**, §V-A): store→store ordering is **guaranteed**.
- **Weak models** (ARMv8, RISC-V RVWMO): store→store ordering is **not**
  guaranteed in general; you need an explicit fence.

**(d) Same-address sequential consistency (coherence).** This one is
**universal — every memory model, even the weakest, guarantees it**: writes to
*the same address* appear in program order. There is a single global order of
writes to each location, consistent with each thread's program order. Break
this and you have broken literally every ISA.

### 3.3 What fusing two stores actually does

```
program order:

   ST_A  → writes bytes at X     ◀── head nucleus (STORE1)
   ...catalyst...
   ST_C  → writes bytes at Z     ◀── tail nucleus (STORE2)
```

Fusing them makes ST_A and ST_C become visible **at the same instant, as one
event**, at the head nucleus's position. Two things change:

1. They become **atomic** with respect to each other. (Harmless — this is
   *strengthening*, and strengthening ordering is always safe.)
2. ST_C has **moved earlier in visibility order**, jumping over everything in
   the catalyst. (**This is the dangerous part** — this is *weakening*.)

> **Rule of thumb:** making ordering stronger is always safe; making it weaker
> is what breaks programs. Fusion does both at once, and only the second one
> matters.

### 3.4 The killer case: a store in the catalyst

```
   ST_A   [line L + 0]   = 1      ◀── STORE1
   ST_B   [line L + 8]   = 2      ◀── catalyst store
   ST_C   [line L + 16]  = 3      ◀── STORE2
```

Fuse ST_A and ST_C. Now another core observing memory sees `{A, C}` become
visible together, and `B` either before or after. Program order said
`A → B → C`. We have produced `A,C → B` or `B → A,C`.

- Under **TSO** this is a flat **store→store ordering violation**. Illegal.
- Even under the **weakest** model, if ST_B overlaps any byte written by ST_A or
  ST_C, we have violated **same-address sequential consistency**. Illegal
  everywhere.

The paper states exactly this (§IV-B4):

> *"Helios cannot guarantee that i) There is no store μ-op in the catalyst of a
> store pair NCSF'd μ-op and ii) If there is such a store μ-op, it cannot
> guarantee that it does not overlap with the tail nucleus. As a result an
> NCSF'd store pair μ-op risks violating store-store ordering in models
> enforcing it and same-address sequential-consistency for all memory models."*

**And here is why there is no escape hatch:** for loads, the fix is "speculate,
detect, squash." For stores that is not available — by the time you could detect
the problem, the write is already globally visible. You cannot un-ring the bell.
So the only option is to **not fuse**.

> ### The one-line rule
> **Two stores may be fused only if there is no other store between them in
> program order.**

### 3.5 When we CAN fuse two stores

All the load conditions from §2.3, **plus**:

1. **Zero stores in the catalyst.** The rule above.
2. **No fence / serializing instruction in the catalyst.** A fence exists
   precisely to forbid the reordering fusion would perform.
3. **Byte ranges handled correctly on overlap.** If the two stores write
   overlapping bytes, the **younger** store's bytes must win in the merge. The
   simplest safe policy — and the one I would use for an ideal study — is to
   fuse only when the byte ranges are **disjoint**. The paper's Figure 4 shows
   overlapping pairs are rare, so little is lost.
4. **Same base register (SBR).** See 3.6.4 — Helios does not support the
   different-base-register case for stores.

### 3.6 When we CANNOT fuse two stores

#### 3.6.1 Any store in the catalyst
Covered above. This is *the* constraint, and it has no load analogue.

#### 3.6.2 A fence or serializing instruction in the catalyst
Helios tracks this with an **NCSF Serializing bit** at Rename; a tail nucleus
seeing it set triggers unfuse (repair Case 4).

#### 3.6.3 A fault on the tail nucleus
If STORE2 page-faults but STORE1 does not, you must take a **precise exception**
at STORE2's program point — with STORE1's write applied and STORE2's not.
A fused μ-op has already merged them. Helios unfuses (repair Case 6).

#### 3.6.4 Different base registers (DBR) — a register-port limit, not a correctness one

Count the inputs of a fused store pair:

| | sources needed |
|---|---|
| SBR store pair | base + data1 + data2 = **3** |
| DBR store pair | base1 + base2 + data1 + data2 = **4** |

Baseline CSF+CTF memory fusion already provides **3 source** and 2 destination
registers. A DBR store pair would need a **4th source register**, which means
widening the register file read ports and every pipeline latch that carries
source IDs — expensive.

The paper measures the payoff and finds DBR store pairs are only **0.54%** of
fused stores, so:

> *"we only support SBR store pair fusion."*

Note the contrast: DBR **load** pair fusion *is* supported, recovered via the
predictor, because a load pair needs fewer sources.

#### 3.6.5 The catalyst-load subtlety (worth getting right in any implementation)

Consider a **load** in the catalyst that reads bytes STORE2 will write:

```
   ST_A   [L+0]  = 1      ◀── STORE1
   LD     [L+16]          ◀── catalyst load, reads STORE2's bytes
   ST_C   [L+16] = 3      ◀── STORE2
```

The fused store pair sits at ST_A's position and occupies **one** store-queue
entry covering *both* byte ranges. A naive store-to-load-forwarding check would
match that entry and forward **STORE2's data (3)** to the load — but
architecturally STORE2 has not executed yet, so the load must see the *old*
value.

This is not a flaw in the paper; it is a detail the LSQ design must handle. The
paper does specify that LQ/SQ entries carry *"an offset from the base address
for the second access"* and *"the access size of the second access"*
(§IV-B6) — exactly the per-half metadata forwarding logic needs to answer "which
half of this fused entry is architecturally live for a load at this age?"

**For our ideal study the safe simplification is to drop the pair if any
catalyst load touches the candidate's cache line**, and make that a parameter so
we can also measure the aggressive upper bound.

#### 3.6.6 Everything from Part 2 still applies
Cacheline crossing, address dependence through the catalyst, head already
fused, window distance — all unchanged.

### 3.7 How Helios enforces the store rule in hardware

Two concrete mechanisms, both worth copying:

**(a) A single-entry store UCH.** The **UCH** (Unfused Committed History) records
cache lines touched by recently committed, not-yet-fused memory μ-ops, so a
retiring μ-op can discover a fusible partner. Helios sizes it asymmetrically:

| | UCH organization |
|---|---|
| **Loads** | 6-entry, fully associative, LRU |
| **Stores** | **1 entry** — the last unfused committed store |

The paper's reason is stated directly: *"stores cannot be fused across other
stores to prevent memory consistency issues."* The single entry **structurally
enforces** the rule — you can only ever pair with the immediately preceding
unfused store, so a catalyst store is impossible by construction.

**(b) The `NCSF_StorePair` bit at Rename.** Set when *any* store μ-op other than
the first head nucleus of the NCSF nest is renamed. Any **store tail nucleus**
that sees this bit set **unfuses** the pending fused μ-op waiting in the IQ
(repair Case 3). This catches the speculative case the frontend could not rule
out.

### 3.8 Consequence: store fusion yields less than load fusion

This is expected, not a bug. From the paper's own measurements at commit:
**0.28 loads/cycle** search the UCH versus **0.13 and 0.16 per cycle for
stores**. Between the no-catalyst-store rule and SBR-only, the opportunity is
structurally much narrower. Any store-fusion result should be read against that
ceiling.

---

<a name="part-4"></a>
## Part 4 — The ideal-fusion code in this repo, line by line

### 4.1 What "ideal" means here

This is an **oracle / limit study**, not a hardware proposal. It answers
*"if fusion worked perfectly, how much would we gain?"* — an **upper bound** to
decide whether building real hardware is worth it.

Specifically it idealizes away:
- the **fusion predictor** (Helios's TAGE-like structure) — pass 1 has oracle
  knowledge of true effective addresses, so it never mispredicts;
- the **fusion window** — `IDEAL_FUSION_DISTANCE` defaults to **512** μ-ops,
  far larger than a real AQ;
- **all repair/unfuse costs** — nothing ever has to unfuse.

### 4.2 File map

| File | Role |
|---|---|
| `src/ideal-fusion/ideal_fusion.c` | all the logic (774 lines) |
| `src/ideal-fusion/ideal_fusion.h` | public API + `Load2BufferEntry` |
| `src/ideal-fusion/ideal_fusion.stat.def` | stat counters |
| `src/general.param.def:117-124` | the four parameters |
| `src/op.h:81-85, 250-254` | per-μ-op fusion fields |
| `src/ft.cc:161-164` | **pass 1 entry point** (called at fetch) |
| `src/map_stage.c:272` | `ideal_fusion_on_map()` |
| `src/map.c:560-561` | `ideal_fusion_on_load1_wake()` |
| `src/node_stage.c` (many sites) | ROB / LSQ / retire accounting |
| `src/map_rename.c:487, 687` | register-consumer accounting |
| `src/issue_queue.cc:673` | IQ/RS dispatch skip |

### 4.3 Parameters (`general.param.def`)

```c
DEF_PARAM( ideal_fusion_pass     , IDEAL_FUSION_PASS     , uns  , uns   , 1   , )
DEF_PARAM( ideal_fusion_distance , IDEAL_FUSION_DISTANCE , uns  , uns   , 512 , )
DEF_PARAM( ideal_fusion_log      , IDEAL_FUSION_LOG      , char*, string, "ideal_fusion_candidates.csv", )
DEF_PARAM( ideal_fusion_type     , IDEAL_FUSION_TYPE     , int  , ideal_fusion_type, 0, )
```

- **`ideal_fusion_pass`** — `1` = discover pairs, `2` = apply fusion,
  `3` = measurement mode (§4.7).
- **`ideal_fusion_distance`** — max μ-op distance between head and tail; also
  the candidate-eviction horizon.
- **`ideal_fusion_log`** — the CSV that carries pairs from pass 1 to pass 2.
- **`ideal_fusion_type`** — tie-break policy, `"oldest-first"` (0) or
  `"most-recent"` (1); see §4.5.

### 4.4 Per-μ-op state (`op.h`)

```c
typedef enum Ideal_Fusion_Load_Role_enum {
  IDEAL_FUSION_NOT_CANDIDATE,
  IDEAL_FUSION_LOAD1,        // head nucleus
  IDEAL_FUSION_LOAD2,        // tail nucleus
} Ideal_Fusion_Load_Role;

Counter                 ideal_fusion_micro_op_num;          // dynamic on-path sequence number
Ideal_Fusion_Load_Role  ideal_fusion_load_role;
Counter                 ideal_fusion_partner_micro_op_num;  // the other half
```

**`ideal_fusion_micro_op_num` is the linchpin of the whole design.** Assigned in
`ideal_fusion_on_fetch_op()`:

```c
op->ideal_fusion_micro_op_num = ++next_on_path_micro_op_num;   // ideal_fusion.c
```

It counts **only on-path μ-ops** — off-path μ-ops never advance it. That makes it
a stable, reproducible name for a dynamic μ-op across two separate simulation
runs, which is what lets pass 2 recognize the exact pairs pass 1 found.

### 4.5 Pass 1 — discovery

Driven from `ft.cc:164`, once per fetched μ-op.

**Data structure.** A hash table of load candidates bucketed by **cache block
address**, so matching inspects only earlier loads from the same line instead of
scanning all history:

```c
#define IDEAL_FUSION_LOAD_CANDIDATE_BUCKETS 4096

typedef struct Ideal_Fusion_Load_Candidate_struct {
  Addr    pc, virtual_addr, cache_block_addr, cache_block_offset;
  uns     mem_size;
  Counter micro_op_num;
  Flag    fused;                                   // already claimed as a LOAD1
  struct Ideal_Fusion_Load_Candidate_struct* next; // chaining
} Ideal_Fusion_Load_Candidate;
```

**The algorithm**, per fetched on-path μ-op:

```
1. micro_op_num = ++next_on_path_micro_op_num

2. every IDEAL_FUSION_DISTANCE μ-ops → cleanup_stale_loads()
      free candidates that are already fused OR older than the window

3. if μ-op is a STORE  → invalidate_loads_for_store(); return
      drop EVERY candidate in the store's cache block (conservative)

4. if μ-op is a LOAD   → load1 = find_matching_load1(op)
      if found:  log_matched_pair(load1, op);  load1->fused = TRUE
      else:      track_load(op)          // becomes a future LOAD1 candidate
```

**The match predicate** (`load1_matches_load2`) — this is §2.3 in code:

```c
return load1->cache_block_addr == cache_block_addr &&          // same 64B line
       !load1->fused &&                                        // not already claimed
       access_fits_in_cache_block(load1->virtual_addr, load1->mem_size) &&
       access_fits_in_cache_block(load2->oracle_info.va,
                                  load2->oracle_info.mem_size) &&  // no line crossers
       load2->ideal_fusion_micro_op_num - load1->micro_op_num
                                       < IDEAL_FUSION_DISTANCE;    // window
```

Plus, from `find_matching_load1` / `track_load`, a candidate must be `MEM_LD`,
have `num_dest_regs != 0`, and have a valid `va` and non-zero `mem_size`.

**The tie-break policy.** Several candidates in the same line may qualify.
`IDEAL_FUSION_TYPE` picks:
- `oldest-first` (0) — smallest `micro_op_num`. Frees the oldest ROB entry
  soonest.
- `most-recent` (1) — largest `micro_op_num`. Shortest distance, so most likely
  to be feasible in real hardware.

**One pair per load.** Note step 4: a load that *becomes* a LOAD2 is **not**
tracked as a future candidate, and a claimed LOAD1 is marked `fused`. So every
load participates in **at most one** pair — matching the paper's 2-μop-only
scope. No chains.

**Output.** An 11-field CSV per pair:

```
load1_pc,load1_data_addr,load1_block_offset,load1_mem_size,load1_micro_op_num,
load2_pc,load2_data_addr,load2_block_offset,load2_mem_size,load2_micro_op_num,
micro_op_distance
```

Addresses and sizes are logged (not just sequence numbers) so pass 2 can
**re-validate** every row and so the log stays debuggable.

### 4.6 Pass 2 — actuation

**Loading.** `load_pair_indexes()` reads the CSV once and builds **two** hash
tables keyed on `micro_op_num` — `load1_pair_index` and `load2_pair_index` —
because a μ-op arriving at fetch knows only its own sequence number and must be
able to ask "am I a head? am I a tail?" in O(1).

It re-validates every row (same block, offsets consistent, both fit in the line,
`load2 > load1`, distance matches) and **`exit(EXIT_FAILURE)` on any
inconsistency** — a good design choice; a silently-corrupt pair file would
produce quietly wrong results. It also re-applies the distance filter here, so
**one pass-1 log can be replayed at several window sizes** without re-running
pass 1.

**Classification.** At fetch, `classify_load()` tags the μ-op `IDEAL_FUSION_LOAD1`
or `IDEAL_FUSION_LOAD2` and records the partner's sequence number.

**What LOAD2 becomes.** The model is: **the tail nucleus is a NOP that consumes
no resources and completes exactly when the head does.** Every hook implements
one facet of that:

| File:line | Effect being modeled |
|---|---|
| `node_stage.c:369-382` | **no LSQ entry** — `lsq_available`/`lsq_dispatch` skipped |
| `node_stage.c:414-416` | **no ROB entry** — `node->node_count++` skipped |
| `node_stage.c:426-433` | marked `OS_DONE` + `precommitted` immediately at ROB fill |
| `issue_queue.cc:673` | **no IQ/RS entry** — skipped in `dispatch()` |
| `map_rename.c:487` | source-register release check skipped |
| `map_rename.c:687` | not counted as an on-path register consumer |
| `node_stage.c:588-592` | `lsq_commit()` skipped at retire |
| `node_stage.c:604-605` | `node_count--` skipped at retire (symmetry with fill) |
| `node_stage.c:230,247,285,665` | excluded from ROB occupancy accounting and flush counts |

Taken together: LOAD2 occupies **no ROB entry, no LQ entry, no IQ entry**, issues
**no cache access**, and costs **no extra latency**. That is the "ideal" in ideal
fusion.

**The Load2 buffer — coordinating the pair.** The two halves reach the pipeline
at different times, so a small side structure rendezvouses them
(`LOAD2_BUFFER_HT_SIZE = 1000003`, hashed on the **LOAD1** sequence number):

```c
typedef struct Load2BufferEntry {
  Op*     load2;
  Counter load2_unique_num;    // stale-pointer guard
  Flag    load2_waiting, load1_completed, pair_completed;
  Counter load1_wake_cycle, load1_done_cycle;
  Counter load1_micro_op_num, load2_micro_op_num;
} Load2BufferEntry;
```

Two entry points, and either may fire first:

- **`ideal_fusion_on_map(op, wake_action)`** (from `map_stage.c:272`) — when
  LOAD2 is renamed. If LOAD1 already completed, finish LOAD2 immediately;
  otherwise park it with `load2_waiting = TRUE`.
- **`ideal_fusion_on_load1_wake(load1, wake_action)`** (from `map.c:560`) — when
  LOAD1's data comes back. Record its cycles; if LOAD2 is parked, complete it.

Completion is the crux — the tail *inherits the head's timing*:

```c
static void ideal_fusion_complete_load2(Op* load2, Counter load1_wake_cycle,
                                        Counter load1_done_cycle,
                                        void (*wake_action)(Op*, Op*, uns)) {
  load2->wake_cycle = load1_wake_cycle;
  load2->done_cycle = load1_done_cycle;
  wake_up_ops(load2, REG_DATA_DEP, wake_action);   // release its dependents
}
```

That `wake_up_ops` is essential: LOAD2 never executes, so **something** must wake
its dependents or the machine deadlocks. (The git history confirms this was the
hard part — *"fix pass-2 deadlock with I-Fuse Load2 buffer wake model"*,
*"fix dispatch loop abandoning ops after LOAD2 NOPs"*.)

The `op_pool_valid` / `unique_num` re-check before completing guards against the
`Op*` having been recycled from the op pool after a squash — a real hazard, since
the buffer holds a raw pointer across cycles.

### 4.7 Pass 3 — measurement mode

`IDEAL_FUSION_PASS == 3` **fuses nothing**. It loads the same pairs pass 2 would
fuse, lets both loads execute normally, and logs each one's real `wake_cycle` /
`done_cycle` to `IDEAL_FUSION_MEASURE_OUT`.

Its purpose is to **validate the central assumption of pass 2** — that the older
LOAD1 actually completes before the younger LOAD2 would have. If LOAD2 often
finishes *first* in the unfused machine, then giving it LOAD1's timing makes it
*slower*, and pass 2's speedup is overstated. This is a genuinely good piece of
experimental hygiene.

### 4.8 Statistics (`ideal_fusion.stat.def`)

```c
IDEAL_FUSION_LOAD1_TAGGED / LOAD2_TAGGED   // pairs recognized at fetch
IDEAL_FUSION_LOAD2_BYPASSED                // LOAD2s that skipped the pipeline
IDEAL_FUSION_FUSED_LOADS                   // fused LOAD2s that RETIRED (on-path)
IDEAL_FUSION_LOADS_PARTICIPATED            // = FUSED_LOADS x 2
ONPATH_MEM_LOADS                           // denominator for fusion coverage
LD_EXEC_MINUS_FETCH_LATENCY                // load latency accumulators
LD_RETIRE_MINUS_FETCH_LATENCY
DCACHE_ACCESS_ONPATH / OFFPATH             // cache accesses saved
```

Counting the **retired** LOAD2s rather than the tagged ones is the right call —
tagged counts include μ-ops that were later squashed.

### 4.9 How to run it

```bash
# Pass 1 — discover pairs
./scarab --ideal_fusion_pass 1 --ideal_fusion_distance 512 \
         --ideal_fusion_log pairs.csv --ideal_fusion_type oldest-first  ...

# Pass 2 — apply fusion, same trace, same log
./scarab --ideal_fusion_pass 2 --ideal_fusion_distance 512 \
         --ideal_fusion_log pairs.csv  ...

# Pass 3 — validate the timing assumption
IDEAL_FUSION_MEASURE_OUT=measure.csv \
./scarab --ideal_fusion_pass 3 --ideal_fusion_log pairs.csv  ...
```

Both passes must use the **same trace and same simulation region**, or the
`micro_op_num` sequence will not line up.

---

<a name="part-5"></a>
## Part 5 — What store-store fusion will need

A sketch of the delta, to be built on `hpca2027-ideal-store-fusion`.

### 5.1 Pass 1 — a *different* candidate structure, not a copy

The load path keeps a 4096-bucket hash of many live candidates. Stores must
**not** do that, because §3.4 forbids fusing across another store. The faithful
model is Helios's single-entry St-UCH:

```
one slot:  pending_store1

on STORE:
    if pending_store1 matches (same line, disjoint bytes, within distance):
        log pair;  clear slot            // pair consumed
    else:
        replace slot with this store     // NO fusion across stores
on FENCE / serializing op:
    clear slot
on LOAD touching pending_store1's line   (strict mode):
    clear slot                           // §3.6.5 forwarding hazard
```

Because the slot is *replaced* by every store, a catalyst store is impossible by
construction — the same structural guarantee the hardware gets.

### 5.2 New parameters

| Param | Purpose |
|---|---|
| `ideal_fusion_class` | `0` = loads (today's behavior), `1` = stores |
| `ideal_fusion_store_strict` | `TRUE` = drop pair on an aliasing catalyst load (§3.6.5); `FALSE` = aggressive upper bound |

### 5.3 Pass 2 — mostly reusable, with two real differences

The `IDEAL_FUSION_LOAD1/LOAD2` role enum extends to `STORE1/STORE2`, and every
`ideal_fusion_load2_is_nop()` hook site generalizes to
`ideal_fusion_tail_is_nop()` — all ten sites mean "this is a fused tail nucleus,
it consumes no resources", which is equally true of STORE2.

Two things genuinely differ:

1. **Memory dependences must survive.** `map_mem_dep()` runs at
   `map_stage.c:263`, *before* the fusion hook at `:272`, so STORE2 still lands
   in the store hash and younger loads keep their dependences. Good — leave it
   alone.
2. **Wake the right dependence types.** A load wakes `REG_DATA_DEP`. A store's
   consumers wait on `MEM_ADDR_DEP` and `MEM_DATA_DEP` (see
   `exec_stage.c:466-472`). A NOP'd STORE2 must wake **all three**, or younger
   dependent loads hang — the same deadlock class the load path already hit.

Also: STORE2 frees an **SQ** entry rather than an LQ entry, and the SQ is the
smaller structure (72 vs 128), so relieving it may matter more per fusion.

### 5.4 Stats worth adding

Beyond the STORE mirrors of §4.8, the *kill reasons* are the interesting science
— they quantify how much the consistency rule actually costs:

```
IDEAL_FUSION_STORE_CAND_KILLED_BY_STORE   // the §3.4 rule
IDEAL_FUSION_STORE_CAND_KILLED_BY_LOAD    // the §3.6.5 rule
IDEAL_FUSION_STORE_CAND_KILLED_BY_FENCE
IDEAL_FUSION_STORE_CAND_KILLED_BY_DISTANCE
```

Expect store fusion to be considerably rarer than load fusion (§3.8). That is
the result, not a bug.

---

<a name="glossary"></a>
## Glossary

| Term | Meaning |
|---|---|
| **μ-op** | micro-operation; the simple internal op the backend executes |
| **Cracking** | splitting one architectural instruction into several μ-ops |
| **Fusion** | the dual: merging several μ-ops into one |
| **Head nucleus** | the older μ-op of a fused pair (`LOAD1` in this code) |
| **Tail nucleus** | the younger μ-op of a fused pair (`LOAD2`) |
| **Catalyst** | the μ-ops between head and tail in program order |
| **CSF / NCSF** | ConSecutive / Non-ConSecutive Fusion (empty / non-empty catalyst) |
| **CTF / NCTF** | ConTiguous / Non-ConTiguous Fusion (adjacent bytes or not) |
| **SBR / DBR** | Same / Different Base Register |
| **ROB** | Reorder Buffer — holds in-flight μ-ops until in-order retire (`NODE_TABLE_SIZE`=256) |
| **IQ / RS** | Issue Queue / Reservation Station — where μ-ops await operands |
| **LQ / SQ** | Load Queue / Store Queue (128 / 72 entries) |
| **LSQ** | the two together |
| **AQ** | Allocation Queue — frontend buffer between Decode and Rename; the fusion window |
| **UCH** | Unfused Committed History — Helios structure recording recent unfused committed memory μ-ops (6 entries for loads, **1** for stores) |
| **FP** | Fusion Predictor — Helios's TAGE-like predictor of head↔tail distance |
| **RaW** | Read-after-Write — a true data dependence |
| **STLDF** | Store-to-load forwarding |
| **TSO** | Total Store Order — the consistency model simulated in the paper |
| **Same-address SC** | writes to one address appear in program order; guaranteed by *every* memory model |
| **On-path / off-path** | correctly-predicted path vs. wrong path (`op->off_path`) |
| **Ideal / oracle study** | limit study using perfect future knowledge, to bound the achievable gain |

---

## Reading list, in order

1. Paper §II-A — the taxonomy. Learn head/tail/catalyst before anything else.
2. Paper §III-D — *Limitations of Microarchitectural Fusion*. This is the
   "when we can't" section.
3. Paper §IV-B4 — *Memory Consistency & Sequential Semantics*. The store rule.
4. `ideal_fusion.c` → `ideal_fusion_on_fetch_op()` — the pass-1 driver.
5. `ideal_fusion.c` → `load1_matches_load2()` — the fusion predicate.
6. `node_stage.c:360-435` — where a tail nucleus stops costing resources.
