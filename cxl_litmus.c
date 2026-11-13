// cxl_litmus.c - The litmus tests of the CXL-TSO paper, run on real hardware.
//
// One host is this CPU: its cores are the threads of a host, and its caches are
// the host's cache. The other host is the integrated GPU, used through a
// mapping that does not snoop the CPU caches (see gpu_nc.h). The GPU can
//   - write a value to memory   (MI_STORE_DWORD_IMM), and
//   - read a value from memory  (MI_COPY_MEM_MEM into a result slot),
// both directly in DRAM. It has no stale copies of its own, so it plays the
// host that writes, or the host that reads after a proper flush.
//
// A litmus test is run as a SCHEDULE: the harness makes the CPU run some of
// its instructions, then submits a GPU batch and waits for it, and so on. Each
// step is applied to NT independent cells (one x line, one y line and one line
// of GPU result slots each), and every test is repeated for a number of rounds.
// LITMUS.md gives, for every test, the program as in the paper, the schedule
// used here, and the result.
//
// How to read a test (see "Schedule steps" below):
//   init(X_Y_CACHED)              start: x = y = 0; the CPU holds clean copies
//   GPU(g_set(x, 1))              GPU:  x := 1          (in memory)
//   GPU(g_get(r0, x))             GPU:  r0 := x         (from memory)
//   CPU(b[k] = load(x))           CPU:  b := x          (main core)
//   ON_CORE2(t2_c_load_x)         CPU:  c := x          (second core of the host)
//   gpu_results(b, NULL)          the GPU's r0 is the paper's register b
//   COUNT(row, outcome, 'F', IS(b, 0))   count the cells where b = 0
// a, b, c, d are the paper's registers (one value per cell; k is the cell).
// Program comments use the paper's notation: x :=wb,sf 1 is "x := 1; clwb x;
// sfence", fo,mf b := x is "clflushopt x; mfence; b := x" (fo = clflushopt,
// fl = clflush, wb = clwb, sf = sfence, mf = mfence); || separates hosts and
// | threads of one host.
//
// Usage: ./cxl_litmus [rounds=100]
//   env CPU=<n>      core of the main thread (default 0); the second thread of
//                    the host runs on CPU+1 (override with CPU2=<n>)
//   env COHERENT=1   control: the GPU snoops the CPU caches
#include "gpu_nc.h"
#include <pthread.h>

#define NT 512                       // cells per round
#define MI_COPY_MEM_MEM ((0x2Eu << 23) | (5 - 2))   // dst addr (2 dwords), src addr (2 dwords)

static struct nc g;
static int coherent;
static uint32_t epoch;               // changes at every init(), so values never repeat

// The value the paper calls v, made unique to the current test run.
static inline uint32_t V(uint32_t v) { return 0x0a000000u | (epoch << 4) | v; }

// Layout of cell k: line 4k holds x (x[0] at byte 0, x[1] at byte 32), line
// 4k+1 holds y, line 4k+2 holds the GPU's result slots.
static inline uint64_t XOFF(int k) { return (uint64_t)(4 * k) * LINE; }
static inline uint64_t YOFF(int k) { return (uint64_t)(4 * k + 1) * LINE; }
static inline uint64_t ROFF(int k, int j) { return (uint64_t)(4 * k + 2) * LINE + 4 * j; }
static inline volatile uint32_t *XP(int k) { return &g.t[XOFF(k) / 4]; }
static inline volatile uint32_t *YP(int k) { return &g.t[YOFF(k) / 4]; }
static inline volatile uint32_t *RP(int k) { return &g.t[ROFF(k, 0) / 4]; }

// ============================================================== GPU side
static uint32_t *bp;
static void gb_begin(void) { bp = g.b; }
static void gb_store(uint64_t off, uint32_t val) {
  uint64_t va = TARGET_VA + off;
  *bp++ = MI_STORE_DWORD_IMM; *bp++ = (uint32_t)va; *bp++ = (uint32_t)(va >> 32); *bp++ = val;
}
static void gb_copy(uint64_t dst, uint64_t src) {
  uint64_t dva = TARGET_VA + dst, sva = TARGET_VA + src;
  *bp++ = MI_COPY_MEM_MEM; *bp++ = (uint32_t)dva; *bp++ = (uint32_t)(dva >> 32);
  *bp++ = (uint32_t)sva; *bp++ = (uint32_t)(sva >> 32); *bp++ = MI_NOOP;
}
static void gb_run(void) {
  *bp++ = MI_BATCH_BUFFER_END; *bp++ = MI_NOOP;
  uint32_t len = (uint32_t)((char *)bp - (char *)g.b);
  if (len > 4 * 4 * LINES + 4096) { fprintf(stderr, "batch too long\n"); exit(1); }
  clflush_range(g.b, len);
  nc_exec(&g, len);
  nc_wait(&g);
  asm volatile("mfence" ::: "memory");
}

// GPU(cmds): one GPU batch, running cmds for every cell; the CPU waits for it.
// Inside cmds, x, y, x1 (= x[1]) are the cell's locations and r0, r1 its two
// result slots, and the commands are
//   g_set(loc, v)    loc := v, written to memory
//   g_get(r, loc)    r := loc, read from memory
#define GPU(...)                                                              \
  do {                                                                        \
    gb_begin();                                                               \
    for (int k = 0; k < NT; k++) {                                            \
      uint64_t x = XOFF(k), y = YOFF(k), x1 = XOFF(k) + 32;                   \
      uint64_t r0 = ROFF(k, 0), r1 = ROFF(k, 1);                              \
      (void)x; (void)y; (void)x1; (void)r0; (void)r1;                         \
      __VA_ARGS__;                                                            \
    }                                                                         \
    gb_run();                                                                 \
  } while (0)
#define g_set(loc, v) gb_store(loc, V(v))
#define g_get(r, loc) gb_copy(r, loc)

