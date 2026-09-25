# Does `clflushopt x; lfence; load x` see another agent's write?

On CXL memory shared by several hosts without hardware coherence, software has
to manage caching itself. Before reading data that another host may have
written, a reader has to evict its own cached copy of the line and then read
from the device. Some programs do this with:

```
clflushopt x
lfence
mov x, reg
```

The Intel SDM doesn't clearly say whether `lfence` keeps that load from being
served from the old cache line before the flush takes effect. These programs
test it directly on real hardware.

## Short answer (Core Ultra 9 285 / Arrow Lake)

**No, `lfence` is not enough.** On this CPU, loads after
`clflushopt; lfence` sometimes return the stale cached value: rarely when each
trial starts from a drained pipeline, but up to ~40% of the time when a slow
store is ahead of the flush. `sfence; lfence` is no better. **`mfence` was the
only fence that never let a stale load through**, in any test.

See [RESULTS.md](RESULTS.md) for the numbers and what they mean, and
`results.txt` for the raw output of that run.

## How it works: the iGPU as a non-coherent writer

A normal single-socket machine is fully coherent, so no other agent in it can
leave the CPU's cached copy stale. The integrated GPU can, if its memory
accesses are set up not to snoop the CPU caches. `gpu_nc.h` does that through
the i915 driver:

1. Allocate a GEM buffer object whose **GPU-side PAT index is uncached and
   non-coherent** (index 2 on Meteor Lake/Arrow Lake), and map it
   **write-back** on the CPU.
2. CPU writes `OLD` to each test line, flushes it to DRAM, then reads it back,
   so each line has a *clean* `OLD` copy in the CPU caches.
3. The GPU command streamer writes `NEW` to each line (`MI_STORE_DWORD_IMM` in
   a batch buffer). The write goes to DRAM without invalidating the CPU's copy.
   Wait for the GPU to finish.
4. Run the sequence under test. A load returning `OLD` came from the stale
   cache line, i.e. it got ahead of the flush. `NEW` means it went to memory.

`gpu_nc.h` explains each i915 primitive in detail: render node, GEM BOs, PAT
indices, WB mmap, batch buffers and MI command encoding, softpinned ppGTT
addresses, execbuf and wait.

### Controls and sanity checks

- **`load (no flush)`** must be ~100% stale. This shows the setup really
  produces stale lines, and that nothing (kernel, prefetchers) flushes them
  behind our back.
- **PAT index 3** (GPU snoops the CPU caches) must be 0% stale in every row.
  This shows the stale reads come from non-coherence.
- **After every round**, the tests flush every line properly and check that
  it reads `NEW`, so the GPU write really reached memory. They exit with an
  error if not.
- **The unrolled test re-checks each stale load right away** with
  `clflushopt; mfence; load`, which must return `NEW`. This rules out the GPU
  write landing late. It also records where in the sequence the stale loads
  happened: late writes would bunch them at the start, and they are spread out.

## Requirements

- Linux with the **i915** driver on an Intel iGPU that supports
  `I915_GEM_CREATE_EXT_SET_PAT` (Meteor Lake, Arrow Lake). PAT indices are
  platform specific; on other GPUs, check the i915 PAT table and confirm the
  controls behave as described above.
- A CPU with `clflushopt` and `clwb`.
- Kernel UAPI headers (`/usr/include/drm/i915_drm.h`), gcc, python3.
- Access to `/dev/dri/renderD128`, via either:
  ```
  sudo usermod -aG render $USER                   # permanent; log in again
  sudo setfacl -m u:$USER:rw /dev/dri/renderD128  # until reboot
  ```

## Running the tests

### Everything at once

```
./run_all.sh
```

This builds both programs, runs the full suite (about 10 seconds), prints the
results and saves them to `results.txt`. On a hybrid CPU it runs every test on
the first P-core and the first E-core (from `/sys/devices/cpu_core/cpus` and
`/sys/devices/cpu_atom/cpus`). Use `PCORE=<n> ECORE=<n> ./run_all.sh` to pick
other cores.

