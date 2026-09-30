# Results: Intel Core Ultra 9 285 (Arrow Lake)

Core Ultra 9 285: 8 Lion Cove P-cores (CPUs 0-7) and 16 Skymont E-cores
(CPUs 8-23), Arrow Lake-S iGPU with the i915 driver, Linux 7.0.0-34-generic.
One `./run.sh` run on 2026-09-30 at 20:58 UTC, on CPU 0 (P-core) and CPU 8
(E-core). The raw output is in `results.txt`.

"Stale" means the load returned the value the CPU cached before the GPU
overwrote the line in DRAM: on non-coherent CXL memory, a read that missed
another host's write.

## Summary

| Sequence | P-core stale | E-core stale |
|---|---|---|
| `clflushopt; lfence; load` | **13** | **33** |
| `clflush; lfence; load` | **1** | **32** |
| store-miss; `clflushopt; lfence; load` | **86.2%** | **33** |
| store-miss; `clflush; lfence; load` | **8.3%** | **33** |
| `clflushopt; lfence; load` x2048, back to back | **248** | 0 |
| `clflush; lfence; load` x2048, back to back | **26.3%** | 0 |
| every `mfence` variant | 0 | 0 |

`lfence` let stale loads through on both core types, with both `clflushopt`
and `clflush`. `mfence` never did.

## Controls

| Control | P-core | E-core |
|---|---|---|
| load (no flush), must be ~100% stale | 100.000% | 98.982% |
| every `mfence` variant, must be 0 | 0 | 0 |
| stale loads re-read with `clflushopt; mfence`, must read NEW | all NEW | all NEW |
| values neither OLD nor NEW, must be 0 | 0 | 0 |
| every line reads NEW after a full flush at the end of each round | passed | passed |
| `COHERENT=1` (GPU snoops CPU caches): every row must be 0 | 0 in all 13 rows (P-core) | |

## Full numbers

About 68,000 loads per row for the isolated trials, 153,600 for back to back.

| Sequence | P-core stale | P-core % | E-core stale | E-core % |
|---|---|---|---|---|
| load (no flush) | 68,266 | 100.000% | 67,571 | 98.982% |
| `clflushopt; mfence; load` | 0 | 0 | 0 | 0 |
| `clflush; mfence; load` | 0 | 0 | 0 | 0 |
| `clflushopt; lfence; load` | 13 | 0.019% | 33 | 0.048% |
| `clflush; lfence; load` | 1 | 0.001% | 32 | 0.047% |
| store-miss; `clflushopt; mfence; load` | 0 | 0 | 0 | 0 |
| store-miss; `clflush; mfence; load` | 0 | 0 | 0 | 0 |
| store-miss; `clflushopt; lfence; load` | 58,877 | 86.248% | 33 | 0.048% |
| store-miss; `clflush; lfence; load` | 5,644 | 8.268% | 33 | 0.048% |
| back to back `clflushopt; mfence; load` | 0 | 0 | 0 | 0 |
| back to back `clflush; mfence; load` | 0 | 0 | 0 | 0 |
| back to back `clflushopt; lfence; load` | 248 | 0.161% | 0 | 0 |
| back to back `clflush; lfence; load` | 40,413 | 26.311% | 0 | 0 |

## Interpretation

- **`lfence` doesn't wait for the flush to take effect.** It waits for
  earlier instructions to *retire*, but a flush can retire before the line is
  actually invalidated. `mfence` waits for earlier flushes to complete.
- **The P-core failure rate jumps when a store is ahead of the flush:** 86%
  with a cache-missing store before `clflushopt; lfence`. That fits flushes
  being handled like stores, queued in the store buffer behind the missing
  store, which `lfence` does not drain. This model fits the data but is not
  proven.
- **`clflush` is no better than `clflushopt` here.** Its stronger ordering
  rules don't cover later loads.

## Variation between runs

The rates change between sessions. In one earlier session on this machine,
store-miss `clflushopt; lfence` was stale only 0.01% of the time on a P-core;
later sessions gave 25-86% with essentially the same test. The cause is
unknown. Every session showed stale loads after `lfence`, and none after
`mfence`.
