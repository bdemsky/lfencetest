// gpu_stale.c - Can "FLUSH x; FENCE; load x" return a stale cached value?
//
// Each round uses gpu_nc.h to make all LINES test lines stale (CPU cache =
// OLD, DRAM = NEW), then tests the lines ONE AT A TIME. Each line gets one of
// the sequences below, followed by an mfence so the next trial starts with an
// empty pipeline and store buffer. The sequence for each line rotates from
// round to round, so every sequence sees every line and position.
//
// A load that returns OLD was served from the stale cache line (it got ahead
// of the flush). NEW means it went to memory.
//
// Grouped mode (K=<n>) instead flushes K lines back to back, runs a single
// fence, then loads all K: many flushes in flight behind one fence.
//
// Usage: ./gpu_stale [pat_index=2] [rounds=200] [render|blt]
//   env CPU=<n>  pin to CPU n (default 2)
//   env K=<n>    grouped mode with n lines per group (1..64)
//   env F=<n>    fence for grouped mode: 0 none, 1 lfence (default), 2 mfence
//
// Controls to check before reading the other rows:
//   "load (no flush)" should be ~100% stale (the lines really are stale).
//   With pat_index=3 (coherent), every row should be 0% stale.
#include "gpu_nc.h"

#define OLD_TAG 0x0a000000u
#define NEW_TAG 0x0e000000u

// ---- Sequences under test: each loads line p and returns the value. ----
#define SEQ(name, pre)                                              \
  static uint32_t name(volatile uint32_t *p) {                      \
    uint32_t v;                                                     \
    asm volatile(pre "mov (%1), %0" : "=r"(v) : "r"(p) : "memory"); \
    return v;                                                       \
  }
SEQ(s_plain,            "")
SEQ(s_clflushopt,       "clflushopt (%1)\n\t")
SEQ(s_clflushopt_lf,    "clflushopt (%1)\n\tlfence\n\t")
SEQ(s_clflushopt_sf,    "clflushopt (%1)\n\tsfence\n\t")
SEQ(s_clflushopt_mf,    "clflushopt (%1)\n\tmfence\n\t")
SEQ(s_clflushopt_sf_lf, "clflushopt (%1)\n\tsfence\n\tlfence\n\t")
SEQ(s_clflush,          "clflush (%1)\n\t")
SEQ(s_clflush_lf,       "clflush (%1)\n\tlfence\n\t")
SEQ(s_clwb_lf,          "clwb (%1)\n\tlfence\n\t")
SEQ(s_clwb_mf,          "clwb (%1)\n\tmfence\n\t")

// Same idea, but first a store to a scratch line that is not in the cache.
// That store waits a long time in the store buffer, ahead of the flush, to
// test whether a backed-up store buffer lets the load get ahead of the flush.
static volatile uint32_t *scratch;
#define SSEQ(name, body)                                                  \
  static uint32_t name(volatile uint32_t *p) {                            \
    uint32_t v;                                                           \
    asm volatile(body : "=&r"(v) : "r"(p), "r"(scratch) : "memory");      \
    return v;                                                             \
  }
SSEQ(s_st_lf,    "movl $1, (%2)\n\tclflushopt (%1)\n\tlfence\n\tmov (%1), %0")
SSEQ(s_st_sf_lf, "movl $1, (%2)\n\tclflushopt (%1)\n\tsfence\n\tlfence\n\tmov (%1), %0")
SSEQ(s_st_mf,    "movl $1, (%2)\n\tclflushopt (%1)\n\tmfence\n\tmov (%1), %0")
SSEQ(s_st_cf_lf, "movl $1, (%2)\n\tclflush (%1)\n\tlfence\n\tmov (%1), %0")
SSEQ(s_st_nofl,  "movl $1, (%2)\n\tlfence\n\tmov (%1), %0")

static const struct { const char *name; uint32_t (*fn)(volatile uint32_t *); } seqs[] = {
    {"load (no flush)",                 s_plain},
    {"clflushopt; load",                s_clflushopt},
    {"clflushopt; lfence; load",        s_clflushopt_lf},
    {"clflushopt; sfence; load",        s_clflushopt_sf},
    {"clflushopt; mfence; load",        s_clflushopt_mf},
    {"clflushopt; sfence; lfence; ld",  s_clflushopt_sf_lf},
    {"clflush; load",                   s_clflush},
    {"clflush; lfence; load",           s_clflush_lf},
    {"clwb; lfence; load",              s_clwb_lf},
    {"clwb; mfence; load",              s_clwb_mf},
    {"MISS-st; clflushopt; lfence; ld", s_st_lf},
    {"MISS-st; clflushopt; sf; lf; ld", s_st_sf_lf},
    {"MISS-st; clflushopt; mfence; ld", s_st_mf},
    {"MISS-st; clflush; lfence; ld",    s_st_cf_lf},
    {"MISS-st; lfence; ld (no flush)",  s_st_nofl},
};
#define NSEQ (int)(sizeof(seqs) / sizeof(seqs[0]))