// After the GPU batches of a test: fetch the result slots (invalidate the
// result lines, then read them), and hand slot 0 and slot 1 to the paper's
// registers r0 and r1 (either may be NULL).
static uint32_t res[NT][4];
static void gpu_results(uint32_t *r0, uint32_t *r1) {
  for (int k = 0; k < NT; k++) asm volatile("clflushopt (%0)" ::"r"(RP(k)) : "memory");
  asm volatile("mfence" ::: "memory");
  for (int k = 0; k < NT; k++)
    for (int j = 0; j < 4; j++) res[k][j] = RP(k)[j];
  for (int k = 0; k < NT; k++) {
    if (r0) r0[k] = res[k][0];
    if (r1) r1[k] = res[k][1];
  }
}

// ============================================================== CPU side
// Each helper is ONE asm block; its name spells out the instruction sequence,
// in order, with nothing in between. p is the cell's x (or y); the values
// stored or compared are the paper's values.
#define LOAD_SEQ(name, body)                                                  \
  static inline uint32_t name(volatile uint32_t *p) {                         \
    uint32_t v;                                                               \
    asm volatile(body : "=&r"(v) : "r"(p) : "memory", "cc");                  \
    return v;                                                                 \
  }
LOAD_SEQ(load, "mov (%1), %0")
LOAD_SEQ(clflushopt_mfence_load, "clflushopt (%1)\n\tmfence\n\tmov (%1), %0")
LOAD_SEQ(clflush_mfence_load, "clflush (%1)\n\tmfence\n\tmov (%1), %0")
LOAD_SEQ(clwb_mfence_load, "clwb (%1)\n\tmfence\n\tmov (%1), %0")
LOAD_SEQ(prefetcht0_load, "prefetcht0 (%1)\n\tmov (%1), %0")

#define STORE_SEQ(name, body)                                                 \
  static inline void name(volatile uint32_t *p, uint32_t v) {                 \
    asm volatile(body ::"r"(p), "r"(V(v)) : "memory", "cc");                  \
  }
STORE_SEQ(store, "mov %1, (%0)")
STORE_SEQ(store_clwb_sfence, "mov %1, (%0)\n\tclwb (%0)\n\tsfence")
STORE_SEQ(store_clwb_mfence, "mov %1, (%0)\n\tclwb (%0)\n\tmfence")
// write-no-race: p is the line x, the store is to x[1] (byte 32 of the line)
STORE_SEQ(clflushopt_sfence_store_x1, "clflushopt (%0)\n\tsfence\n\tmov %1, 32(%0)")
STORE_SEQ(clflushopt_store_x1, "clflushopt (%0)\n\tmov %1, 32(%0)")
STORE_SEQ(clflush_store_x1, "clflush (%0)\n\tmov %1, 32(%0)")
STORE_SEQ(clflush_sfence_store_x1, "clflush (%0)\n\tsfence\n\tmov %1, 32(%0)")
STORE_SEQ(clflush_mfence_store_x1, "clflush (%0)\n\tmfence\n\tmov %1, 32(%0)")
STORE_SEQ(clwb_sfence_store_x1, "clwb (%0)\n\tsfence\n\tmov %1, 32(%0)")
// write-fl-race with something before the clflush: is the loss due to what precedes the flush?
// "a := Y" is a load of y (the line after x, at 64(%0)) into eax.
#define STORE_SEQ_A(name, body)                                               \
  static inline void name(volatile uint32_t *p, uint32_t v) {                 \
    asm volatile(body ::"r"(p), "r"(V(v)) : "memory", "cc", "eax");           \
  }
// write-fl-race where the write itself is a locked instruction: x[1] := 2 done by a CAS that
// succeeds (x[1] is 0), by an xchg, or attempted by a CAS that fails (it still writes the line).
static inline void clflush_cas_x1(volatile uint32_t *p, uint32_t v) {
  uint32_t e = V(0);
  asm volatile("clflush (%1)\n\tlock cmpxchg %2, 32(%1)" : "+a"(e) : "r"(p), "r"(V(v)) : "memory", "cc");
}
static inline void clflush_xchg_x1(volatile uint32_t *p, uint32_t v) {
  uint32_t n = V(v);
  asm volatile("clflush (%1)\n\txchg %0, 32(%1)" : "+r"(n) : "r"(p) : "memory");
}
static inline void clflush_failing_cas_x1(volatile uint32_t *p, uint32_t v) {
  uint32_t e = V(5);
  asm volatile("clflush (%1)\n\tlock cmpxchg %2, 32(%1)" : "+a"(e) : "r"(p), "r"(V(v)) : "memory", "cc");
}
STORE_SEQ(mfence_clflush_store_x1, "mfence\n\tclflush (%0)\n\tmov %1, 32(%0)")
STORE_SEQ(lfence_clflush_store_x1, "lfence\n\tclflush (%0)\n\tmov %1, 32(%0)")
STORE_SEQ_A(loady_clflush_store_x1, "mov 64(%0), %%eax\n\tclflush (%0)\n\tmov %1, 32(%0)")
STORE_SEQ_A(loady_mfence_clflush_store_x1, "mov 64(%0), %%eax\n\tmfence\n\tclflush (%0)\n\tmov %1, 32(%0)")
STORE_SEQ_A(loady_lfence_clflush_store_x1, "mov 64(%0), %%eax\n\tlfence\n\tclflush (%0)\n\tmov %1, 32(%0)")
STORE_SEQ_A(loady_mfence_clflush_sfence_store_x1, "mov 64(%0), %%eax\n\tmfence\n\tclflush (%0)\n\tsfence\n\tmov %1, 32(%0)")