**Check the controls before reading anything else in the output:**

1. In every PAT 2 and PAT 0 table, `load (no flush)` is ~100% stale.
2. The PAT 3 table is 0% stale in every row.
3. Nothing reports "GPU write ... did not reach memory" or "unexpected
   values".
4. In `unrolled_test`, every re-check reads `NEW` (`0 OLD`).

If any of these fail, the GPU isn't acting as a non-coherent writer on your
machine and the other rows mean nothing. The usual cause is a different PAT
table on another GPU generation.

### Building by hand

```
make            # builds gpu_stale and unrolled_test
make clean
```

### `gpu_stale`: one trial at a time

Each line gets one sequence, then an `mfence` to drain everything before the
next line. Sequences rotate across lines and rounds, so each gets about
1/15 of the loads.

```
./gpu_stale [pat_index=2] [rounds=200] [render|blt]
```

| Example | What it does |
|---|---|
| `./gpu_stale` | main test: PAT 2 (uncached, non-coherent), render engine, CPU 2 |
| `./gpu_stale 3` | coherent control: every row should be 0% stale |
| `./gpu_stale 0` | other non-coherent PAT (write-back on the GPU side) |
| `./gpu_stale 2 100 blt` | GPU writes from the copy engine instead of render |
| `CPU=8 ./gpu_stale` | run on CPU 8 (an E-core on the 285) |
| `K=64 F=1 ./gpu_stale` | grouped: 64 x `clflushopt`, one `lfence`, then 64 loads |

`F` selects the grouped-mode fence: 0 = none, 1 = `lfence`, 2 = `mfence`.
`K` can be 1 to 64.

Output columns: `stale` = load returned OLD (served from the stale cache
line); `fresh` = returned NEW (read from memory); `other` = anything else
(should always be 0).

The `MISS-st` rows first store to a cache line that isn't in the cache, so a
slow store sits in the store buffer ahead of the flush.

### `unrolled_test`: back to back

2048 fully unrolled `clflushopt L; FENCE; load L` triples in a row with no
fences between them. Lines are visited in order (`seq`) or in a fixed shuffle
(`rand`), with fence `none`, `lfence`, `sfence; lfence` (`sflf`) or `mfence`.

```
./unrolled_test [rounds=1600]
CPU=8 ./unrolled_test
```

Rounds cycle through the 8 variants, alternating two versions of each:

- **Store version:** saves every loaded value to an array, so each triple
  also does a store.
- **Register-only version:** counts stale loads with `cmp`/`adc` and writes
  nothing to memory.

The second table shows, for fenced variants, where in the 2048-triple
sequence the stale loads happened, and what an immediate
`clflushopt; mfence; load` re-check of those lines returned.

## Caveats

- Only one CPU model has been tested (Core Ultra 9 285). Other Intel
  generations and AMD may behave differently.
- Stale-load rates vary between sessions on the same machine. Whether stale
  loads appear at all is consistent; how often is not. See RESULTS.md.
- The PAT indices are platform specific. See the comment in `gpu_nc.h`.

## Files

| File | Purpose |
|---|---|
| `gpu_nc.h` | iGPU non-coherent writer: all the i915 setup, documented |
| `gpu_stale.c` | one-trial-at-a-time and grouped tests |
| `gen_unrolled.py` | generates `unrolled.c` (unrolled sequences and variant table) |
| `unrolled.h` | interface to the generated code |
| `unrolled_test.c` | back-to-back test driver |
| `Makefile` | builds both programs |
| `run_all.sh` | builds and runs the full suite, writes `results.txt` |
| `RESULTS.md` | results and interpretation for a Core Ultra 9 285 |
| `results.txt` | raw output of the `run_all.sh` run that RESULTS.md describes |
