# Results: Intel Core Ultra 9 285 (Arrow Lake)

Machine: Core Ultra 9 285 (8 Lion Cove P-cores = CPUs 0-7, 16 Skymont E-cores
= CPUs 8-23), Arrow Lake-S iGPU with the i915 driver, Linux 7.0.0-34-generic.
All numbers come from one `./run_all.sh` run on 2026-09-25 at 23:18 UTC (raw
output in `results.txt`), P-core = CPU 0, E-core = CPU 8.

"Stale" means the load returned the value the CPU had cached before the GPU
overwrote the line in DRAM. On non-coherent CXL memory, that is a read that
missed another host's write.

## Bottom line

| Sequence | Safe on this CPU? |
|---|---|
| `clflushopt x; load x` | **no**: almost always stale |
| `clflushopt x; sfence; load x` | **no**: almost always stale |
| `clflushopt x; lfence; load x` | **no**: usually fresh, sometimes stale (up to ~40%) |
| `clflushopt x; sfence; lfence; load x` | **no**: up to 76% stale back to back |
| `clflush x; lfence; load x` | **no**: ~30% stale with a store ahead of it |
| `clwb x; <any fence>; load x` | **no**: `clwb` keeps the line, always stale |
| `clflushopt x; mfence; load x` | **yes**: 0 stale in every test |