static inline void clflush_mfence(volatile uint32_t *p) { asm volatile("clflush (%0)\n\tmfence" ::"r"(p) : "memory"); }
static inline void clflushopt_mfence(volatile uint32_t *p) { asm volatile("clflushopt (%0)\n\tmfence" ::"r"(p) : "memory"); }
static inline void clwb_sfence(volatile uint32_t *p) { asm volatile("clwb (%0)\n\tsfence" ::"r"(p) : "memory"); }
static inline void clwb_mfence(volatile uint32_t *p) { asm volatile("clwb (%0)\n\tmfence" ::"r"(p) : "memory"); }
static inline void prefetcht0(volatile uint32_t *p) { asm volatile("prefetcht0 (%0)" ::"r"(p) : "memory"); }
static inline void mfence(void) { asm volatile("mfence" ::: "memory"); }
// a := CAS(x, e, n) (lock cmpxchg): returns the value read; it succeeded iff
// that value is e.
static inline uint32_t cas(volatile uint32_t *p, uint32_t e, uint32_t n) {
  uint32_t r = V(e);
  asm volatile("lock cmpxchg %2, (%1)" : "+a"(r) : "r"(p), "r"(V(n)) : "memory", "cc");
  return r;
}

// CPU(stmts): the CPU's main core runs stmts for every cell, one cell after
// the other. Inside, x and y point to the cell's locations and k is the cell.
#define CPU(...)                                                              \
  for (int k = 0; k < NT; k++) {                                              \
    volatile uint32_t *x = XP(k), *y = YP(k);                                 \
    (void)x; (void)y;                                                         \
    __VA_ARGS__;                                                              \
  }

// Cache pressure: read len bytes of a large buffer, so that some lines of the
// test leave the CPU caches.
#define THRASH_BYTES (192u << 20)
#define PRESSURE_64MB (64u << 20)
static char *thrash_buf;
static volatile uint64_t sink;
static void thrash(size_t len) {
  uint64_t s = 0;
  for (size_t i = 0; i < len; i += LINE) s += (unsigned char)thrash_buf[i % THRASH_BYTES];
  sink += s;
}

// The paper's registers, one value per cell, and the value read by a CAS
// whose result the paper does not name.
static uint32_t a[NT], b[NT], c[NT], d[NT], cas_read[NT];

// --------------------------------------- second thread of the host (core 2)
// ON_CORE2(fn) runs fn on the second core and waits for it; START_ON_CORE2(fn)
// and JOIN_CORE2() let it run concurrently with the main core.
static void (*volatile w_fn)(void);
static volatile int w_seq, w_done;
static void *worker(void *arg) {
  cpu_set_t cs;
  CPU_ZERO(&cs);
  CPU_SET((int)(intptr_t)arg, &cs);
  if (sched_setaffinity(0, sizeof cs, &cs)) die("sched_setaffinity(worker)");
  int seen = 0;
  for (;;) {
    while (__atomic_load_n(&w_seq, __ATOMIC_ACQUIRE) == seen) asm volatile("pause");
    seen++;
    w_fn();
    asm volatile("mfence" ::: "memory");
    __atomic_store_n(&w_done, seen, __ATOMIC_RELEASE);
  }
  return NULL;
}
static void START_ON_CORE2(void (*fn)(void)) { w_fn = fn; __atomic_add_fetch(&w_seq, 1, __ATOMIC_RELEASE); }
static void JOIN_CORE2(void) {
  while (__atomic_load_n(&w_done, __ATOMIC_ACQUIRE) != __atomic_load_n(&w_seq, __ATOMIC_ACQUIRE))
    asm volatile("pause");
}
static void ON_CORE2(void (*fn)(void)) { START_ON_CORE2(fn); JOIN_CORE2(); }

// What the second core runs (CPU(...) on the second core):
static void t2_touch_x_y(void) { CPU((void)load(x); (void)load(y)); }
static void t2_c_load_x(void) { CPU(c[k] = load(x)); }
static void t2_b_load_x(void) { CPU(b[k] = load(x)); }
static void t2_c_load_x_clwb_sfence(void) { CPU(c[k] = load(x); clwb_sfence(x)); }
static void t2_clflush_mfence_x(void) { CPU(clflush_mfence(x)); }
static void t2_cas_x_0_1_after_random_delay(void) {
  unsigned s = 12345;
  CPU(s = s * 1103515245u + 12345u;
      for (unsigned dl = (s >> 16) % 24; dl; dl--) asm volatile("pause");
      cas_read[k] = cas(x, 0, 1));
}

// Start of every test: x[0] = x[1] = y = 0 in memory, result slots cleared,
// nothing cached. Then, with X_Y_CACHED, the main core reads x and y, so the
// host holds clean copies ("the host fetched the lines earlier"); with
// X_Y_CACHED_ON_BOTH_CORES the second core reads them too.
enum start { NOTHING_CACHED, X_Y_CACHED, X_Y_CACHED_ON_BOTH_CORES };
static void init(enum start st) {
  epoch++;
  for (int k = 0; k < NT; k++) {
    XP(k)[0] = V(0); XP(k)[8] = V(0); YP(k)[0] = V(0);
    for (int j = 0; j < 4; j++) RP(k)[j] = 0xdead0000u;
  }
  clflush_range(g.t, (size_t)LINES * LINE);
  if (st != NOTHING_CACHED) CPU((void)load(x); (void)load(y));
  mfence();
  if (st == X_Y_CACHED_ON_BOTH_CORES) ON_CORE2(t2_touch_x_y);
}

// ============================================================== reporting
// verdict: 'A' allowed by the model, 'F' forbidden, 'E' allowed only with
// erratic reads, 'C' forbidden only on coherent memory.
struct row { const char *test, *outcome; char verdict; long hits, trials; };
static struct row rows[80];
static int nrows;
static long *row(const char *test, const char *outcome, char verdict) {
  for (int i = 0; i < nrows; i++)
    if (!strcmp(rows[i].test, test) && !strcmp(rows[i].outcome, outcome)) return &rows[i].hits;
  rows[nrows] = (struct row){test, outcome, verdict, 0, 0};
  return &rows[nrows++].hits;
}
// COUNT(row, outcome, verdict, cond): count the cells k for which cond holds.
#define COUNT(test, outcome, verdict, cond)                                   \
  do { long *h_ = row(test, outcome, verdict);                                \
       for (int k = 0; k < NT; k++) { h_[1]++; if (cond) h_[0]++; } } while (0)