// Grouped mode: clflushopt k lines back to back, one fence, then load all k.
static void grp(struct nc *g, int first, int k, int fence, uint32_t *out) {
  for (int j = 0; j < k; j++)
    asm volatile("clflushopt (%0)" ::"r"(nc_line(g, first + j)) : "memory");
  if (fence == 1) asm volatile("lfence" ::: "memory");
  if (fence == 2) asm volatile("mfence" ::: "memory");
  for (int j = 0; j < k; j++) out[j] = *nc_line(g, first + j);
}

int main(int argc, char **argv) {
  int pat = argc > 1 ? atoi(argv[1]) : 2;
  int rounds = argc > 2 ? atoi(argv[2]) : 200;
  int blt = argc > 3 && !strcmp(argv[3], "blt");
  int K = getenv("K") ? atoi(getenv("K")) : 0;
  int F = getenv("F") ? atoi(getenv("F")) : 1;
  if (K < 0 || K > 64) { fprintf(stderr, "K must be 0..64\n"); return 1; }

  int cpu = pin_cpu();
  struct nc g;
  nc_open(&g, pat, blt ? I915_EXEC_BLT : I915_EXEC_RENDER);

  // Scratch lines for the MISS-st sequences (one per test line).
  char *sbuf = aligned_alloc(4096, (size_t)LINES * LINE);
  memset(sbuf, 0, (size_t)LINES * LINE);

  long stale[NSEQ] = {0}, fresh[NSEQ] = {0}, other[NSEQ] = {0};
  long gstale = 0, gfirst = 0, glast = 0, gtot = 0;

  for (int r = 0; r < rounds; r++) {
    // Round number in the values, so a leftover from an earlier round can't
    // pass for this one. Values are also tagged with the line index.
    uint32_t oldv = OLD_TAG | (r << 12), newv = NEW_TAG | (r << 12);
    nc_make_stale(&g, oldv, newv, 1);

    if (K) {
      uint32_t out[64];
      for (int i = 0; i + K <= LINES; i += K) {
        grp(&g, i, K, F, out);
        asm volatile("mfence" ::: "memory");
        for (int j = 0; j < K; j++) {
          int s = out[j] == nc_val(oldv, 1, i + j);
          gstale += s;
          if (j == 0) gfirst += s;
          if (j == K - 1) glast += s;
        }
        gtot++;
      }
    } else {
      clflush_range(sbuf, (size_t)LINES * LINE);  // every scratch store will miss
      for (int i = 0; i < LINES; i++) {
        int s = (i + r) % NSEQ;
        scratch = (volatile uint32_t *)(sbuf + (size_t)i * LINE);
        uint32_t v = seqs[s].fn(nc_line(&g, i));
        asm volatile("mfence" ::: "memory");  // isolate trials from each other
        if (v == nc_val(oldv, 1, i)) stale[s]++;
        else if (v == nc_val(newv, 1, i)) fresh[s]++;
        else other[s]++;
      }
    }
    nc_check_landed(&g, newv, 1);
  }

  if (K) {
    printf("cpu=%d pat_index=%d K=%d fence=%s groups=%ld stale=%ld/%ld (%.2f%%)  "
           "first-load stale=%ld last-load stale=%ld\n",
           cpu, pat, K, F == 0 ? "none" : F == 1 ? "lfence" : "mfence", gtot, gstale,
           gtot * K, 100.0 * gstale / (gtot * K), gfirst, glast);
    return 0;
  }
  printf("cpu=%d pat_index=%d engine=%s rounds=%d lines/round=%d\n", cpu, pat,
         blt ? "blt" : "render", rounds, LINES);
  printf("%-32s %10s %10s %8s %8s\n", "sequence", "stale", "fresh", "other", "%stale");
  for (int s = 0; s < NSEQ; s++) {
    long n = stale[s] + fresh[s] + other[s];
    printf("%-32s %10ld %10ld %8ld %7.2f%%\n", seqs[s].name, stale[s], fresh[s], other[s],
           n ? 100.0 * stale[s] / n : 0);
  }
  return 0;
}