For a race detector: accept `mfence` (and probably locked RMW or `serialize`,
which haven't been tested) between a flush and a read of the same line. Treat
`lfence`, `sfence`, and `sfence; lfence` as insufficient.

## Controls: the setup works

| Check | Result |
|---|---|
| Plain load, no flush (PAT 2, non-coherent) | 100.00% stale (1 fresh in 27,305 on the P-core) |
| Every sequence with PAT 3 (GPU snoops CPU caches) | 0.00% stale, all 15 rows |
| PAT 0 (WB, non-coherent) and the copy engine | same pattern as PAT 2 |
| Full flush after each round reads NEW on every line | passed, every round |
| Immediate `clflushopt; mfence` re-check of each stale load (unrolled test) | NEW in every case (0 OLD) |
| Values that are neither OLD nor NEW | 0 |

So the GPU really leaves the CPU copies stale, the stale reads really come
from non-coherence, and the GPU's writes were in DRAM when the loads ran.

## 1. One trial at a time (`gpu_stale`, PAT 2)

Each trial is `SEQUENCE` then an `mfence`, so every trial starts with a
drained pipeline and store buffer. There are about 27,300 loads per row.
`MISS-st` means a store to a cache-missing scratch line comes first.

| Sequence | P-core stale | E-core stale |
|---|---|---|
| load (no flush) | 100.00% | 100.00% |
| `clflushopt; load` | 95.40% | 100.00% |
| `clflushopt; sfence; load` | 95.08% | 100.00% |
| **`clflushopt; lfence; load`** | **2 (0.01%)** | **14 (0.05%)** |
| `clflushopt; sfence; lfence; load` | 8 (0.03%) | 13 (0.05%) |
| `clflushopt; mfence; load` | 0 | 0 |
| `clflush; load` | 94.92% | 100.00% |
| `clflush; lfence; load` | 3 (0.01%) | 12 (0.04%) |
| `clwb; lfence; load` | 100.00% | 100.00% |
| `clwb; mfence; load` | 100.00% | 100.00% |
| **MISS-st; `clflushopt; lfence; load`** | **33.51%** | **28 (0.10%)** |
| MISS-st; `clflushopt; sfence; lfence; load` | 28.38% | 14 (0.05%) |
| MISS-st; `clflushopt; mfence; load` | 0 | 0 |
| MISS-st; `clflush; lfence; load` | 28.19% | 13 (0.05%) |

In isolation `lfence` almost always works, but not always. With a slow store
ahead of the flush, it fails about a third of the time on the P-core.

## 2. Many flushes behind one fence (`gpu_stale`, grouped)

`clflushopt` K lines, one fence, then load all K. 204,800 loads per row.

| K | no fence (P / E) | `lfence` (P / E) | `mfence` (P / E) |
|---|---|---|---|
| 1 | 85.04% / 100.00% | 0 / 0 | 0 / 0 |
| 4 | 99.26% / 99.99% | 0 / 0 | 0 / 0 |
| 16 | 91.60% / 99.98% | 0 / 0 | 0 / 0 |
| 64 | 2.30% / 0.00% | 0 / 0 | 0 / 0 |

`lfence` never failed here. With K = 64 even the unfenced loads are mostly
fresh, because 64 flushes back to back stall the pipeline long enough for
them to finish.

## 3. Back to back (`unrolled_test`)

2048 unrolled `clflushopt L; FENCE; load L` triples, nothing between them.
There are 204,800 loads per cell. The store version saves each loaded value
to memory; the register-only version doesn't store anything.

| Variant | P-core store | P-core reg-only | E-core store | E-core reg-only |
|---|---|---|---|---|
| in order, no fence | 100.0% | 100.0% | 100.0% | 100.0% |
| in order, `lfence` | **313 (0.2%)** | 0 | **3** | 0 |
| in order, `sfence; lfence` | **68.9%** | 0 | **1,295 (0.6%)** | 0 |
| in order, `mfence` | 0 | 0 | 0 | 0 |
| shuffled, no fence | 100.0% | 100.0% | 100.0% | 100.0% |
| shuffled, `lfence` | **2,070 (1.0%)** | **35** | **79** | **17** |
| shuffled, `sfence; lfence` | **75.5%** | **12.6%** | 0 | 0 |
| shuffled, `mfence` | 0 | 0 | 0 | 0 |

Every stale load in fenced variants read NEW when re-checked right away.

On the P-core the stale loads are spread across the whole sequence, which
rules out GPU writes arriving late (those would bunch stale loads at the
start). For example, in-order `lfence` gives 36 36 57 28 36 44 55 21 per
eighth of the sequence.

## Interpretation

- **`lfence` doesn't wait for `clflushopt` to take effect.** The failures grow
  when stores are in flight: MISS-st rows, and the store version versus the
  register-only version. A plausible explanation: `clflushopt` goes through
  the store buffer like a store. `lfence` waits for earlier instructions to
  retire, not for the store buffer to drain, so the flush can still be queued
  when the load runs. `mfence` does drain the store buffer. This model fits
  the data but is not proven.
- **`sfence; lfence` is often worse than `lfence` alone.** That is surprising
  and unexplained. It shows that `sfence` doesn't wait for `clflushopt` to
  finish before later instructions proceed.
- **Legacy `clflush` behaves the same** as `clflushopt` with respect to later
  loads.
- **`clwb` writes the line back but leaves it in the cache**, so it can never
  be used to read another host's writes.
- **P-cores fail far more often than E-cores**, but E-cores still fail
  with both `lfence` and `sfence; lfence`.

## Things to know

- **Rates vary between sessions.** In an earlier session,
  MISS-st `clflushopt; lfence` was 0.01% stale on a P-core; in later sessions,
  with the same code, it was consistently 25-40%. A table-order change and the
  end-of-round check were both ruled out as the cause. What stays constant is
  that `lfence` sometimes fails and `mfence` never does.
- **E-core failures in `unrolled_test` cluster.** Shuffled-order `lfence`
  failures all fall in the sixth eighth of the sequence (72, 57, 56, 79 across
  four runs), so they look tied to particular lines or code positions rather
  than random timing. The 1,295 in-order `sfence; lfence` failures in this
  run are exactly 256 per eighth from the fourth eighth on (plus 15 in the
  third). That fits a single round in which every load from about line 780
  onward came back stale. Earlier runs had none. Neither pattern has been
  investigated yet.
- **Only this one CPU model has been tested.**