// IS(r, v): register r of cell k holds the paper's value v.
#define IS(r, v) ((r)[k] == V(v))

// ================================================ Section 2 of the paper

// cas-nc / cas
//      host 1 (CPU)              || host 2 (GPU)
//      a := CAS(x,0,1)   // 0    || b := CAS(x,0,1)   // 0
//   Both CAS succeed: allowed on non-coherent x (cas-nc), forbidden on
//   coherent X (cas; run here as the COHERENT=1 control).
// The GPU cannot run a CAS: its successful CAS is the write x := 1 it would
// make. The CPU writes 3 instead of 1 so that the two writes can be told apart.
static void t_cas_nc(void) {
  init(X_Y_CACHED);
  GPU(g_set(x, 1));                                   // GPU:  x := 1 (host 2's CAS)
  CPU(a[k] = cas(x, 0, 3));                           // CPU:  a := CAS(x,0,3)
  COUNT(coherent ? "cas (coherent memory)" : "cas-nc", "both CAS succeed", coherent ? 'C' : 'A',
        IS(a, 0));
}

// rrf
//      host 1          || host 2
//      x := 1          || a := Y   // 1
//      Y := 1          || b := x   // 0
//   (a,b) = (1,0): allowed, for two independent reasons, each run alone.
// The flag Y only orders the hosts; the schedule does that here.
// (wrc of the appendix runs as the first schedule: see t_mp_flush_nc.)
static void t_rrf(void) {
  // Host 2 fetched x earlier.  CPU = host 2, GPU = host 1.
  init(X_Y_CACHED);
  GPU(g_set(x, 1));                                   // GPU:  x := 1
  CPU(b[k] = load(x));                                // CPU:  b := x
  COUNT("rrf (this host reads)", "b = 0", 'A', IS(b, 0));

  // Host 1 has not written x back.  CPU = host 1, GPU = host 2.
  init(X_Y_CACHED);
  CPU(store(x, 1));                                   // CPU:  x := 1
  mfence();
  GPU(g_get(r0, x));                                  // GPU:  b := x
  gpu_results(b, NULL);
  COUNT("rrf (this host writes)", "b = 0", 'A', IS(b, 0));
}

// rrf-flush
//      host 1          || host 2
//      x :=wb,sf 1     || a := Y          // 1
//      Y := 1          || fo,mf b := x    // 0
//   (a,b) = (1,0): forbidden.
static void t_rrf_flush(void) {
  // CPU = host 2 (reads), GPU = host 1.
  init(X_Y_CACHED);
  GPU(g_set(x, 1));                                   // GPU:  x := 1
  CPU(b[k] = clflushopt_mfence_load(x));              // CPU:  clflushopt x; mfence; b := x
  COUNT("rrf-flush (this host reads; clflushopt, mfence)", "b = 0", 'F', IS(b, 0));

  // CPU = host 1 (writes), GPU = host 2.
  init(X_Y_CACHED);
  CPU(store_clwb_sfence(x, 1));                       // CPU:  x := 1; clwb x; sfence
  GPU(g_get(r0, x));                                  // GPU:  b := x
  gpu_results(b, NULL);
  COUNT("rrf-flush (this host writes; clwb, sfence)", "b = 0", 'F', IS(b, 0));

  // VARIANT (not a paper test; the paper says clwb cannot replace clflushopt
  // in host 2): host 2 runs  wb,mf b := x.  Allowed.
  init(X_Y_CACHED);
  GPU(g_set(x, 1));                                   // GPU:  x := 1
  CPU(b[k] = clwb_mfence_load(x));                    // CPU:  clwb x; mfence; b := x
  COUNT("rrf-flush with clwb in the reader", "b = 0", 'A', IS(b, 0));
}

// sb
//      host 1 (CPU)       || host 2 (GPU)
//      x :=wb,mf 1        || y :=wb,mf 1
//      fo,mf a := y // 0  || fo,mf b := x // 0
//   (a,b) = (0,0): forbidden.
// The GPU's steps are g1 = "y := 1" and g2 = "b := x" (in memory, so its
// flushes are implicit); the CPU's are c1 = "x := 1; clwb x; mfence",
// c2 = "clflushopt y; mfence", c3 = "a := y". All 10 interleavings are run.
static void t_sb(void) {
  // VARIANT (no flushes or fences in the program): allowed.
  init(X_Y_CACHED);
  CPU(store(x, 1));                                   // CPU:  x := 1
  mfence();
  GPU(g_set(y, 1); g_get(r0, x));                     // GPU:  y := 1; b := x
  CPU(a[k] = load(y));                                // CPU:  a := y
  gpu_results(b, NULL);
  COUNT("sb without flushes", "(a,b) = (0,0)", 'A', IS(a, 0) && IS(b, 0));

  // sb: g1 at position p1, g2 at position p2 > p1, c1 c2 c3 in the others.
  for (int p1 = 0; p1 < 5; p1++)
    for (int p2 = p1 + 1; p2 < 5; p2++) {
      init(X_Y_CACHED);
      int cstep = 0;
      for (int pos = 0; pos < 5; pos++) {
        if (pos == p1) { GPU(g_set(y, 1)); continue; }          // g1  GPU: y := 1
        if (pos == p2) { GPU(g_get(r0, x)); continue; }         // g2  GPU: b := x
        switch (cstep++) {
        case 0: CPU(store_clwb_mfence(x, 1)); break;            // c1  CPU: x := 1; clwb x; mfence
        case 1: CPU(clflushopt_mfence(y)); break;               // c2  CPU: clflushopt y; mfence
        case 2: CPU(a[k] = load(y)); break;                     // c3  CPU: a := y
        }
      }
      gpu_results(b, NULL);
      COUNT("sb (all 10 interleavings)", "(a,b) = (0,0)", 'F', IS(a, 0) && IS(b, 0));
    }
}

