# LFENCE does not order a load after CLFLUSHOPT / CLFLUSH

On memory without hardware cache coherence (for example CXL memory shared by
several hosts), a reader must evict its possibly stale cached copy of a line
before loading data another host wrote. Code sometimes does this with:

```
clflushopt x      ; or clflush x
lfence
mov x, reg
```

The Intel SDM doesn't clearly say whether the `lfence` keeps the load from
being served from the old cache line. This repo lets you check on your own
machine. On a Core Ultra 9 285 (Arrow Lake) the answer is **no**: the load
can return the stale value, often. With `mfence` in place of `lfence`, it
never did. See [RESULTS.md](RESULTS.md).

## How it works

A normal machine is fully coherent, so we need something to write memory
without invalidating the CPU's caches, the way another CXL host would. The
integrated GPU does that when its mapping of the memory is set not to snoop
the CPU caches. Each round, for 2048 cache lines:

1. The CPU writes `OLD` to each line, flushes it to DRAM, and reads it back,
   so the CPU cache holds a clean `OLD` copy.
2. The GPU writes `NEW` to each line in DRAM. The CPU's cached `OLD` copies
   stay where they are.
3. The CPU runs the sequence under test on each line. If the load returns
   `OLD`, it was served from the cache line the flush should have removed.

`gpu_nc.h` does the GPU part through the i915 driver and explains each step.
The GPU runs a batch of `MI_STORE_DWORD_IMM` commands through a non-snooping
(uncached) mapping. No shaders are involved.

## What is tested

`lfence_test` runs each pattern with `lfence`, and the same pattern with
`mfence` as a control:

- **Isolated trials:** `FLUSH x; FENCE; mov x`, with an `mfence` before the
  next trial so each starts with an empty pipeline.
- **Store-miss trials:** the same, but first a store to a line that is not in
  the cache. This is the clearest case on Arrow Lake P-cores.
- **Back to back:** 2048 `FLUSH x; FENCE; mov x` triples in a row, one per
  line.

The controls, all checked automatically:

- A load with no flush must be ~100% stale, which shows the lines really are
  stale.
- Every `mfence` variant must be 0% stale, which shows a correct sequence
  reads `NEW`.
- Every stale load is re-read at once with `clflushopt; mfence; load`, which
  must return `NEW`. That rules out the GPU write simply not having reached
  DRAM yet.
- After each round, every line must read `NEW` once properly flushed.
- `COHERENT=1` runs everything with a GPU mapping that snoops the CPU caches.
  Every row must then be 0% stale, which shows the stale reads come from
  non-coherence.

If any control fails, the program says so and exits with status 2.

## Requirements

- Linux with the **i915** driver on an Intel iGPU that supports
  `I915_GEM_CREATE_EXT_SET_PAT` (Meteor Lake, Arrow Lake). The PAT indices
  are platform specific; see `gpu_nc.h`. The controls tell you if they are
  wrong for your GPU.
- A CPU with `clflushopt`.
- gcc and the kernel UAPI headers (`/usr/include/drm/i915_drm.h`).
- Access to the GPU render node, via either:
  ```
  sudo usermod -aG render $USER                   # permanent; log in again
  sudo setfacl -m u:$USER:rw /dev/dri/renderD128  # until reboot
  ```

## Running

```
./run.sh
```

This builds the test, runs it on the first P-core and the first E-core (on
hybrid CPUs), then runs the coherent control. Output goes to the terminal and
to `results.txt`. It takes a few seconds.

To run by hand:

```
make
./lfence_test [rounds=300]    # non-coherent run on CPU 2
CPU=8 ./lfence_test           # pick the core
COHERENT=1 ./lfence_test      # coherent control
```

`run.sh` also accepts `PCORE=<n> ECORE=<n> ROUNDS=<n>`.

Each run ends with one of:

- `RESULT: N of M loads after "flush; lfence" returned stale data.` The
  misordering was observed.
- `RESULT: no stale loads ...` Not observed in this run. Try more rounds or
  another core.
- `SETUP CHECK FAILED` A control misbehaved, so the run proves nothing on
  this machine.

## Caveats

- **Rates vary** between core types and between sessions on the same machine.
  In one earlier session, store-miss `clflushopt; lfence` was stale only
  0.01% of the time on a P-core; in later sessions it was 25-86%. E-cores
  show far fewer stale loads, but still some.
- **Only one CPU model has been tested so far.** Other Intel generations and
  AMD may behave differently. Results from other machines are welcome.
- **The GPU stands in for a remote host**, but the misordering is between a
  flush and a later load on the CPU. It doesn't depend on how the stale line
  was created.

## Files

| File | Purpose |
|---|---|
| `lfence_test.c` | the test |
| `gpu_nc.h` | iGPU as a non-coherent writer (i915 setup, documented) |
| `run.sh` | builds and runs the test on P- and E-cores plus the coherent control |
| `Makefile` | builds `lfence_test` |
| `RESULTS.md` | results on a Core Ultra 9 285 |
| `results.txt` | raw output of the run in RESULTS.md |

## CXL-TSO litmus tests (branch `cxl`)

This branch adds two programs that use the same iGPU trick to check the
assumptions of the CXL-TSO memory model on real hardware:

- `cxl_litmus` runs the litmus tests of the paper, with the CPU as one host and
  the GPU as the other. The GPU both writes memory and, through
  `MI_COPY_MEM_MEM`, reads it.
- `cxl_test` runs instruction-level checks: which fences order a load after a
  flush, whether a clean line is ever written back, whether a read can return
  an older value after a newer one, and what a failing compare-and-swap does.

[LITMUS.md](LITMUS.md) states every test as in the paper, explains how it is
run with a GPU in place of a second machine, and gives the results on a Core
Ultra 9 285. Run everything with `./run-cxl.sh`.
