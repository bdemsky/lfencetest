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
// test is run on NT independent cells (one x line, one y line and one line of
// result slots each), for a number of rounds. LITMUS.md gives, for every test,
// the program as in the paper, the schedule used here, and the result.
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

static inline uint32_t V(uint32_t v) { return 0x0a000000u | (epoch << 4) | v; }
static inline uint64_t XOFF(int k) { return (uint64_t)(4 * k) * LINE; }
static inline uint64_t YOFF(int k) { return (uint64_t)(4 * k + 1) * LINE; }
static inline uint64_t ROFF(int k, int j) { return (uint64_t)(4 * k + 2) * LINE + 4 * j; }
static inline volatile uint32_t *XP(int k) { return &g.t[XOFF(k) / 4]; }
static inline volatile uint32_t *YP(int k) { return &g.t[YOFF(k) / 4]; }
static inline volatile uint32_t *RP(int k) { return &g.t[ROFF(k, 0) / 4]; }

// ------------------------------------------------------------------ GPU side
static uint32_t *bp;
static void gb_begin(void) { bp = g.b; }
static void gb_store(uint64_t off, uint32_t val) {
  uint64_t a = TARGET_VA + off;
  *bp++ = MI_STORE_DWORD_IMM; *bp++ = (uint32_t)a; *bp++ = (uint32_t)(a >> 32); *bp++ = val;
}
static void gb_copy(uint64_t dst, uint64_t src) {
  uint64_t d = TARGET_VA + dst, s = TARGET_VA + src;
  *bp++ = MI_COPY_MEM_MEM; *bp++ = (uint32_t)d; *bp++ = (uint32_t)(d >> 32);
  *bp++ = (uint32_t)s; *bp++ = (uint32_t)(s >> 32); *bp++ = MI_NOOP;
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
#define GPU(body) do { gb_begin(); for (int k = 0; k < NT; k++) { body; } gb_run(); } while (0)
#define G_WRITE_X(v) GPU(gb_store(XOFF(k), V(v)))          /* x := v, in memory        */
#define G_WRITE_Y(v) GPU(gb_store(YOFF(k), V(v)))          /* y := v, in memory        */
#define G_READ_X(j)  GPU(gb_copy(ROFF(k, j), XOFF(k)))     /* result j := x, from memory */

// Results the GPU stored: invalidate the result lines, then read them.
static uint32_t res[NT][4];
static void get_res(void) {
  for (int k = 0; k < NT; k++) asm volatile("clflushopt (%0)" ::"r"(RP(k)) : "memory");
  asm volatile("mfence" ::: "memory");
  for (int k = 0; k < NT; k++)
    for (int j = 0; j < 4; j++) res[k][j] = RP(k)[j];
}

// ------------------------------------------------------------------ CPU side
#define OP1(name, body)                                                       \
  static inline uint32_t name(volatile uint32_t *p) {                         \
    uint32_t v;                                                               \
    asm volatile(body : "=&r"(v) : "r"(p) : "memory", "cc");                  \
    return v;                                                                 \
  }
OP1(ld, "mov (%1), %0")
OP1(fo_mf_ld, "clflushopt (%1)\n\tmfence\n\tmov (%1), %0")
OP1(fl_mf_ld, "clflush (%1)\n\tmfence\n\tmov (%1), %0")
OP1(wb_mf_ld, "clwb (%1)\n\tmfence\n\tmov (%1), %0")
OP1(pf_ld, "prefetcht0 (%1)\n\tmov (%1), %0")
#define OPW(name, body)                                                       \
  static inline void name(volatile uint32_t *p, uint32_t v) {                 \
    asm volatile(body ::"r"(p), "r"(v) : "memory", "cc");                     \
  }
OPW(st, "mov %1, (%0)")
OPW(st_wb_sf, "mov %1, (%0)\n\tclwb (%0)\n\tsfence")
OPW(st_wb_mf, "mov %1, (%0)\n\tclwb (%0)\n\tmfence")
// the second host of write-no-race and its variants; p = the line, p+8 = x[1]
OPW(fo_sf_st1, "clflushopt (%0)\n\tsfence\n\tmov %1, 32(%0)")
OPW(fo_st1, "clflushopt (%0)\n\tmov %1, 32(%0)")
OPW(fl_st1, "clflush (%0)\n\tmov %1, 32(%0)")
OPW(wb_sf_st1, "clwb (%0)\n\tsfence\n\tmov %1, 32(%0)")
OPW(fl_sf_st1, "clflush (%0)\n\tsfence\n\tmov %1, 32(%0)")
OPW(fl_mf_st1, "clflush (%0)\n\tmfence\n\tmov %1, 32(%0)")
static inline void op_fl_mf(volatile uint32_t *p) { asm volatile("clflush (%0)\n\tmfence" ::"r"(p) : "memory"); }
static inline void op_fo_mf(volatile uint32_t *p) { asm volatile("clflushopt (%0)\n\tmfence" ::"r"(p) : "memory"); }
static inline void op_wb_sf(volatile uint32_t *p) { asm volatile("clwb (%0)\n\tsfence" ::"r"(p) : "memory"); }
static inline void op_wb_mf(volatile uint32_t *p) { asm volatile("clwb (%0)\n\tmfence" ::"r"(p) : "memory"); }
static inline void op_pf(volatile uint32_t *p) { asm volatile("prefetcht0 (%0)" ::"r"(p) : "memory"); }
static inline void mf(void) { asm volatile("mfence" ::: "memory"); }
// CAS(x, e, n): returns the value read; it succeeded iff that value is e.
static inline uint32_t cas(volatile uint32_t *p, uint32_t e, uint32_t n) {
  asm volatile("lock cmpxchg %2, (%1)" : "+a"(e) : "r"(p), "r"(n) : "memory", "cc");
  return e;
}

#define THRASH_BYTES (192u << 20)
static char *thrash_buf;
static volatile uint64_t sink;
static void thrash(size_t len) {
  uint64_t s = 0;
  for (size_t i = 0; i < len; i += LINE) s += (unsigned char)thrash_buf[i % THRASH_BYTES];
  sink += s;
}

// --------------------------------------- second thread of the host (another core)
static void (*volatile w_fn)(void);
static volatile int w_seq, w_done;
static uint32_t wv[NT];              // values read by the second thread
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
static void wstart(void (*fn)(void)) { w_fn = fn; __atomic_add_fetch(&w_seq, 1, __ATOMIC_RELEASE); }
static void wjoin(void) {
  while (__atomic_load_n(&w_done, __ATOMIC_ACQUIRE) != __atomic_load_n(&w_seq, __ATOMIC_ACQUIRE))
    asm volatile("pause");
}
static void wrun(void (*fn)(void)) { wstart(fn); wjoin(); }
static void w_touch(void) { for (int k = 0; k < NT; k++) { (void)ld(XP(k)); (void)ld(YP(k)); } }
static void w_read_x(void) { for (int k = 0; k < NT; k++) wv[k] = ld(XP(k)); }
static void w_read_x_wb(void) { for (int k = 0; k < NT; k++) { wv[k] = ld(XP(k)); op_wb_sf(XP(k)); } }
static void w_flush_x(void) { for (int k = 0; k < NT; k++) op_fl_mf(XP(k)); }
static void w_cas_0_1(void) {
  unsigned s = 12345;
  for (int k = 0; k < NT; k++) {
    s = s * 1103515245u + 12345u;
    for (unsigned d = (s >> 16) % 24; d; d--) asm volatile("pause");
    wv[k] = cas(XP(k), V(0), V(1));
  }
}

// Start of every test: x = y = 0 in memory. If `cached`, the main core (and
// the second core if `cached2`) holds a clean copy of x and y: the host
// "has fetched the lines earlier".
static void init(int cached, int cached2) {
  epoch++;
  for (int k = 0; k < NT; k++) {
    XP(k)[0] = V(0); XP(k)[8] = V(0); YP(k)[0] = V(0);
    for (int j = 0; j < 4; j++) RP(k)[j] = 0xdead0000u;
  }
  clflush_range(g.t, (size_t)LINES * LINE);
  if (cached) for (int k = 0; k < NT; k++) { (void)ld(XP(k)); (void)ld(YP(k)); }
  mf();
  if (cached2) wrun(w_touch);
}

// ------------------------------------------------------------------ reporting
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
#define COUNT(test, outcome, verdict, cond)                                   \
  do { long *h_ = row(test, outcome, verdict);                                \
       for (int k = 0; k < NT; k++) { h_[1]++; if (cond) h_[0]++; } } while (0)

static uint32_t a[NT], b[NT], c[NT];

// =========================================================== the litmus tests
static void t_rrf(void) {
  // reader side: this host fetched x earlier; the other host writes x
  init(1, 0);
  G_WRITE_X(1);
  for (int k = 0; k < NT; k++) b[k] = ld(XP(k));
  COUNT("rrf (this host reads)", "b = 0", 'A', b[k] == V(0));
  // writer side: this host writes x and does not write it back
  init(1, 0);
  for (int k = 0; k < NT; k++) st(XP(k), V(1));
  mf();
  G_READ_X(0);
  get_res();
  COUNT("rrf (this host writes)", "b = 0", 'A', res[k][0] == V(0));
}

static void t_rrf_flush(void) {
  init(1, 0);
  G_WRITE_X(1);
  for (int k = 0; k < NT; k++) b[k] = fo_mf_ld(XP(k));
  COUNT("rrf-flush (this host reads; clflushopt, mfence)", "b = 0", 'F', b[k] == V(0));
  init(1, 0);
  G_WRITE_X(1);
  for (int k = 0; k < NT; k++) b[k] = fl_mf_ld(XP(k));
  COUNT("mp-flush-nc (this host reads; clflush, mfence)", "b = 0", 'F', b[k] == V(0));
  init(1, 0);
  G_WRITE_X(1);
  for (int k = 0; k < NT; k++) b[k] = wb_mf_ld(XP(k));
  COUNT("rrf-flush with clwb in the reader", "b = 0", 'A', b[k] == V(0));
  init(1, 0);
  for (int k = 0; k < NT; k++) st_wb_sf(XP(k), V(1));
  G_READ_X(0);
  get_res();
  COUNT("rrf-flush (this host writes; clwb, sfence)", "b = 0", 'F', res[k][0] == V(0));
}

static void t_sb(void) {
  // without flushes: this host writes x, the other writes y and reads x,
  // then this host reads y from the copy it fetched earlier
  init(1, 0);
  for (int k = 0; k < NT; k++) st(XP(k), V(1));
  mf();
  GPU(gb_store(YOFF(k), V(1)); gb_copy(ROFF(k, 0), XOFF(k)));
  for (int k = 0; k < NT; k++) a[k] = ld(YP(k));
  get_res();
  COUNT("sb without flushes", "(a,b) = (0,0)", 'A', a[k] == V(0) && res[k][0] == V(0));

  // with flushes: all 10 interleavings of
  //   this host:  c1 = x:=1; clwb x; mfence   c2 = clflushopt y; mfence   c3 = a:=y
  //   other host: g1 = y:=1 (in memory)       g2 = b:=x (from memory)
  for (int p1 = 0; p1 < 5; p1++)
    for (int p2 = p1 + 1; p2 < 5; p2++) {
      init(1, 0);
      int cstep = 0;
      for (int pos = 0; pos < 5; pos++) {
        if (pos == p1) { G_WRITE_Y(1); continue; }
        if (pos == p2) { G_READ_X(0); continue; }
        if (cstep == 0) for (int k = 0; k < NT; k++) st_wb_mf(XP(k), V(1));
        if (cstep == 1) for (int k = 0; k < NT; k++) op_fo_mf(YP(k));
        if (cstep == 2) for (int k = 0; k < NT; k++) a[k] = ld(YP(k));
        cstep++;
      }
      get_res();
      COUNT("sb (all 10 interleavings)", "(a,b) = (0,0)", 'F', a[k] == V(0) && res[k][0] == V(0));
    }
}

static void t_far(void) {
  // host 2 = this CPU: thread 2 (main core) writes x, thread 3 (second core)
  // reads it. host 1 = GPU reads x from memory.
  init(1, 1);
  for (int k = 0; k < NT; k++) st(XP(k), V(1));
  mf();                               // the store has reached the host's cache
  wrun(w_read_x);                     // c := x on the second core
  G_READ_X(0);                        // b := x by the other host
  for (int k = 0; k < NT; k++) op_wb_mf(XP(k));  // thread 2's clwb; mfence, too late
  get_res();
  COUNT("far", "(b,c) = (0,1)", 'A', res[k][0] == V(0) && wv[k] == V(1));

  init(1, 1);
  for (int k = 0; k < NT; k++) st(XP(k), V(1));
  mf();
  wrun(w_read_x_wb);                  // c := x; clwb x; sfence
  G_READ_X(0);
  get_res();
  COUNT("far with a writeback after the read of c", "(b,c) = (0,1)", 'F',
        res[k][0] == V(0) && wv[k] == V(1));
}

static void t_cleanwb(void) {
  // "second host" = this CPU, holding a clean copy of x = 0.
  // first host = GPU: x := 1, and later a := x.
  static const char *nm[4] = {"cleanwb (second host runs clwb)", "cleanwb (second host runs clflushopt)",
                              "cleanwb (second host runs clflush)", "cleanwb (second host evicts the line)"};
  for (int v = 0; v < 4; v++) {
    init(1, 0);
    G_WRITE_X(1);
    for (int k = 0; k < NT; k++) {
      if (v == 0) op_wb_sf(XP(k));
      if (v == 1) op_fo_mf(XP(k));
      if (v == 2) op_fl_mf(XP(k));
    }
    if (v == 3) thrash(THRASH_BYTES);
    G_READ_X(0);
    get_res();
    COUNT(nm[v], "a = 0", 'F', res[k][0] == V(0));
  }
}

static void t_erratic(void) {
  init(1, 0);
  G_WRITE_X(1);
  for (int k = 0; k < NT; k++) { a[k] = pf_ld(XP(k)); b[k] = ld(XP(k)); }
  COUNT("erratic (reads back to back)", "(a,b) = (1,0)", 'E', a[k] == V(1) && b[k] == V(0));
  init(1, 0);
  G_WRITE_X(1);
  thrash(64u << 20);                  // some lines leave the cache, so a can be 1
  for (int k = 0; k < NT; k++) a[k] = ld(XP(k));
  thrash(64u << 20);
  for (int k = 0; k < NT; k++) b[k] = pf_ld(XP(k));
  COUNT("erratic (cache pressure between the reads)", "(a,b) = (1,0)", 'E', a[k] == V(1) && b[k] == V(0));
  COUNT("erratic (cache pressure between the reads)", "a = 1 (how often the first read was fresh)", 'A',
        a[k] == V(1));

  init(0, 0);
  G_WRITE_X(1);
  for (int k = 0; k < NT; k++) { a[k] = ld(XP(k)); b[k] = fl_mf_ld(XP(k)); }
  COUNT("erratic-flush", "(a,b) = (1,0)", 'F', a[k] == V(1) && b[k] == V(0));
}

static void t_corr(void) {
  // corr-fl1: x:=1 || x:=2 || a:=x; clflush x; mfence; b:=x; c:=x
  init(0, 0);
  G_WRITE_X(1);
  for (int k = 0; k < NT; k++) { a[k] = ld(XP(k)); op_fl_mf(XP(k)); op_pf(XP(k)); }
  mf();
  G_WRITE_X(2);
  for (int k = 0; k < NT; k++) { op_pf(XP(k)); b[k] = ld(XP(k)); c[k] = ld(XP(k)); }
  COUNT("corr-fl1", "(a,b,c) = (1,2,1)", 'E', a[k] == V(1) && b[k] == V(2) && c[k] == V(1));
  // corr-fl2: x:=1 || x:=2 || a:=x; b:=x; clflush x; mfence; c:=x   (2 reaches memory first)
  init(0, 0);
  G_WRITE_X(2);
  for (int k = 0; k < NT; k++) op_pf(XP(k));
  mf();
  G_WRITE_X(1);
  for (int k = 0; k < NT; k++) { op_pf(XP(k)); a[k] = ld(XP(k)); b[k] = ld(XP(k)); c[k] = fl_mf_ld(XP(k)); }
  COUNT("corr-fl2", "(a,b,c) = (1,2,1)", 'E', a[k] == V(1) && b[k] == V(2) && c[k] == V(1));
  // corr-fl12: a:=x; clflush; mfence; b:=x; clflush; mfence; c:=x
  init(0, 0);
  G_WRITE_X(1);
  for (int k = 0; k < NT; k++) { a[k] = ld(XP(k)); op_fl_mf(XP(k)); }
  G_WRITE_X(2);
  for (int k = 0; k < NT; k++) { b[k] = ld(XP(k)); c[k] = fl_mf_ld(XP(k)); }
  COUNT("corr-fl12", "(a,b,c) = (1,2,1)", 'F', a[k] == V(1) && b[k] == V(2) && c[k] == V(1));
  // ner-lost: x:=1; x:=2; clflush x; y:=1 || a:=y; b:=x; c:=x
  init(1, 0);
  G_WRITE_X(1);
  for (int k = 0; k < NT; k++) op_pf(XP(k));
  mf();
  G_WRITE_X(2);
  for (int k = 0; k < NT; k++) { b[k] = ld(XP(k)); c[k] = ld(XP(k)); }
  COUNT("ner-lost", "(b,c) = (0,1)", 'E', b[k] == V(0) && c[k] == V(1));
}

static void t_write_race(void) {
  // host 1 = GPU: x[0] := 1, later a := x[0].   host 2 = this CPU: x[1] := 2.
  init(1, 0);
  G_WRITE_X(1);
  for (int k = 0; k < NT; k++) st(XP(k) + 8, V(2));
  for (int k = 0; k < NT; k++) op_fl_mf(XP(k));   // the line is written back at some point
  G_READ_X(0);
  get_res();
  COUNT("write-race", "a = 0", 'A', res[k][0] == V(0));

  static const char *nm[6] = {"write-no-race (clflushopt; sfence; write)",
                              "write-no-race without the sfence (clflushopt; write)",
                              "write-no-race with clflush and no fence (clflush; write)",
                              "write-no-race with clflush (clflush; sfence; write)",
                              "write-no-race with clflush (clflush; mfence; write)",
                              "write-no-race with clwb (clwb; sfence; write)"};
  static const char vd[6] = {'F', 'A', 'F', 'F', 'F', 'A'};
  for (int v = 0; v < 6; v++) {
    init(1, 0);
    G_WRITE_X(1);
    for (int k = 0; k < NT; k++) {
      if (v == 0) fo_sf_st1(XP(k), V(2));
      if (v == 1) fo_st1(XP(k), V(2));
      if (v == 2) fl_st1(XP(k), V(2));
      if (v == 3) fl_sf_st1(XP(k), V(2));
      if (v == 4) fl_mf_st1(XP(k), V(2));
      if (v == 5) wb_sf_st1(XP(k), V(2));
      mf();
    }
    // Diagnostic, before any further flush: does memory already hold x[1]?
    // If so, the flush took effect AFTER the write that follows it.
    GPU(gb_copy(ROFF(k, 1), XOFF(k) + 32));
    for (int k = 0; k < NT; k++) op_fl_mf(XP(k));
    G_READ_X(0);
    get_res();
    COUNT(nm[v], "x[0] = 0 (the write of host 1 is lost)", vd[v], res[k][0] == V(0));
    COUNT(nm[v], "  memory holds x[1] right after the sequence", 'A', res[k][1] == V(2));
  }
}

static void t_cas(void) {
  // cas-nc: the other host's successful CAS is the write x := 1 in memory;
  // this host still holds x = 0 and runs CAS(x,0,1) (it writes 3 so that the
  // two writes can be told apart).
  init(1, 0);
  G_WRITE_X(1);
  for (int k = 0; k < NT; k++) a[k] = cas(XP(k), V(0), V(3));
  COUNT(coherent ? "cas (coherent memory)" : "cas-nc", "both CAS succeed", coherent ? 'C' : 'A',
        a[k] == V(0));

  // cas-no-wb: CAS(x,0,1); y:=1 || a:=y; clflush x; mfence; b:=x
  init(1, 0);
  for (int k = 0; k < NT; k++) a[k] = cas(XP(k), V(0), V(1));
  G_READ_X(0);
  get_res();
  COUNT("cas-no-wb", "b = 0", 'A', a[k] == V(0) && res[k][0] == V(0));

  // cas-two-events: x:=1 || a:=CAS(x,1,2); y:=1 || b:=y; clflush x; mfence; c:=x
  init(0, 0);
  G_WRITE_X(1);
  for (int k = 0; k < NT; k++) a[k] = cas(XP(k), V(1), V(2));
  G_READ_X(0);
  get_res();
  COUNT("cas-two-events", "(a,c) = (1,0)", 'F', a[k] == V(1) && res[k][0] == V(0));
  COUNT("cas-two-events", "(a,c) = (1,1)", 'A', a[k] == V(1) && res[k][0] == V(1));

  // cas-host-atomic: CAS(x,0,1) | a:=y; fl; mf; b:=x; fl; mf; c:=x || x:=2; clwb; sfence; y:=1
  // The second core runs the CAS at a random moment while the main core runs
  // the two flushed reads.
  init(1, 1);
  G_WRITE_X(2);
  wstart(w_cas_0_1);
  for (int k = 0; k < NT; k++) { b[k] = fl_mf_ld(XP(k)); c[k] = fl_mf_ld(XP(k)); }
  wjoin();
  COUNT("cas-host-atomic", "(b,c) = (2,1)", 'F', b[k] == V(2) && c[k] == V(1));
  COUNT("cas-host-atomic", "(b,c) = (1,1): the CAS ran first", 'A', b[k] == V(1) && c[k] == V(1));
  COUNT("cas-host-atomic", "(b,c) = (2,2): the CAS ran later and failed", 'A', b[k] == V(2) && c[k] == V(2));

  // cas-erratic: x:=1 || a:=CAS(x,0,2) | b:=x || ...   on one host, a CAS that
  // still sees 0 while another thread of the host already sees 1
  init(1, 1);
  G_WRITE_X(1);
  thrash(64u << 20);
  wrun(w_read_x);
  for (int k = 0; k < NT; k++) a[k] = cas(XP(k), V(0), V(2));
  COUNT("cas-erratic", "(a,b) = (0,1)", 'E', a[k] == V(0) && wv[k] == V(1));
}

static void t_host_flush(void) {
  // fl-hob-r / r-hob-fl rely on a flush by one thread of a host acting on the
  // reads of the other threads of that host.
  init(1, 1);
  G_WRITE_X(1);
  for (int k = 0; k < NT; k++) c[k] = ld(XP(k));
  COUNT("flush by another thread of the host: none", "c = 0", 'A', c[k] == V(0));
  init(1, 1);
  G_WRITE_X(1);
  wrun(w_flush_x);                    // clflush x; mfence on the second core
  for (int k = 0; k < NT; k++) c[k] = ld(XP(k));
  COUNT("flush by another thread of the host: clflush; mfence", "c = 0", 'F', c[k] == V(0));
}

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
  init(0, 0);
  for (int k = 0; k < NT; k++) st(XP(k), V(7));
  clflush_range(g.t, (size_t)LINES * LINE);
  G_READ_X(1);
  get_res();
  for (int k = 0; k < NT; k++)
    if (res[k][1] != V(7)) { fprintf(stderr, "GPU read self-test failed (cell %d: %08x)\n", k, res[k][1]); return 2; }

  for (int r = 0; r < rounds; r++) {
    t_rrf(); t_rrf_flush(); t_sb(); t_far(); t_cleanwb(); t_erratic(); t_corr();
    t_write_race(); t_cas(); t_host_flush();
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