// far
//      host 1 (GPU)       || host 2 (CPU), thread 1 | thread 2
//      a := Y        // 1 || x :=wb,mf 1            | c := x   // 1
//      fo,mf b := x  // 0 ||                        | Y := 1
//   (a,b,c) = (1,0,1): allowed (c is served by host 2's cache before x is
//   written back).  Thread 1 is the main core, thread 2 the second core.
static void t_far(void) {
  init(X_Y_CACHED_ON_BOTH_CORES);
  CPU(store(x, 1));                                   // CPU core 1:  x := 1
  mfence();                                           //   (the store has reached the cache)
  ON_CORE2(t2_c_load_x);                              // CPU core 2:  c := x
  GPU(g_get(r0, x));                                  // GPU:         b := x
  CPU(clwb_mfence(x));                                // CPU core 1:  clwb x; mfence (too late)
  gpu_results(b, NULL);
  COUNT("far", "(b,c) = (0,1)", 'A', IS(b, 0) && IS(c, 1));

  // VARIANT (the paper's remedy): thread 2 runs  c := x; clwb x; sfence.
  // Forbidden.
  init(X_Y_CACHED_ON_BOTH_CORES);
  CPU(store(x, 1));                                   // CPU core 1:  x := 1
  mfence();
  ON_CORE2(t2_c_load_x_clwb_sfence);                  // CPU core 2:  c := x; clwb x; sfence
  GPU(g_get(r0, x));                                  // GPU:         b := x
  gpu_results(b, NULL);
  COUNT("far with a writeback after the read of c", "(b,c) = (0,1)", 'F', IS(b, 0) && IS(c, 1));
}

// iriw, iriw-mixed: not run (they need two reading hosts with caches; see
// LITMUS.md, "Tests that cannot be run here").

// cleanwb
//      host 1 (GPU)       || host 2 (CPU)
//      x := 1             || (a second host, holding a clean copy of x = 0)
//      a := x   // 0      ||
//   a = 0: forbidden (the paper assumes no clean writebacks).
// The CPU gives up its clean copy in one of four ways.
static void t_cleanwb_variant(const char *name, int how) {
  init(X_Y_CACHED);
  GPU(g_set(x, 1));                                   // GPU:  x := 1
  if (how == 0) CPU(clwb_sfence(x));                  // CPU:  clwb x; sfence
  if (how == 1) CPU(clflushopt_mfence(x));            // CPU:  clflushopt x; mfence
  if (how == 2) CPU(clflush_mfence(x));               // CPU:  clflush x; mfence
  if (how == 3) thrash(THRASH_BYTES);                 // CPU:  evicts the line (reads 192 MB)
  GPU(g_get(r0, x));                                  // GPU:  a := x
  gpu_results(a, NULL);
  COUNT(name, "a = 0", 'F', IS(a, 0));
}
static void t_cleanwb(void) {
  t_cleanwb_variant("cleanwb (second host runs clwb)", 0);
  t_cleanwb_variant("cleanwb (second host runs clflushopt)", 1);
  t_cleanwb_variant("cleanwb (second host runs clflush)", 2);
  t_cleanwb_variant("cleanwb (second host evicts the line)", 3);
}

// erratic
//      host 1 (CPU)       || host 2 (GPU)
//      a := x   // 1      || x := 1
//      b := x   // 0      ||
//   (a,b) = (1,0): allowed only with erratic reads.
static void t_erratic(void) {
  // Reads back to back, the first one after a prefetch.
  init(X_Y_CACHED);
  GPU(g_set(x, 1));                                   // GPU:  x := 1
  CPU(a[k] = prefetcht0_load(x);                      // CPU:  prefetcht0 x; a := x
      b[k] = load(x));                                //       b := x
  COUNT("erratic (reads back to back)", "(a,b) = (1,0)", 'E', IS(a, 1) && IS(b, 0));

  // Cache pressure before each read, so that some lines are fetched again
  // between the two reads.
  init(X_Y_CACHED);
  GPU(g_set(x, 1));                                   // GPU:  x := 1
  thrash(PRESSURE_64MB);                              // CPU:  (64 MB of cache pressure)
  CPU(a[k] = load(x));                                // CPU:  a := x
  thrash(PRESSURE_64MB);                              // CPU:  (64 MB of cache pressure)
  CPU(b[k] = prefetcht0_load(x));                     // CPU:  prefetcht0 x; b := x
  COUNT("erratic (cache pressure between the reads)", "(a,b) = (1,0)", 'E', IS(a, 1) && IS(b, 0));
  COUNT("erratic (cache pressure between the reads)", "a = 1 (how often the first read was fresh)", 'A',
        IS(a, 1));
}

// erratic-flush
//      host 1 (CPU)       || host 2 (GPU)
//      a := x        // 1 || x := 1
//      fl,mf b := x  // 0 ||
//   (a,b) = (1,0): forbidden.
static void t_erratic_flush(void) {
  init(NOTHING_CACHED);
  GPU(g_set(x, 1));                                   // GPU:  x := 1
  CPU(a[k] = load(x);                                 // CPU:  a := x
      b[k] = clflush_mfence_load(x));                 //       clflush x; mfence; b := x
  COUNT("erratic-flush", "(a,b) = (1,0)", 'F', IS(a, 1) && IS(b, 0));
}

// write-race   (x[0] and x[1] on one cache line)
//      host 1 (GPU)       || host 2 (CPU)
//      x[0] := 1          || x[1] := 2
//      a := x[0]   // 0   || b := x[1]
//   a = 0 (the write of host 1 is lost): allowed (a cache-line race).
static void t_write_race(void) {
  init(X_Y_CACHED);
  GPU(g_set(x, 1));                                   // GPU:  x[0] := 1
  CPU(store(x + 8, 2));                               // CPU:  x[1] := 2
  CPU(clflush_mfence(x));                             // CPU:  (the line is written back: clflush x; mfence)
  GPU(g_get(r0, x));                                  // GPU:  a := x[0]
  gpu_results(a, NULL);
  COUNT("write-race", "a = 0", 'A', IS(a, 0));
}

