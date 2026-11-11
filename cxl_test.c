// cxl_test.c - Hardware checks for the assumptions of the CXL-TSO model, using
// the iGPU as a non-coherent writer (see gpu_nc.h and lfence_test.c).
//
// Every round starts from the same state, built by nc_make_stale():
//     DRAM = NEW (written by the GPU), CPU cache = clean copy of OLD.
//
// Part A, fence matrix. "FLUSH x; <fence>; load x" for each flush instruction
//   and each candidate fence. A load that returns OLD was served from the line
//   the flush should have removed. The model says: no fence, sfence and lfence
//   do not order the load after the flush (stale allowed); mfence and a locked
//   instruction do (stale forbidden).
//
// Part B, clean writebacks. Apply an action to a line whose cached copy is
//   CLEAN, then read DRAM (clflushopt; mfence; load). If DRAM holds OLD again,
//   the CPU wrote a line back that the program never modified.
//
// Part C, erratic reads. Read each line repeatedly while the caches are put
//   under pressure. The values read must go OLD..OLD NEW..NEW. A NEW followed
//   by an OLD, with no flush in between, is an erratic read.
//
// Part D, two demonstrations: a CAS that succeeds against a stale copy, and a
//   write to one word of a line that loses the GPU's write to another word.
//
// Usage: ./cxl_test [rounds=300]      env CPU=<n>, COHERENT=1 as in lfence_test
#include "gpu_nc.h"

#define OLD_TAG 0x0a000000u
#define NEW_TAG 0x0e000000u

static volatile uint32_t *scratch;          // a line that is not in the cache
static volatile uint32_t lockvar __attribute__((aligned(64)));  // always cached

// ---------------------------------------------------------------------------
// Part A sequences. %1 = test line, %2 = scratch line, %3 = lockvar.
// ---------------------------------------------------------------------------
#define SEQ(name, body)                                                       \
  static uint32_t name(volatile uint32_t *p) {                                \
    uint32_t v;                                                               \
    asm volatile(body : "=&r"(v) : "r"(p), "r"(scratch), "r"(&lockvar)        \
                 : "memory", "cc");                                           \
    return v;                                                                 \
  }
#define ST "movl $1, (%2)\n\t"
#define LD "mov (%1), %0"
#define LOCKED "lock addl $0, (%3)\n\t"

