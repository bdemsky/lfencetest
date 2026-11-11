# CXL-TSO litmus tests on real hardware

This file lists the litmus tests of the CXL-TSO memory model, and for each one
shows how it is run on a single machine and what was observed. The numbers are from one run on an
Intel Core Ultra 9 285 (Arrow Lake), Linux 7.0.0-42, on 2026-11-11:
`CPU=0` (P-core), `CPU=8` (E-core), and a coherent control. The raw output is
in `litmus-cpu0.txt`, `litmus-cpu8.txt`, `litmus-coherent.txt` (from
`cxl_litmus`) and `out-cpu0.txt`, `out-cpu8.txt`, `out-coherent.txt` (from
`cxl_test`).

- [How a second host is obtained](#how-a-second-host-is-obtained)
- [Notation](#notation)
- [Summary](#summary)
- [Tests of Section 2](#tests-of-section-2)
- [Tests of Appendix A](#tests-of-appendix-a)
- [Tests that cannot be run here](#tests-that-cannot-be-run-here)
- [Instruction-level checks](#instruction-level-checks)
- [Two behaviours the model does not have](#two-behaviours-the-model-does-not-have)
- [Limits](#limits)

## How a second host is obtained

The paper's tests need several hosts that share memory without cache coherence.
We have one machine, so:

- **One host is the CPU.** Its cores are the threads of that host, and its
  caches are the host's cache. A test that has two threads on one host runs
  them on two cores (`CPU` and `CPU+1`).
- **The other host is the integrated GPU**, driven through a mapping that does
  not snoop the CPU caches (`gpu_nc.h`). It can do two things, both directly
  in DRAM:
  - *write* a value (`MI_STORE_DWORD_IMM`). To the CPU this looks like a write
    by another host that has already been written back.
  - *read* a value (`MI_COPY_MEM_MEM` into a result slot that the CPU reads
    afterwards). This is a read by another host that holds no copy of the
    line, i.e. a read made right after `clflush; mfence`.

The GPU has no stale cache of its own and cannot run a compare-and-swap. So in
every test the CPU plays the host whose cache matters, and the GPU plays the
hosts that only write, or that read after a proper flush. A successful CAS by
the other host is represented by the write it would perform.

**Schedules.** Two real hosts would run concurrently, and the coherent flags of
the tests (`Y`, `Z`, ...) only serve to order their instructions. Here the
harness orders them directly: the CPU runs some instructions, then a GPU batch
is submitted and waited for, then the CPU continues. Each test below gives the
schedule used. For an outcome the model *allows*, the schedule is the one that
produces it, and we check that it does. For an outcome the model *forbids*, the
schedule is the one that gets closest to it, and we check that it never occurs;
`sb` is run under all ten interleavings.

Every schedule is run on 512 independent cells (one line for `x`, one for `y`,
one for the GPU's results) for 100 rounds, so 51,200 trials per row. Where a
test says the host "holds `x = 0`", the CPU has read the line before the test
starts, which is what a prefetch or an earlier access would do.

**Coherent control.** With `COHERENT=1` the GPU mapping snoops the CPU caches,
so the same programs run on coherent memory. Every weak outcome below was then
observed 0 times.

## Notation

As in the paper: `x`, `y` are non-coherent locations and `X`, `Y`, `Z` coherent
ones, all initially 0; `a`, `b`, `c`, `d` are registers. Columns are threads.
Threads on different hosts are in columns headed by different hosts; threads on
the same host share a host name. A subscript before or after `:=` is a flush
or fence executed before or after the assignment, on the location accessed:
`fo` = `clflushopt`, `fl` = `clflush`, `wb` = `clwb`, `sf` = `sfence`,
`mf` = `mfence`. A comment gives the value read in the outcome discussed.

In the "run as" lines, **CPU** and **GPU** say who executes each step, and
`[x cached]` means the CPU starts with a clean copy of the initial value.

## Summary

| Test | Outcome | Model | P-core | E-core |
|---|---|---|---|---|
| `cas-nc` | both CAS succeed | allowed | 51,197 | 51,200 |
| `cas` (coherent control) | both CAS succeed | forbidden | 0 | — |
| `rrf` | `(a,b) = (1,0)` | allowed | 51,196 / 51,194 | 51,199 / 51,200 |
| `rrf-flush` | `(a,b) = (1,0)` | forbidden | 0 / 0 | 0 / 0 |
| `sb` | `(a,b) = (0,0)` | forbidden | 0 of 512,000 | 0 of 512,000 |
| `far` | `(a,b,c) = (1,0,1)` | allowed | 51,200 | 51,199 |
| `cleanwb` | `a = 0` | forbidden | 0 (4 variants) | 0 (4 variants) |
| `erratic` | `(a,b) = (1,0)` | erratic reads only | 0 | 0 |
| `erratic-flush` | `(a,b) = (1,0)` | forbidden | 0 | 0 |
| `write-race` | `a = 0` | allowed | 51,198 | 51,196 |
| `write-no-race` | write of host 1 lost | forbidden | 0 | 0 |
| `mp-flush-nc` | `(a,b) = (1,0)` | forbidden | 0 | 0 |
| `wrc` / `wrc-fl` | `(a,b,c) = (1,1,0)` | allowed / forbidden | as `rrf` / `rrf-flush` | |
| `corr-fl1`, `corr-fl2` | `(a,b,c) = (1,2,1)` | erratic reads only | 0 / 0 | 0 / 0 |
| `corr-fl12` | `(a,b,c) = (1,2,1)` | forbidden | 0 | 0 |
| `cas-no-wb` | `(a,b) = (1,0)` | allowed | 51,192 | 51,200 |
| `cas-two-events` | `(a,b,c) = (1,1,0)` / `(1,1,1)` | forbidden / allowed | 0 / 51,165 | 0 / 51,199 |
| `cas-host-atomic` | `(a,b,c) = (1,2,1)` | forbidden | 0 | 0 |
| `cas-erratic` | `(a,b) = (0,1)` | erratic reads only | 0 | 0 |
| `ner-lost` | `(a,b,c) = (1,0,1)` | erratic reads only | 0 | 0 |

Counts are out of 51,200 unless stated. Every outcome the model forbids was
observed 0 times, every outcome it allows unconditionally was observed, and no
outcome that needs an erratic read was observed. Two behaviours outside these
tests do differ from the model; see
[the last section](#two-behaviours-the-model-does-not-have).

## Tests of Section 2

### cas-nc and cas

| Host 1 | Host 2 |
|---|---|
| a := CAS(`x`,0,1) &nbsp;// 0 | b := CAS(`x`,0,1) &nbsp;// 0 |

Outcome: both operations succeed (each returns 0). Allowed on a non-coherent
location (`cas-nc`), forbidden on a coherent one (`cas`).

Run as: `[x cached]` · **GPU** `x := 1` (host 2's successful CAS, as seen in
memory) · **CPU** `a := CAS(x,0,3)`. The CPU writes 3 instead of 1 so the two
writes can be told apart.

| | P-core | E-core | Coherent control |
|---|---|---|---|
| CPU's CAS also succeeds | 51,197 | 51,200 | 0 |

### rrf

| Host 1 | Host 2 |
|---|---|
| `x` := 1 | a := `Y` &nbsp;// 1 |
| `Y` := 1 | b := `x` &nbsp;// 0 |

Outcome `(a,b) = (1,0)`: allowed. The paper gives two independent reasons, and
each is run separately.

- Host 2 fetched `x` earlier. Run as: `[x cached]` · **GPU** `x := 1` · **CPU**
  `b := x`.
- Host 1 has not written `x` back. Run as: **CPU** `x := 1` · **GPU** `b := x`.

| `b = 0` | P-core | E-core |
|---|---|---|
| CPU is host 2 (reads) | 51,196 | 51,199 |
| CPU is host 1 (writes) | 51,194 | 51,200 |

### rrf-flush

| Host 1 | Host 2 |
|---|---|
| `x` :=<sub>wb,sf</sub> 1 | a := `Y` &nbsp;// 1 |
| `Y` := 1 | <sub>fo,mf</sub>b := `x` &nbsp;// 0 |

Outcome `(a,b) = (1,0)`: forbidden.

- Run as (CPU is host 2): `[x cached]` · **GPU** `x := 1` · **CPU**
  `clflushopt x; mfence; b := x`.
- Run as (CPU is host 1): **CPU** `x := 1; clwb x; sfence` · **GPU** `b := x`.
- The paper notes that `clwb` cannot replace `clflushopt` in host 2. Run as the
  first schedule with `clwb x; mfence; b := x`.

| `b = 0` | Model | P-core | E-core |
|---|---|---|---|
| CPU is host 2 (`clflushopt; mfence`) | forbidden | 0 | 0 |
| CPU is host 1 (`clwb; sfence`) | forbidden | 0 | 0 |
| CPU is host 2 with `clwb` instead | allowed | 51,200 | 51,199 |

The schedule cannot show that the `sfence` of host 1 is needed, because
submitting a GPU batch is a system call, which waits for the `clwb` anyway.
The fences of host 2 are covered by the
[fence matrix](#fence-matrix-the-rules-behind-the-flushes).

### sb

| Host 1 | Host 2 |
|---|---|
| `x` :=<sub>wb,mf</sub> 1 | `y` :=<sub>wb,mf</sub> 1 |
| <sub>fo,mf</sub>a := `y` &nbsp;// 0 | <sub>fo,mf</sub>b := `x` &nbsp;// 0 |

Outcome `(a,b) = (0,0)`: forbidden.

Run as: the CPU is host 1 and the GPU is host 2. `[x, y cached]`. The CPU's
program is cut into `c1 = x := 1; clwb x; mfence`, `c2 = clflushopt y; mfence`,
`c3 = a := y`; the GPU's into `g1 = y := 1` and `g2 = b := x`. All ten
interleavings of `c1 c2 c3` with `g1 g2` are run. The same program without any
flush or fence is run as: **CPU** `x := 1` · **GPU** `y := 1; b := x` · **CPU**
`a := y`.

| `(a,b) = (0,0)` | Model | P-core | E-core |
|---|---|---|---|
| with the flushes, 10 interleavings | forbidden | 0 of 512,000 | 0 of 512,000 |
| without flushes or fences | allowed | 51,180 | 51,196 |

### far

| Host 1 | Host 2, thread 1 | Host 2, thread 2 |
|---|---|---|
| a := `Y` &nbsp;// 1 | `x` :=<sub>wb,mf</sub> 1 | c := `x` &nbsp;// 1 |
| <sub>fo,mf</sub>b := `x` &nbsp;// 0 | | `Y` := 1 |

Outcome `(a,b,c) = (1,0,1)`: allowed. The read of `c` is served by the cache of
host 2 before `x` has been written back.

Run as: the CPU is host 2, with its two threads on two cores; the GPU is
host 1. **CPU core 1** `x := 1` (the store reaches the cache; its `clwb` has not
run yet) · **CPU core 2** `c := x` · **GPU** `b := x` · **CPU core 1**
`clwb x; mfence`.

The paper's remedy is a writeback after the read. Run as the same schedule with
**CPU core 2** `c := x; clwb x; sfence`.

| `(b,c) = (0,1)` | Model | P-core | E-core |
|---|---|---|---|
| `far` | allowed | 51,200 | 51,199 |
| with a writeback after the read of `c` | forbidden | 0 | 0 |

### cleanwb

| Host 1 | Host 2 |
|---|---|
| `x` := 1 | (a second host) |
| a := `x` &nbsp;// 0 | |

Outcome `a = 0`: allowed only if hosts perform clean writebacks; the paper
assumes they do not, so forbidden.

Run as: the CPU is the *second* host, holding a clean copy of `x = 0`; the GPU
is host 1. `[x cached]` · **GPU** `x := 1` · **CPU** gives up the line in one
of four ways · **GPU** `a := x`.

| `a = 0` | P-core | E-core |
|---|---|---|
| CPU runs `clwb x; sfence` | 0 | 0 |
| CPU runs `clflushopt x; mfence` | 0 | 0 |
| CPU runs `clflush x; mfence` | 0 | 0 |
| CPU evicts the line (reads 192 MB) | 0 | 0 |

See also [clean writebacks](#clean-writebacks) below, which has a positive
control and more actions.

### erratic and erratic-flush

| Host 1 | Host 2 |
|---|---|
| a := `x` &nbsp;// 1 | `x` := 1 |
| b := `x` &nbsp;// 0 | |

Outcome `(a,b) = (1,0)`: allowed with erratic reads, forbidden without.

Run as: `[x cached]` · **GPU** `x := 1` · **CPU** `a := x; b := x`, in two
variants: with a `prefetcht0 x` before the first read, and with cache pressure
(64 MB read) before each read so that some lines are fetched again between the
two reads.

`erratic-flush` is the same program with `clflush x; mfence` between the two
reads; `(1,0)` is forbidden. Run as: **GPU** `x := 1` · **CPU**
`a := x; clflush x; mfence; b := x`.

| `(a,b) = (1,0)` | Model | P-core | E-core |
|---|---|---|---|
| `erratic`, reads back to back | erratic reads only | 0 | 0 |
| `erratic`, cache pressure between the reads | erratic reads only | 0 | 0 |
| &nbsp;&nbsp;(first read was fresh, `a = 1`) | | 27,396 | 48,409 |
| `erratic-flush` | forbidden | 0 | 0 |

See also [erratic reads](#erratic-reads) below.

### write-race

| Host 1 | Host 2 |
|---|---|
| `x[0]` := 1 | `x[1]` := 2 |
| a := `x[0]` &nbsp;// 0 | b := `x[1]` |

`x[0]` and `x[1]` are on the same cache line. Outcome `a = 0`: the write of
host 1 is lost. Allowed (and reported by the model checker as a cache-line
race).

Run as: the CPU is host 2 and the GPU host 1. `[line cached]` · **GPU**
`x[0] := 1` · **CPU** `x[1] := 2`, and later the line is written back
(`clflush x; mfence`) · **GPU** `a := x[0]`.

| `a = 0` | P-core | E-core |
|---|---|---|
| | 51,198 | 51,196 |

### write-no-race

| Host 1 | Host 2 |
|---|---|
| `x[0]` :=<sub>wb,sf</sub> 1 | a := `Y` &nbsp;// 1 |
| `Y` := 1 | <sub>fo,sf</sub>`x[1]` := 2 |

Outcome: the write to `x[0]` is lost. Forbidden.

Run as: `[line cached]` · **GPU** `x[0] := 1` · **CPU**
`clflushopt x; sfence; x[1] := 2` · the line is written back · **GPU** reads
`x[0]`. Variants change what the CPU runs before its write.

| write of host 1 lost | Model | P-core | E-core |
|---|---|---|---|
| `clflushopt x; sfence; x[1] := 2` | forbidden | 0 | 0 |
| `clflush x; sfence; x[1] := 2` | forbidden | 0 | 0 |
| `clflush x; mfence; x[1] := 2` | forbidden | 0 | 0 |
| `clwb x; sfence; x[1] := 2` | allowed | 51,198 | 51,199 |
| `clflushopt x; x[1] := 2` (no fence) | allowed | 50,780 | 0 |
| `clflush x; x[1] := 2` (no fence) | **forbidden** | **50,769** | 0 |

The last row is discussed
[below](#a-write-right-after-clflush-is-not-protected-by-it).

## Tests of Appendix A

### mp-flush-nc, wrc, wrc-fl

| Host 1 | Host 2 |
|---|---|
| `x` := 1 | a := `y` &nbsp;// 1 |
| `clwb(x)`; `sfence` | `clflush(x)`; `mfence` |
| `y` := 1 | b := `x` &nbsp;// 0 |

`mp-flush-nc`, outcome `(a,b) = (1,0)`: forbidden. It is `rrf-flush` with
`clflush`. Run as: `[x cached]` · **GPU** `x := 1` · **CPU**
`clflush x; mfence; b := x`. Observed 0 on both core types.

| Host 1 | Host 2 | Host 3 |
|---|---|---|
| `x` := 1 | a := `x` &nbsp;// 1 | b := `y` &nbsp;// 1 |
| | `y` := 1 | c := `x` &nbsp;// 0 |

`wrc`, outcome `(a,b,c) = (1,1,0)`: allowed. `wrc-fl` adds
`clflush(x); mfence` before `c := x` and the outcome is forbidden. Host 2 only
passes on the fact that `x = 1` has reached memory, which the harness does by
waiting for the GPU. So the schedules are those of `rrf` (CPU is host 3) and
of `mp-flush-nc`, with the same results: 51,196 / 51,199 for `wrc`, and 0 / 0
for `wrc-fl`.

### corr-fl1, corr-fl2, corr-fl12

| Host 1 | Host 2 | Host 3 (`corr-fl1`) | Host 3 (`corr-fl2`) | Host 3 (`corr-fl12`) |
|---|---|---|---|---|
| `x` := 1 | `x` := 2 | a := `x` &nbsp;// 1 | a := `x` &nbsp;// 1 | a := `x` &nbsp;// 1 |
| | | `clflush(x)`; `mfence` | b := `x` &nbsp;// 2 | `clflush(x)`; `mfence` |
| | | b := `x` &nbsp;// 2 | `clflush(x)`; `mfence` | b := `x` &nbsp;// 2 |
| | | c := `x` &nbsp;// 1 | c := `x` &nbsp;// 1 | `clflush(x)`; `mfence` |
| | | | | c := `x` &nbsp;// 1 |

Outcome `(a,b,c) = (1,2,1)`: needs an erratic read in `corr-fl1` and
`corr-fl2`, forbidden in `corr-fl12`.

Run as (the CPU is host 3, the GPU hosts 1 and 2):

- `corr-fl1`: **GPU** `x := 1` · **CPU** `a := x; clflush x; mfence;
  prefetcht0 x` · **GPU** `x := 2` · **CPU** `prefetcht0 x; b := x; c := x`.
- `corr-fl2`: **GPU** `x := 2` · **CPU** `prefetcht0 x` · **GPU** `x := 1` ·
  **CPU** `prefetcht0 x; a := x; b := x; clflush x; mfence; c := x`.
- `corr-fl12`: **GPU** `x := 1` · **CPU** `a := x; clflush x; mfence` · **GPU**
  `x := 2` · **CPU** `b := x; clflush x; mfence; c := x`.

The prefetches try to make the CPU fetch the line while it already holds a
copy, which is what an erratic read is.

| `(a,b,c) = (1,2,1)` | Model | P-core | E-core |
|---|---|---|---|
| `corr-fl1` | erratic reads only | 0 | 0 |
| `corr-fl2` | erratic reads only | 0 | 0 |
| `corr-fl12` | forbidden | 0 | 0 |

### cas-no-wb

| Host 1 | Host 2 |
|---|---|
| CAS(`x`,0,1) | a := `y` &nbsp;// 1 |
| `y` := 1 | `clflush(x)`; `mfence` |
| | b := `x` &nbsp;// 0 |

Outcome `(a,b) = (1,0)`: allowed. A successful CAS reaches the cache, not the
memory.

Run as: `[x cached]` · **CPU** `CAS(x,0,1)` (succeeds) · **GPU** `b := x`.

| `b = 0` | P-core | E-core |
|---|---|---|
| | 51,192 | 51,200 |

### cas-two-events

| Host 1 | Host 2 | Host 3 |
|---|---|---|
| `x` := 1 | a := CAS(`x`,1,2) &nbsp;// 1 | b := `y` &nbsp;// 1 |
| | `y` := 1 | `clflush(x)`; `mfence` |
| | | c := `x` |

Outcome `(a,b,c) = (1,1,0)`: forbidden. `(1,1,1)`: allowed.

Run as: **GPU** `x := 1` · **CPU** `a := CAS(x,1,2)` · **GPU** `c := x`.

| | Model | P-core | E-core |
|---|---|---|---|
| `(a,c) = (1,0)` | forbidden | 0 | 0 |
| `(a,c) = (1,1)` | allowed | 51,165 | 51,199 |

### cas-host-atomic

| Host 1, thread 1 | Host 1, thread 2 | Host 2 |
|---|---|---|
| CAS(`x`,0,1) | a := `y` &nbsp;// 1 | `x` := 2 |
| | `clflush(x)`; `mfence` | `clwb(x)`; `sfence` |
| | b := `x` &nbsp;// 2 | `y` := 1 |
| | `clflush(x)`; `mfence` | |
| | c := `x` &nbsp;// 1 | |

Outcome `(a,b,c) = (1,2,1)`: forbidden. The CAS would have to read 0 before the
read of `b` and write 1 after it.

Run as: the CPU is host 1, with its threads on two cores; the GPU is host 2.
`[x cached on both cores]` · **GPU** `x := 2` · then, concurrently, **CPU
core 2** `CAS(x,0,1)` after a random delay and **CPU core 1**
`clflush x; mfence; b := x; clflush x; mfence; c := x`. This is the one test
that relies on a real race between two cores.

| `(b,c)` | Model | P-core | E-core |
|---|---|---|---|
| `(2,1)` | forbidden | 0 | 0 |
| `(1,1)`: the CAS ran first and succeeded | allowed | 51,015 | 50,766 |
| `(2,2)`: the CAS ran later and failed | allowed | 185 | 434 |

Both orders occurred, so the race was exercised, though rarely in the second
direction.

### cas-erratic

| Host 1 | Host 2, thread 1 | Host 2, thread 2 | Host 3 |
|---|---|---|---|
| `x` := 1 | a := CAS(`x`,0,2) &nbsp;// 0 | b := `x` &nbsp;// 1 | c := `x` &nbsp;// 1 |
| | | | `clflush(x)`; `mfence` |
| | | | d := `x` &nbsp;// 2 |

Outcome `(a,b,c,d) = (0,1,1,2)`: allowed with erratic reads only. On host 2,
the CAS still sees 0 although another thread of the host already sees 1.

Run as: the CPU is host 2. `[x cached on both cores]` · **GPU** `x := 1` ·
cache pressure · **CPU core 2** `b := x` · **CPU core 1** `a := CAS(x,0,2)`.

| `(a,b) = (0,1)` | P-core | E-core |
|---|---|---|
| | 0 | 0 |

### ner-lost

| Host 1 | Host 2 |
|---|---|
| `x` := 1 | a := `y` &nbsp;// 1 |
| `x` := 2 | b := `x` &nbsp;// 0 |
| `clflush(x)` | c := `x` &nbsp;// 1 |
| `y` := 1 | |

Outcome `(a,b,c) = (1,0,1)`: allowed with erratic reads only.

Run as: `[x cached]` · **GPU** `x := 1` · **CPU** `prefetcht0 x` · **GPU**
`x := 2` · **CPU** `b := x; c := x`.

| `(b,c) = (0,1)` | P-core | E-core |
|---|---|---|
| | 0 | 0 |

### fl-hob-r and r-hob-fl (in part)

These two tests use three hosts with caches and cannot be run as stated. What
they rely on is that a flush by one thread of a host is ordered with the reads
of the *other* threads of that host: the cache is per host, not per thread.
That part can be run.

| Host 1 | Host 2, thread 1 | Host 2, thread 2 |
|---|---|---|
| `x` := 1 | `clflush(x)`; `mfence` | c := `x` |

Run as: `[x cached on both cores]` · **GPU** `x := 1` · **CPU core 2**
`clflush x; mfence` · **CPU core 1** `c := x`.

| `c = 0` | Model | P-core | E-core |
|---|---|---|---|
| no flush on the other core | allowed | 51,167 | 51,200 |
| `clflush x; mfence` on the other core | forbidden | 0 | 0 |

## Tests that cannot be run here

| Test | Why |
|---|---|
| `iriw` | Needs two reading hosts, each with its own cache. Only the CPU has one. |
| `iriw` with collocated threads | Needs two hosts whose threads read each other's unwritten stores from the host cache. |
| `iriw-mixed` | Needs a coherent location shared by the CPU and the GPU as well as a non-coherent one, and two reading hosts. |
| `fl-hob-r`, `r-hob-fl` | Three hosts with caches. The property they rely on is run above. |
| `ner-fwd` | All its threads are on one host: it is ordinary x86-TSO store buffering and involves no non-coherent memory. |

## Instruction-level checks

These are run by `cxl_test`. Each round starts with the CPU holding a clean
copy of an old value while memory holds a new one written by the GPU.

### Fence matrix: the rules behind the flushes

`FLUSH x; <fence>; load x`. A stale load is one that returns the old value.
About 29,000 loads per cell; the range covers `clflushopt` and `clflush`, with
and without a cache-missing store before the flush.

| Fence | Model | P-core stale | E-core stale |
|---|---|---|---|
| none | stale allowed | 94.5–100% | 100% |
| `sfence` | stale allowed | 90.9–96.4% | 100% |
| `lfence` | stale allowed | 0–0.9% | 0.03–0.05% |
| `mfence` | forbidden | 0 | 0 |
| `lock add` | forbidden | 0 | 0 |
| (no flush at all) | | 100% | 100% |

### Clean writebacks

An action is applied to a line whose cached copy is clean, and memory is read
afterwards. "Reverted" means memory holds the old value again.

| Action on a clean stale line | P-core reverted | E-core reverted |
|---|---|---|
| nothing (control) | 0 | 0 |
| store of the value already cached (positive control) | 100% | 100% |
| `clwb; sfence` | 0 | 0 |
| `clflushopt; mfence` | 0 | 0 |
| `clflush; mfence` | 0 | 0 |
| `prefetchw` | 0 | 0 |
| eviction by cache pressure | 0 | 0 |
| `cmpxchg` that fails | 99.0% | 99.7% |
| `lock cmpxchg` that fails | 99.1% | 99.7% |
| `cmpxchg` that fails, on another word of the line | 99.1% | 99.8% |
| `lock cmpxchg` that fails, on another word of the line | 99.1% | 99.8% |
| `lock add $0` | 99.0% | 99.7% |

51,200 lines per row. The CPU never wrote back a line it had not written. The
CAS rows are discussed [below](#a-compare-and-swap-that-fails-still-writes).

### Erratic reads

Each line is read 24 times under growing cache pressure, with and without
software prefetches (`prefetcht0`, `prefetcht2`, `prefetchnta`, `prefetchw`).
A line that returns the new value and later the old one would be an erratic
read.

| | P-core | E-core |
|---|---|---|
| lines | 614,400 | 614,400 |
| lines that switched to the new value | 94,515 | 97,810 |
| lines that then returned the old value | 0 | 0 |

## Two behaviours the model does not have

### A compare-and-swap that fails still writes

In the model, a CAS that fails is only a read. On this CPU it leaves the line
modified, holding the value it read, and the whole line is later written back.

| Host 1 | Host 2 |
|---|---|
| a := CAS(`x`,5,6) &nbsp;// 0, fails | b := `Y` &nbsp;// 1 |
| `Y` := 1 | `x` :=<sub>wb,sf</sub> 1 |
| c := `Z` &nbsp;// 1 | `Z` := 1 |
| `clwb(x)`; `sfence`; `W` := 1 | d := `W` &nbsp;// 1 |
| | <sub>fl,mf</sub>e := `x` &nbsp;// **0** |

Host 1 never writes `x` in the model, so `e = 0` is forbidden there. Run as:
`[x cached]` · **CPU** failing `CAS` · **GPU** `x := 1` · **CPU** the line
leaves the cache · read memory.

| the write of host 2 is lost | P-core | E-core |
|---|---|---|
| after a `lock cmpxchg` that fails | 51,183 of 51,200 | 51,166 of 51,200 |
| after a `cmpxchg` that fails | 51,169 of 51,200 | 51,166 of 51,200 |
| after a plain load (control) | 0 | 0 |

This agrees with Intel's documentation of `CMPXCHG`, whose destination
receives a write cycle whatever the result of the comparison. It is the write
that matters, not ownership of the line: `prefetchw`, which requests ownership
without writing, never caused a writeback. The whole line is written back, so
a failing CAS on one word loses a remote write to another word of the line.

### A write right after clflush is not protected by it

| Host 1 | Host 2 |
|---|---|
| `x[0]` :=<sub>wb,sf</sub> 1 | a := `Y` &nbsp;// 1 |
| `Y` := 1 | `clflush(x)` |
| b := `Z` &nbsp;// 1 | `x[1]` := 2 |
| <sub>fl,mf</sub>c := `x[0]` &nbsp;// **0** | `clwb(x)`; `sfence`; `Z` := 1 |

This is `write-no-race` with `clflush` and no fence before the write. The
paper's table of preserved program order says a `clflush` is ordered before a
later write, so host 2 would write into a freshly fetched line and `c = 0` is
forbidden.

| write of host 1 lost | P-core | E-core |
|---|---|---|
| `clflush x; x[1] := 2` | 50,769 of 51,200 | 0 |
| `clflush x; sfence; x[1] := 2` | 0 | 0 |
| `clflush x; mfence; x[1] := 2` | 0 | 0 |

On the P-cores the write lands in the stale copy of the line. A GPU read taken
right after the sequence shows that memory does not yet hold `x[1]`, so the
flush did not simply run after the write; the line is still in the cache,
modified, with the stale `x[0]`. We do not know the mechanism. With one
location per cache line, as in the formal model, the difference cannot be
observed, since the write replaces the whole line. It matters for cache-line
races: an invalidation needs a fence before the write it protects, whichever
flush instruction is used.

## Limits

- **One CPU model, one run.** Rates of the timing-dependent rows vary between
  runs, as RESULTS.md notes for the `lfence` test.
- **Not observing an outcome is not a proof.** In particular, the absence of
  erratic reads is evidence for this CPU only.
- **The GPU is not a CXL host.** What is tested is how the CPU's own caches
  behave when memory changes underneath them, which is the behaviour the model
  describes, but the memory is ordinary DRAM.
- **Schedules replace real concurrency**, except in `cas-host-atomic`. A
  forbidden outcome that needs a finer interleaving than the ones run would be
  missed.
- **The GPU cannot hold a stale copy or run a CAS**, which is why five tests
  are not run.