// write-no-race   (x[0] and x[1] on one cache line)
//      host 1 (GPU)       || host 2 (CPU)
//      x[0] :=wb,sf 1     || a := Y            // 1
//      Y := 1             || fo,sf x[1] := 2
//   The write of host 1 is lost: forbidden.
// Run as: [x cached] . GPU x[0] := 1 . CPU <SEQ> (flush, fence, x[1] := 2) .
// GPU d := x[1] (diagnostic: is x[1] already in memory, i.e. did the flush
// take effect after the write?) . CPU clflush x; mfence (the line is written
// back) . GPU c := x[0]; the write is lost iff c = 0.
// SEQ is one asm block, so the flush, fence and write are adjacent.
#define WRITE_NO_RACE(name, verdict, SEQ)                                     \
  do {                                                                        \
    init(X_Y_CACHED);                                                         \
    GPU(g_set(x, 1));                         /* GPU:  x[0] := 1          */  \
    CPU(SEQ(x, 2); mfence());                 /* CPU:  SEQ; mfence        */  \
    GPU(g_get(r1, x1));                       /* GPU:  d := x[1]          */  \
    CPU(clflush_mfence(x));                   /* CPU:  clflush x; mfence  */  \
    GPU(g_get(r0, x));                        /* GPU:  c := x[0]          */  \
    gpu_results(c, d);                                                        \
    COUNT(name, "x[0] = 0 (the write of host 1 is lost)", verdict, IS(c, 0)); \
    COUNT(name, "  memory holds x[1] right after the sequence", 'A', IS(d, 2)); \
  } while (0)
static void t_write_no_race(void) {
  WRITE_NO_RACE("write-no-race (clflushopt; sfence; write)", 'F', clflushopt_sfence_store_x1);
  // VARIANTS (not paper tests):
  WRITE_NO_RACE("write-no-race without the sfence (clflushopt; write)", 'A', clflushopt_store_x1);
  WRITE_NO_RACE("write-no-race with clflush and no fence (clflush; write)", 'F', clflush_store_x1);
  WRITE_NO_RACE("write-no-race with clflush (clflush; sfence; write)", 'F', clflush_sfence_store_x1);
  WRITE_NO_RACE("write-no-race with clflush (clflush; mfence; write)", 'F', clflush_mfence_store_x1);
  WRITE_NO_RACE("write-no-race with clwb (clwb; sfence; write)", 'A', clwb_sfence_store_x1);
  // write-fl-race, with an instruction before the clflush (does a fence BEFORE the flush help?)
  WRITE_NO_RACE("write-fl-race (clflush; x[1] := 2 by lock cmpxchg)", 'F', clflush_cas_x1);
  WRITE_NO_RACE("write-fl-race (clflush; x[1] := 2 by xchg)", 'F', clflush_xchg_x1);
  WRITE_NO_RACE("write-fl-race (clflush; failing lock cmpxchg on x[1])", 'F', clflush_failing_cas_x1);
  WRITE_NO_RACE("write-fl-race (mfence; clflush; write)", 'F', mfence_clflush_store_x1);
  WRITE_NO_RACE("write-fl-race (lfence; clflush; write)", 'F', lfence_clflush_store_x1);
  WRITE_NO_RACE("write-fl-race (a := Y; clflush; write)", 'F', loady_clflush_store_x1);
  WRITE_NO_RACE("write-fl-race (a := Y; mfence; clflush; write)", 'F', loady_mfence_clflush_store_x1);
  WRITE_NO_RACE("write-fl-race (a := Y; lfence; clflush; write)", 'F', loady_lfence_clflush_store_x1);
  WRITE_NO_RACE("write-fl-race (a := Y; mfence; clflush; sfence; write)", 'F', loady_mfence_clflush_sfence_store_x1);
}

// ================================================ Appendix A of the paper

// mp-flush-nc
//      host 1 (GPU)       || host 2 (CPU)
//      x := 1             || a := y
//      clwb(x); sfence    || clflush(x); mfence
//      y := 1             || b := x   // 0
//   (a,b) = (1,0): forbidden.  (rrf-flush with clflush.)
// wrc / wrc-fl: host 2 of wrc only passes on that x = 1 is in memory, which
// the schedule does by waiting for the GPU; so wrc runs as the first schedule
// of rrf, and wrc-fl as this one. They have no rows of their own.
static void t_mp_flush_nc(void) {
  init(X_Y_CACHED);
  GPU(g_set(x, 1));                                   // GPU:  x := 1
  CPU(b[k] = clflush_mfence_load(x));                 // CPU:  clflush x; mfence; b := x
  COUNT("mp-flush-nc (this host reads; clflush, mfence)", "b = 0", 'F', IS(b, 0));
}

// corr-fl1, corr-fl2, corr-fl12
//      host 1 (GPU) || host 2 (GPU) || host 3 (CPU), one of
//      x := 1       || x := 2       || corr-fl1        corr-fl2        corr-fl12
//                   ||              || a := x          a := x          a := x
//                   ||              || clflush;mfence  b := x          clflush;mfence
//                   ||              || b := x          clflush;mfence  b := x
//                   ||              || c := x          c := x          clflush;mfence
//                   ||              ||                                 c := x
//   (a,b,c) = (1,2,1): allowed only with erratic reads in corr-fl1 and
//   corr-fl2, forbidden in corr-fl12.
// The prefetches try to make the CPU fetch the line while it already holds a
// copy, which is what an erratic read is.
static void t_corr_fl1(void) {
  init(NOTHING_CACHED);
  GPU(g_set(x, 1));                                   // GPU:  x := 1
  CPU(a[k] = load(x);                                 // CPU:  a := x
      clflush_mfence(x);                              //       clflush x; mfence
      prefetcht0(x));                                 //       prefetcht0 x
  mfence();
  GPU(g_set(x, 2));                                   // GPU:  x := 2
  CPU(prefetcht0(x);                                  // CPU:  prefetcht0 x
      b[k] = load(x);                                 //       b := x
      c[k] = load(x));                                //       c := x
  COUNT("corr-fl1", "(a,b,c) = (1,2,1)", 'E', IS(a, 1) && IS(b, 2) && IS(c, 1));
}