SEQ(a_noflush, LD)
#define FAMILY(tag, FL)                                                       \
  SEQ(a_##tag##_none, FL " (%1)\n\t" LD)                                      \
  SEQ(a_##tag##_sf, FL " (%1)\n\tsfence\n\t" LD)                              \
  SEQ(a_##tag##_lf, FL " (%1)\n\tlfence\n\t" LD)                              \
  SEQ(a_##tag##_mf, FL " (%1)\n\tmfence\n\t" LD)                              \
  SEQ(a_##tag##_lk, FL " (%1)\n\t" LOCKED LD)                                 \
  SEQ(a_st_##tag##_none, ST FL " (%1)\n\t" LD)                                \
  SEQ(a_st_##tag##_sf, ST FL " (%1)\n\tsfence\n\t" LD)                        \
  SEQ(a_st_##tag##_lf, ST FL " (%1)\n\tlfence\n\t" LD)                        \
  SEQ(a_st_##tag##_mf, ST FL " (%1)\n\tmfence\n\t" LD)                        \
  SEQ(a_st_##tag##_lk, ST FL " (%1)\n\t" LOCKED LD)
FAMILY(fo, "clflushopt")
FAMILY(fl, "clflush")

// expect: 0 = control, must be 0 stale (mfence)
//         1 = control, must be ~100% stale (no flush)
//         2 = test, model ALLOWS a stale load
//         3 = test, model FORBIDS a stale load (locked instruction)
struct seq { const char *name; uint32_t (*fn)(volatile uint32_t *); int expect; };
#define ROWS(tag, nm)                                                         \
  {nm "; load", a_##tag##_none, 2},                                           \
  {nm "; sfence; load", a_##tag##_sf, 2},                                     \
  {nm "; lfence; load", a_##tag##_lf, 2},                                     \
  {nm "; mfence; load", a_##tag##_mf, 0},                                     \
  {nm "; lock add; load", a_##tag##_lk, 3},                                   \
  {"store-miss; " nm "; load", a_st_##tag##_none, 2},                         \
  {"store-miss; " nm "; sfence; load", a_st_##tag##_sf, 2},                   \
  {"store-miss; " nm "; lfence; load", a_st_##tag##_lf, 2},                   \
  {"store-miss; " nm "; mfence; load", a_st_##tag##_mf, 0},                   \
  {"store-miss; " nm "; lock add; load", a_st_##tag##_lk, 3},
static const struct seq seqs[] = {
    {"load (no flush)", a_noflush, 1},
    ROWS(fo, "clflushopt")
    ROWS(fl, "clflush")
};
#define NSEQ (int)(sizeof(seqs) / sizeof(seqs[0]))

// Read what DRAM holds for a line: invalidate, wait, load.
static uint32_t dram(volatile uint32_t *p) {
  uint32_t v;
  asm volatile("clflushopt (%1)\n\tmfence\n\tmov (%1), %0" : "=r"(v) : "r"(p) : "memory");
  return v;
}

// ---------------------------------------------------------------------------
// Part B actions, applied to a line whose cached copy is clean OLD.
// ---------------------------------------------------------------------------
static void b_none(volatile uint32_t *p, uint32_t old) { (void)p; (void)old; }
static void b_clwb(volatile uint32_t *p, uint32_t old) {
  (void)old;
  asm volatile("clwb (%0)\n\tsfence" ::"r"(p) : "memory");
}
static void b_clflushopt(volatile uint32_t *p, uint32_t old) {
  (void)old;
  asm volatile("clflushopt (%0)\n\tmfence" ::"r"(p) : "memory");
}
static void b_clflush(volatile uint32_t *p, uint32_t old) {
  (void)old;
  asm volatile("clflush (%0)\n\tmfence" ::"r"(p) : "memory");
}
static void b_prefetchw(volatile uint32_t *p, uint32_t old) {
  (void)old;
  asm volatile("prefetchw (%0)\n\tmfence" ::"r"(p) : "memory");
}
// A compare-and-swap that FAILS: the comparand never matches OLD or NEW.
static void b_lock_cas_fail(volatile uint32_t *p, uint32_t old) {
  uint32_t expect = 0x55555555u, newv = 0x77777777u;
  (void)old;
  asm volatile("lock cmpxchg %2, (%1)" : "+a"(expect) : "r"(p), "r"(newv) : "memory", "cc");
}
static void b_cas_fail(volatile uint32_t *p, uint32_t old) {
  uint32_t expect = 0x55555555u, newv = 0x77777777u;
  (void)old;
  asm volatile("cmpxchg %2, (%1)" : "+a"(expect) : "r"(p), "r"(newv) : "memory", "cc");
}
// The same failing CAS, aimed at a DIFFERENT word of the line (word 8, 32 bytes
// in) from the one the GPU wrote (word 0).
static void b_lock_cas_fail_w8(volatile uint32_t *p, uint32_t old) { b_lock_cas_fail(p + 8, old); }
static void b_cas_fail_w8(volatile uint32_t *p, uint32_t old) { b_cas_fail(p + 8, old); }
// Locked read-modify-write that stores the value it read.
static void b_lock_add0(volatile uint32_t *p, uint32_t old) {
  (void)old;
  asm volatile("lock addl $0, (%0)" ::"r"(p) : "memory", "cc");
}
// Positive control: an ordinary store of the value the cache already holds.
static void b_store_same(volatile uint32_t *p, uint32_t old) { *p = old; }

// expect: 0 = control, DRAM must still be NEW
//         1 = positive control, DRAM must be OLD again (shows we can see a writeback)
//         2 = test
struct act { const char *name; void (*fn)(volatile uint32_t *, uint32_t); int expect; };
static const struct act acts[] = {
    {"nothing", b_none, 0},
    {"store of the same value", b_store_same, 1},
    {"clwb; sfence", b_clwb, 2},
    {"clflushopt; mfence", b_clflushopt, 2},
    {"clflush; mfence", b_clflush, 2},
    {"prefetchw", b_prefetchw, 2},
    {"cmpxchg that fails", b_cas_fail, 2},
    {"lock cmpxchg that fails", b_lock_cas_fail, 2},
    {"lock add $0", b_lock_add0, 2},
    {"cmpxchg that fails, on another word", b_cas_fail_w8, 2},
    {"lock cmpxchg that fails, on another word", b_lock_cas_fail_w8, 2},
    {"eviction by cache pressure", b_none, 2},  // handled separately
};
#define NACT (int)(sizeof(acts) / sizeof(acts[0]))
#define ACT_EVICT (NACT - 1)

// A buffer much larger than the last-level cache, read to push lines out.
#define THRASH_BYTES (192u << 20)
static char *thrash_buf;
static volatile uint64_t sink;
static void thrash(size_t off, size_t len) {
  uint64_t s = 0;
  for (size_t i = 0; i < len; i += LINE) s += (unsigned char)thrash_buf[(off + i) % THRASH_BYTES];
  sink += s;
}

// ---------------------------------------------------------------------------
// Part C read variants.
// ---------------------------------------------------------------------------
#define RD(name, body)                                                        \
  static uint32_t name(volatile uint32_t *p) {                                \
    uint32_t v;                                                               \
    asm volatile(body : "=&r"(v) : "r"(p) : "memory");                        \
    return v;                                                                 \
  }
RD(c_plain, "mov (%1), %0")
RD(c_pft0, "prefetcht0 (%1)\n\tmov (%1), %0")
RD(c_pft2, "prefetcht2 (%1)\n\tmov (%1), %0")
RD(c_pfnta, "prefetchnta (%1)\n\tmov (%1), %0")
RD(c_pfw, "prefetchw (%1)\n\tmov (%1), %0")
struct rd { const char *name; uint32_t (*fn)(volatile uint32_t *); };
static const struct rd rds[] = {
    {"load", c_plain},
    {"prefetcht0; load", c_pft0},
    {"prefetcht2; load", c_pft2},
    {"prefetchnta; load", c_pfnta},
    {"prefetchw; load", c_pfw},
};
#define NRD (int)(sizeof(rds) / sizeof(rds[0]))
#define PASSES 24

// Like nc_make_stale(), but calls hook(line i) on every line after the CPU has
// cached a clean OLD copy and BEFORE the GPU writes NEW to DRAM.
static void make_stale_hook(struct nc *g, uint32_t oldv, uint32_t newv,
                            void (*hook)(volatile uint32_t *, int)) {
  uint32_t *c = g->b;
  for (int i = 0; i < LINES; i++) {
    uint64_t a = TARGET_VA + (uint64_t)i * LINE;
    *c++ = MI_STORE_DWORD_IMM;
    *c++ = (uint32_t)a;
    *c++ = (uint32_t)(a >> 32);
    *c++ = nc_val(newv, 1, i);
  }
  *c++ = MI_BATCH_BUFFER_END;
  *c++ = MI_NOOP;
  uint32_t blen = (uint32_t)((char *)c - (char *)g->b);
  clflush_range(g->b, blen);
  for (int i = 0; i < LINES; i++) *nc_line(g, i) = nc_val(oldv, 1, i);
  clflush_range(g->t, (size_t)LINES * LINE);
  for (int i = 0; i < LINES; i++) (void)*nc_line(g, i);
  asm volatile("mfence" ::: "memory");
  for (int i = 0; i < LINES; i++) hook(nc_line(g, i), i);
  asm volatile("mfence" ::: "memory");
  nc_exec(g, blen);
  nc_wait(g);
  asm volatile("mfence" ::: "memory");
}
// Hook for Part E: lines 4k+1 get a failing lock cmpxchg, 4k+2 a failing
// cmpxchg, 4k+3 a plain load, 4k nothing.
static void e_hook(volatile uint32_t *p, int i) {
  if ((i & 3) == 1) b_lock_cas_fail(p, 0);
  else if ((i & 3) == 2) b_cas_fail(p, 0);
  else if ((i & 3) == 3) (void)*p;
}

int main(int argc, char **argv) {
  int rounds = argc > 1 ? atoi(argv[1]) : 300;
  int coherent = getenv("COHERENT") && atoi(getenv("COHERENT"));
  int cpu = pin_cpu();
  struct nc g;
  nc_open(&g, coherent ? 3 : 2);

  char *sbuf = aligned_alloc(4096, (size_t)LINES * LINE);
  memset(sbuf, 0, (size_t)LINES * LINE);
  thrash_buf = aligned_alloc(4096, THRASH_BYTES);
  memset(thrash_buf, 1, THRASH_BYTES);
  int ok = 1;

  printf("cxl_test: cpu=%d, %s (GPU PAT %d), %d rounds\n", cpu,
         coherent ? "COHERENT control" : "non-coherent", coherent ? 3 : 2, rounds);

  // ------------------------------------------------------------------ Part A
  long loads[NSEQ] = {0}, stale[NSEQ] = {0}, other = 0, recheck_old = 0;
  for (int r = 0; r < rounds; r++) {
    uint32_t oldv = OLD_TAG | (r << 12), newv = NEW_TAG | (r << 12);
    nc_make_stale(&g, oldv, newv, 1);
    clflush_range(sbuf, (size_t)LINES * LINE);
    for (int i = 0; i < LINES; i++) {
      int s = (i + r) % NSEQ;
      scratch = (volatile uint32_t *)(sbuf + (size_t)i * LINE);
      uint32_t v = seqs[s].fn(nc_line(&g, i));
      asm volatile("mfence" ::: "memory");
      loads[s]++;
      if (v == nc_val(oldv, 1, i)) {
        stale[s]++;
        if (dram(nc_line(&g, i)) != nc_val(newv, 1, i)) recheck_old++;
      } else if (v != nc_val(newv, 1, i)) {
        other++;
      }
    }
    nc_check_landed(&g, newv, 1);
  }
  printf("\nPart A: fence matrix                             loads     stale        %%   model\n");
  for (int s = 0; s < NSEQ; s++) {
    static const char *ex[] = {"0 (control)", "~100% (control)", "stale allowed", "stale FORBIDDEN"};
    printf("  %-44s %8ld %9ld %8.3f%%  %s\n", seqs[s].name, loads[s], stale[s],
           100.0 * stale[s] / loads[s], coherent ? "0 (coherent)" : ex[seqs[s].expect]);
    if (coherent || seqs[s].expect == 0) ok &= stale[s] == 0;
    else if (seqs[s].expect == 1) ok &= stale[s] * 10 >= loads[s] * 9;
  }
  printf("  stale loads whose DRAM re-read was not NEW: %ld (must be 0); other values: %ld (must be 0)\n",
         recheck_old, other);
  ok &= recheck_old == 0 && other == 0;

  // ------------------------------------------------------------------ Part B
  long bl[NACT] = {0}, bold[NACT] = {0}, bother[NACT] = {0}, ev_refetched = 0;
  for (int r = 0; r < rounds; r++) {
    uint32_t oldv = OLD_TAG | ((rounds + r) << 12), newv = NEW_TAG | ((rounds + r) << 12);
    nc_make_stale(&g, oldv, newv, 1);
    for (int i = 0; i < LINES; i++) {
      int a = (i + r) % NACT;
      if (a == ACT_EVICT) continue;
      volatile uint32_t *p = nc_line(&g, i);
      acts[a].fn(p, nc_val(oldv, 1, i));
      asm volatile("mfence" ::: "memory");
      uint32_t v = dram(p);
      bl[a]++;
      if (v == nc_val(oldv, 1, i)) bold[a]++;
      else if (v != nc_val(newv, 1, i)) bother[a]++;
    }
    // Capacity eviction: read far more memory than the caches hold, then look.
    thrash(0, THRASH_BYTES);
    for (int i = 0; i < LINES; i++) {
      if ((i + r) % NACT != ACT_EVICT) continue;
      volatile uint32_t *p = nc_line(&g, i);
      uint32_t v1 = *p;          // NEW here means the line was evicted and re-fetched
      uint32_t v = dram(p);
      bl[ACT_EVICT]++;
      if (v1 == nc_val(newv, 1, i)) ev_refetched++;
      if (v == nc_val(oldv, 1, i)) bold[ACT_EVICT]++;
      else if (v != nc_val(newv, 1, i)) bother[ACT_EVICT]++;
    }
  }
  printf("\nPart B: clean writebacks (action on a CLEAN stale line, then read DRAM)\n");
  printf("  %-44s %8s %9s %9s\n", "action", "lines", "DRAM=OLD", "%");
  for (int a = 0; a < NACT; a++) {
    static const char *ex[] = {"must be 0 (control)", "must be ~100% (positive control)", "test"};
    printf("  %-44s %8ld %9ld %8.3f%%  %s\n", acts[a].name, bl[a], bold[a],
           100.0 * bold[a] / bl[a], coherent ? "(coherent)" : ex[acts[a].expect]);
    if (!coherent && acts[a].expect == 0) ok &= bold[a] == 0;
    if (!coherent && acts[a].expect == 1) ok &= bold[a] * 10 >= bl[a] * 9;
    ok &= bother[a] == 0;
  }
  printf("  eviction rows: %ld of %ld lines had left the cache (plain load returned NEW)\n",
         ev_refetched, bl[ACT_EVICT]);

  // ------------------------------------------------------------------ Part C
  long c_lines[NRD] = {0}, c_new[NRD] = {0}, c_erratic[NRD] = {0}, c_other = 0;
  static unsigned char seen_new[LINES], erratic[LINES];
  for (int r = 0; r < rounds; r++) {
    uint32_t oldv = OLD_TAG | ((2 * rounds + r) << 12), newv = NEW_TAG | ((2 * rounds + r) << 12);
    nc_make_stale(&g, oldv, newv, 1);
    memset(seen_new, 0, sizeof seen_new);
    memset(erratic, 0, sizeof erratic);
    for (int pass = 0; pass < PASSES; pass++) {
      for (int i = 0; i < LINES; i++) {
        uint32_t v = rds[(i + r) % NRD].fn(nc_line(&g, i));
        if (v == nc_val(newv, 1, i)) seen_new[i] = 1;
        else if (v == nc_val(oldv, 1, i)) { if (seen_new[i]) erratic[i] = 1; }
        else c_other++;
      }
      // Growing cache pressure between passes, so lines leave the cache at
      // different times and from different levels.
      thrash((size_t)pass * (8u << 20), (size_t)(pass + 1) * (1u << 20));
    }
    for (int i = 0; i < LINES; i++) {
      int k = (i + r) % NRD;
      c_lines[k]++;
      c_new[k] += seen_new[i];
      c_erratic[k] += erratic[i];
    }
    nc_check_landed(&g, newv, 1);
  }
  printf("\nPart C: erratic reads (%d reads of each line under growing cache pressure)\n", PASSES);
  printf("  %-44s %8s %9s %9s\n", "read sequence", "lines", "saw NEW", "NEW,OLD");
  for (int k = 0; k < NRD; k++)
    printf("  %-44s %8ld %9ld %9ld\n", rds[k].name, c_lines[k], c_new[k], c_erratic[k]);
  printf("  other values: %ld (must be 0)\n", c_other);
  ok &= c_other == 0;

  // ------------------------------------------------------------------ Part D
  long d_n = 0, cas_ok = 0, cas_lost = 0, wr_n = 0, wr_lost = 0;
  for (int r = 0; r < rounds && r < 50; r++) {
    uint32_t oldv = OLD_TAG | ((3 * rounds + r) << 12), newv = NEW_TAG | ((3 * rounds + r) << 12);
    nc_make_stale(&g, oldv, newv, 1);
    for (int i = 0; i < LINES; i++) {
      volatile uint32_t *p = nc_line(&g, i);
      if (i & 1) {
        // CAS expecting the stale value OLD: does it succeed, and is NEW lost?
        uint32_t expect = nc_val(oldv, 1, i), z = 0x0c000000u | (uint32_t)i;
        asm volatile("lock cmpxchg %2, (%1)" : "+a"(expect) : "r"(p), "r"(z) : "memory", "cc");
        d_n++;
        if (expect == nc_val(oldv, 1, i)) cas_ok++;  // eax unchanged: the CAS succeeded
        if (dram(p) == z) cas_lost++;
      } else {
        // Write word 1 of the line; the GPU had written word 0.
        p[1] = 0x12345678u;
        wr_n++;
        if (dram(p) == nc_val(oldv, 1, i)) wr_lost++;
      }
    }
  }
  printf("\nPart D: demonstrations\n");
  printf("  CAS expecting the stale value: %ld of %ld succeeded; %ld overwrote the GPU's value in DRAM\n",
         cas_ok, d_n, cas_lost);
  printf("  write to another word of the line: %ld of %ld lost the GPU's write to word 0\n",
         wr_lost, wr_n);

  // ------------------------------------------------------------------ Part E
  // A compare-and-swap that fails is executed while the copy is clean and
  // CURRENT. Only afterwards does the GPU write NEW. Is the GPU's write lost
  // when the line later leaves the cache (by a flush, or by cache pressure)?
  static const char *en[4] = {"nothing (control)", "lock cmpxchg that fails", "cmpxchg that fails",
                              "load (control)"};
  long e_n[2][4] = {{0}}, e_lost[2][4] = {{0}}, e_gone[4] = {0};
  for (int r = 0; r < rounds && r < 100; r++) {
    for (int mode = 0; mode < 2; mode++) {
      uint32_t oldv = OLD_TAG | ((4 * rounds + 2 * r + mode) << 12);
      uint32_t newv = NEW_TAG | ((4 * rounds + 2 * r + mode) << 12);
      make_stale_hook(&g, oldv, newv, e_hook);
      if (mode == 1) thrash(0, THRASH_BYTES);  // no flush by the program
      for (int i = 0; i < LINES; i++) {
        volatile uint32_t *p = nc_line(&g, i);
        int k = i & 3;
        if (mode == 1 && *p != nc_val(oldv, 1, i)) e_gone[k]++;  // left the cache
        // mode 0: this flush writes a dirty line back. mode 1: it only reads DRAM
        // if the line was already evicted (a still-cached dirty line is written
        // back here too, so mode 1 over-counts for lines that never left).
        uint32_t v = dram(p);
        e_n[mode][k]++;
        if (v == nc_val(oldv, 1, i)) e_lost[mode][k]++;
      }
    }
  }
  printf("\nPart E: failing CAS executed BEFORE the remote write; is the remote write lost?\n");
  printf("  %-28s %10s %10s   %10s %10s %10s\n", "action before remote write", "lines",
         "lost(flush)", "lines", "lost(evict)", "(evicted)");
  for (int k = 0; k < 4; k++) {
    printf("  %-28s %10ld %10ld   %10ld %10ld %10ld\n", en[k], e_n[0][k], e_lost[0][k],
           e_n[1][k], e_lost[1][k], e_gone[k]);
    if (!coherent && (k == 0 || k == 3)) ok &= e_lost[0][k] == 0 && e_lost[1][k] == 0;
  }

  printf("\n%s\n", ok ? "Controls OK." : "SETUP CHECK FAILED: a control did not behave as expected.");
  return ok ? 0 : 2;
}