static void t_corr_fl2(void) {
  // x := 2 reaches memory first, then x := 1.
  init(NOTHING_CACHED);
  GPU(g_set(x, 2));                                   // GPU:  x := 2
  CPU(prefetcht0(x));                                 // CPU:  prefetcht0 x
  mfence();
  GPU(g_set(x, 1));                                   // GPU:  x := 1
  CPU(prefetcht0(x);                                  // CPU:  prefetcht0 x
      a[k] = load(x);                                 //       a := x
      b[k] = load(x);                                 //       b := x
      c[k] = clflush_mfence_load(x));                 //       clflush x; mfence; c := x
  COUNT("corr-fl2", "(a,b,c) = (1,2,1)", 'E', IS(a, 1) && IS(b, 2) && IS(c, 1));
}

static void t_corr_fl12(void) {
  init(NOTHING_CACHED);
  GPU(g_set(x, 1));                                   // GPU:  x := 1
  CPU(a[k] = load(x);                                 // CPU:  a := x
      clflush_mfence(x));                             //       clflush x; mfence
  GPU(g_set(x, 2));                                   // GPU:  x := 2
  CPU(b[k] = load(x);                                 // CPU:  b := x
      c[k] = clflush_mfence_load(x));                 //       clflush x; mfence; c := x
  COUNT("corr-fl12", "(a,b,c) = (1,2,1)", 'F', IS(a, 1) && IS(b, 2) && IS(c, 1));
}

// fl-hob-r, r-hob-fl (in part)
// The full tests need three hosts with caches. What they rely on is run: a
// flush by one thread of a host orders the reads of the OTHER threads of that
// host.
//      host 1 (GPU)       || host 2 (CPU), thread 1 | thread 2
//      x := 1             || clflush(x); mfence     | c := x   // 0
//   c = 0 after the flush: forbidden.  Thread 1 is the second core, thread 2
//   the main core.
static void t_fl_hob_r_part(void) {
  // CONTROL: no flush on the other core.  Allowed.
  init(X_Y_CACHED_ON_BOTH_CORES);
  GPU(g_set(x, 1));                                   // GPU:         x := 1
  CPU(c[k] = load(x));                                // CPU core 1:  c := x
  COUNT("flush by another thread of the host: none", "c = 0", 'A', IS(c, 0));

  init(X_Y_CACHED_ON_BOTH_CORES);
  GPU(g_set(x, 1));                                   // GPU:         x := 1
  ON_CORE2(t2_clflush_mfence_x);                      // CPU core 2:  clflush x; mfence
  CPU(c[k] = load(x));                                // CPU core 1:  c := x
  COUNT("flush by another thread of the host: clflush; mfence", "c = 0", 'F', IS(c, 0));
}

// cas-no-wb
//      host 1 (CPU)       || host 2 (GPU)
//      CAS(x,0,1)         || a := y
//      y := 1             || clflush(x); mfence
//                         || b := x   // 0
//   (a,b) = (1,0): allowed (a successful CAS reaches the cache only).
// Counted: the CAS succeeded and b = 0.
static void t_cas_no_wb(void) {
  init(X_Y_CACHED);
  CPU(cas_read[k] = cas(x, 0, 1));                    // CPU:  CAS(x,0,1)
  GPU(g_get(r0, x));                                  // GPU:  b := x
  gpu_results(b, NULL);
  COUNT("cas-no-wb", "b = 0", 'A', IS(cas_read, 0) && IS(b, 0));
}

// cas-two-events
//      host 1 (GPU) || host 2 (CPU)           || host 3 (GPU)
//      x := 1       || a := CAS(x,1,2)  // 1  || b := y   // 1
//                   || y := 1                 || clflush(x); mfence
//                   ||                        || c := x
//   (a,b,c) = (1,1,0): forbidden.  (1,1,1): allowed.
static void t_cas_two_events(void) {
  init(NOTHING_CACHED);
  GPU(g_set(x, 1));                                   // GPU:  x := 1
  CPU(a[k] = cas(x, 1, 2));                           // CPU:  a := CAS(x,1,2)
  GPU(g_get(r0, x));                                  // GPU:  c := x
  gpu_results(c, NULL);
  COUNT("cas-two-events", "(a,c) = (1,0)", 'F', IS(a, 1) && IS(c, 0));
  COUNT("cas-two-events", "(a,c) = (1,1)", 'A', IS(a, 1) && IS(c, 1));
}

// cas-host-atomic
//      host 1 (CPU), thread 1 | thread 2           || host 2 (GPU)
//      CAS(x,0,1)             | a := y    // 1     || x := 2
//                             | clflush(x); mfence || clwb(x); sfence
//                             | b := x    // 2     || y := 1
//                             | clflush(x); mfence ||
//                             | c := x    // 1     ||
//   (a,b,c) = (1,2,1): forbidden.
// Thread 1 (the CAS) is the second core, started at a random moment per cell;
// thread 2 is the main core, running concurrently. This is the one test that
// relies on a real race between two cores.
static void t_cas_host_atomic(void) {
  init(X_Y_CACHED_ON_BOTH_CORES);
  GPU(g_set(x, 2));                                   // GPU:         x := 2
  START_ON_CORE2(t2_cas_x_0_1_after_random_delay);    // CPU core 2:  CAS(x,0,1), concurrently with
  CPU(b[k] = clflush_mfence_load(x);                  // CPU core 1:  clflush x; mfence; b := x
      c[k] = clflush_mfence_load(x));                 //              clflush x; mfence; c := x
  JOIN_CORE2();
  COUNT("cas-host-atomic", "(b,c) = (2,1)", 'F', IS(b, 2) && IS(c, 1));
  COUNT("cas-host-atomic", "(b,c) = (1,1): the CAS ran first", 'A', IS(b, 1) && IS(c, 1));
  COUNT("cas-host-atomic", "(b,c) = (2,2): the CAS ran later and failed", 'A', IS(b, 2) && IS(c, 2));
}

// cas-erratic
//      host 1 (GPU) || host 2 (CPU), thread 1 | thread 2  || host 3
//      x := 1       || a := CAS(x,0,2)  // 0  | b := x // 1 || c := x  // 1
//                   ||                        |            || clflush(x); mfence
//                   ||                        |            || d := x  // 2
//   (a,b,c,d) = (0,1,1,2): allowed only with erratic reads. Host 3 is not run;
//   (a,b) = (0,1) is counted. Thread 1 is the main core, thread 2 the second.
static void t_cas_erratic(void) {
  init(X_Y_CACHED_ON_BOTH_CORES);
  GPU(g_set(x, 1));                                   // GPU:         x := 1
  thrash(PRESSURE_64MB);                              // CPU:         (64 MB of cache pressure)
  ON_CORE2(t2_b_load_x);                              // CPU core 2:  b := x
  CPU(a[k] = cas(x, 0, 2));                           // CPU core 1:  a := CAS(x,0,2)
  COUNT("cas-erratic", "(a,b) = (0,1)", 'E', IS(a, 0) && IS(b, 1));
}

// ner-lost
//      host 1 (GPU)       || host 2 (CPU)
//      x := 1             || a := y   // 1
//      x := 2             || b := x   // 0
//      clflush(x)         || c := x   // 1
//      y := 1             ||
//   (a,b,c) = (1,0,1): allowed only with erratic reads.
static void t_ner_lost(void) {
  init(X_Y_CACHED);
  GPU(g_set(x, 1));                                   // GPU:  x := 1
  CPU(prefetcht0(x));                                 // CPU:  prefetcht0 x
  mfence();
  GPU(g_set(x, 2));                                   // GPU:  x := 2
  CPU(b[k] = load(x);                                 // CPU:  b := x
      c[k] = load(x));                                //       c := x
  COUNT("ner-lost", "(b,c) = (0,1)", 'E', IS(b, 0) && IS(c, 1));
}

// ner-fwd: not run (all its threads are on one host; it is ordinary x86-TSO).

// ============================================================== main
int main(int argc, char **argv) {
  int rounds = argc > 1 ? atoi(argv[1]) : 100;
  coherent = getenv("COHERENT") && atoi(getenv("COHERENT"));
  int cpu = pin_cpu();
  int cpu2 = getenv("CPU2") ? atoi(getenv("CPU2")) : cpu + 1;
  nc_open(&g, coherent ? 3 : 2);
  thrash_buf = aligned_alloc(4096, THRASH_BYTES);
  memset(thrash_buf, 1, THRASH_BYTES);
  pthread_t th;
  if (pthread_create(&th, NULL, worker, (void *)(intptr_t)cpu2)) die("pthread_create");

  // Self-test of the GPU read: a value this CPU wrote and flushed must be read back.
  init(NOTHING_CACHED);
  CPU(store(x, 7));
  clflush_range(g.t, (size_t)LINES * LINE);
  GPU(g_get(r1, x));
  gpu_results(NULL, a);
  for (int k = 0; k < NT; k++)
    if (!IS(a, 7)) { fprintf(stderr, "GPU read self-test failed (cell %d: %08x)\n", k, a[k]); return 2; }

  int only_wnr = getenv("ONLY_WRITE_NO_RACE") != NULL;   // run just the write-no-race family
  for (int r = 0; r < rounds; r++) {
    if (only_wnr) { t_write_no_race(); continue; }
    // Section 2, in the paper's order
    t_cas_nc(); t_rrf(); t_rrf_flush(); t_sb(); t_far(); t_cleanwb(); t_erratic();
    t_erratic_flush(); t_write_race(); t_write_no_race();
    // Appendix A, in the paper's order
    t_mp_flush_nc(); t_corr_fl1(); t_corr_fl2(); t_corr_fl12(); t_fl_hob_r_part();
    t_cas_no_wb(); t_cas_two_events(); t_cas_host_atomic(); t_cas_erratic(); t_ner_lost();
  }

  printf("cxl_litmus: cpus %d and %d, %s (GPU PAT %d), %d rounds\n\n", cpu, cpu2,
         coherent ? "COHERENT control" : "non-coherent", coherent ? 3 : 2, rounds);
  printf("%-58s %-46s %-12s %9s %9s\n", "test", "outcome", "model", "observed", "trials");
  int bad = 0;
  for (int i = 0; i < nrows; i++) {
    const char *vd = rows[i].verdict == 'A' ? "allowed" : rows[i].verdict == 'F' ? "forbidden"
                   : rows[i].verdict == 'E' ? "erratic only" : "forbidden";
    int viol = (rows[i].verdict == 'F' || rows[i].verdict == 'C') && rows[i].hits;
    printf("%-58s %-46s %-12s %9ld %9ld%s\n", rows[i].test, rows[i].outcome, vd, rows[i].hits,
           rows[i].trials, viol ? "   <-- FORBIDDEN OUTCOME OBSERVED" : "");
    bad |= viol;
  }
  printf("\n%s\n", coherent ? "Coherent control run." :
         bad ? "A forbidden outcome was observed." : "No forbidden outcome was observed.");
  return bad && !coherent ? 3 : 0;
}
