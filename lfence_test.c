// lfence_test.c - Is LFENCE enough to order a load after CLFLUSHOPT/CLFLUSH?
//
// On non-coherent shared memory (e.g. CXL memory shared by several hosts), a
// reader must evict its stale cached copy before loading data another agent
// wrote. Some code does:
//
//     clflushopt x      (or clflush x)
//     lfence
//     mov x, reg
//
// This program checks whether that load can still be served from the stale
// cache line. It uses the iGPU as the "other host" (see gpu_nc.h): each round,
// every test line holds OLD in the CPU cache and NEW in DRAM. A load that
// returns OLD was served from the cache line the flush should have removed.
//
// Part 1, isolated trials. Each line gets one sequence, followed by an mfence
// so the next trial starts with an empty pipeline and store buffer.
// Sequences rotate over the lines from round to round. The "store-miss"
// variants first store to a line that is not in the cache, so a slow store
// is waiting in the store buffer when the flush executes.
//
// Part 2, back to back. LINES copies of "FLUSH L; FENCE; load L" run in a row
// (unrolled with .rept), with no fences between them.
//
// Controls:
//   - "load (no flush)" must be ~100% stale: the lines really are stale.
//   - Every "mfence" row must be 0 stale: a correct sequence reads NEW.
//   - Every stale load is re-read at once with "clflushopt; mfence; load",
//     which must return NEW. If the GPU write had not reached DRAM yet, the
//     re-read would return OLD too.
//   - After each round, a full flush must read NEW on every line.
//   - COHERENT=1 makes the GPU snoop CPU caches (PAT 3): every row must then
//     be 0 stale, showing the stale reads come from non-coherence.
//
// Usage: ./lfence_test [rounds=300]
//   env CPU=<n>       core to run on (default 2)
//   env COHERENT=1    coherent control run
#include "gpu_nc.h"

#define OLD_TAG 0x0a000000u
#define NEW_TAG 0x0e000000u

// ---------------------------------------------------------------------------
// Part 1: isolated sequences. Each loads line p and returns the value.
// %1 = the test line, %2 = a scratch line that is not in the cache.
// ---------------------------------------------------------------------------
static volatile uint32_t *scratch;
#define SEQ(name, body)                                                  \
  static uint32_t name(volatile uint32_t *p) {                           \
    uint32_t v;                                                          \
    asm volatile(body : "=&r"(v) : "r"(p), "r"(scratch) : "memory");     \
    return v;                                                            \
  }
#define ST "movl $1, (%2)\n\t"  // the cache-missing store
SEQ(s_noflush,     "mov (%1), %0")
SEQ(s_fo_mf,       "clflushopt (%1)\n\tmfence\n\tmov (%1), %0")
SEQ(s_fl_mf,       "clflush (%1)\n\tmfence\n\tmov (%1), %0")
SEQ(s_fo_lf,       "clflushopt (%1)\n\tlfence\n\tmov (%1), %0")
SEQ(s_fl_lf,       "clflush (%1)\n\tlfence\n\tmov (%1), %0")
SEQ(s_st_fo_mf, ST "clflushopt (%1)\n\tmfence\n\tmov (%1), %0")
SEQ(s_st_fl_mf, ST "clflush (%1)\n\tmfence\n\tmov (%1), %0")
SEQ(s_st_fo_lf, ST "clflushopt (%1)\n\tlfence\n\tmov (%1), %0")
SEQ(s_st_fl_lf, ST "clflush (%1)\n\tlfence\n\tmov (%1), %0")

// expect: 0 = control that must be 0% stale, 1 = control that must be ~100%
// stale, 2 = the pattern under test.
struct seq {
  const char *name;
  uint32_t (*fn)(volatile uint32_t *);
  int expect;
};
static const struct seq seqs[] = {
    {"load (no flush)",                         s_noflush,  1},
    {"clflushopt; mfence; load",                s_fo_mf,    0},
    {"clflush; mfence; load",                   s_fl_mf,    0},
    {"clflushopt; lfence; load",                s_fo_lf,    2},
    {"clflush; lfence; load",                   s_fl_lf,    2},
    {"store-miss; clflushopt; mfence; load",    s_st_fo_mf, 0},
    {"store-miss; clflush; mfence; load",       s_st_fl_mf, 0},
    {"store-miss; clflushopt; lfence; load",    s_st_fo_lf, 2},
    {"store-miss; clflush; lfence; load",       s_st_fl_lf, 2},
};
#define NSEQ (int)(sizeof(seqs) / sizeof(seqs[0]))

// ---------------------------------------------------------------------------
// Part 2: back to back. For each of the LINES lines in order:
//     FLUSH off(p); FENCE; mov off(p), eax; mov eax, 4*i(out)
// The offsets are assembler constants (an assembler counter advanced by
// .rept), so there is no address arithmetic between the triples.
// ---------------------------------------------------------------------------
#define STR_(x) #x
#define STR(x) STR_(x)
#define B2B(name, flush, fence)                                          \
  static void name(volatile uint32_t *p, uint32_t *out) {                \
    asm volatile(".set .Lo%=, 0\n\t.set .Lr%=, 0\n\t"                   \
                 ".rept " STR(LINES) "\n\t"                              \
                 flush " .Lo%=(%0)\n\t" fence "\n\t"                     \
                 "mov .Lo%=(%0), %%eax\n\tmov %%eax, .Lr%=(%1)\n\t"       \
                 ".set .Lo%=, .Lo%= + " STR(LINE) "\n\t"                   \
                 ".set .Lr%=, .Lr%= + 4\n\t"                               \
                 ".endr"                                                 \
                 : : "r"(p), "r"(out) : "rax", "memory");                \
  }
B2B(b_fo_mf, "clflushopt", "mfence")
B2B(b_fl_mf, "clflush",    "mfence")
B2B(b_fo_lf, "clflushopt", "lfence")
B2B(b_fl_lf, "clflush",    "lfence")

struct b2b {
  const char *name;
  void (*fn)(volatile uint32_t *, uint32_t *);
  int expect;
};
static const struct b2b b2bs[] = {
    {"clflushopt; mfence; load  x2048", b_fo_mf, 0},
    {"clflush; mfence; load     x2048", b_fl_mf, 0},
    {"clflushopt; lfence; load  x2048", b_fo_lf, 2},
    {"clflush; lfence; load     x2048", b_fl_lf, 2},
};
#define NB2B (int)(sizeof(b2bs) / sizeof(b2bs[0]))

// The re-check applied to every stale load.
static uint32_t flush_mfence_load(volatile uint32_t *p) {
  uint32_t v;
  asm volatile("clflushopt (%1)\n\tmfence\n\tmov (%1), %0" : "=r"(v) : "r"(p) : "memory");
  return v;
}

struct count { long loads, stale, other, recheck_old; };

// Classify one loaded value. A stale value is re-checked immediately.
static void tally(struct count *c, struct nc *g, int line, uint32_t v, uint32_t oldv,
                  uint32_t newv) {
  c->loads++;
  if (v == nc_val(oldv, 1, line)) {
    c->stale++;
    if (flush_mfence_load(nc_line(g, line)) != nc_val(newv, 1, line)) c->recheck_old++;
  } else if (v != nc_val(newv, 1, line)) {
    c->other++;
  }
}

static const char *expect_str(int e) {
  return e == 0 ? "0% (control)" : e == 1 ? "~100% (control)" : "test";
}

static void print_row(const char *name, const struct count *c, int expect, int coherent) {
  printf("  %-38s %9ld %9ld %8.3f%%   %s\n", name, c->loads, c->stale,
         100.0 * c->stale / c->loads, coherent ? "0% (coherent)" : expect_str(expect));
}

int main(int argc, char **argv) {
  int rounds = argc > 1 ? atoi(argv[1]) : 300;
  int coherent = getenv("COHERENT") && atoi(getenv("COHERENT"));
  int cpu = pin_cpu();

  struct nc g;
  nc_open(&g, coherent ? 3 : 2);
  char *sbuf = aligned_alloc(4096, (size_t)LINES * LINE);  // scratch lines
  memset(sbuf, 0, (size_t)LINES * LINE);
  static uint32_t out[LINES];

  struct count c1[NSEQ] = {{0}}, c2[NB2B] = {{0}};

  // Part 1: isolated trials.
  for (int r = 0; r < rounds; r++) {
    // The round number is part of every value, so a leftover from an
    // earlier round can't pass for this one.
    uint32_t oldv = OLD_TAG | (r << 12), newv = NEW_TAG | (r << 12);
    nc_make_stale(&g, oldv, newv, 1);
    clflush_range(sbuf, (size_t)LINES * LINE);  // every scratch store misses
    for (int i = 0; i < LINES; i++) {
      int s = (i + r) % NSEQ;
      scratch = (volatile uint32_t *)(sbuf + (size_t)i * LINE);
      uint32_t v = seqs[s].fn(nc_line(&g, i));
      asm volatile("mfence" ::: "memory");  // isolate trials from each other
      tally(&c1[s], &g, i, v, oldv, newv);
    }
    nc_check_landed(&g, newv, 1);
  }

  // Part 2: back to back, one variant per round, cycling.
  for (int r = 0; r < rounds; r++) {
    int b = r % NB2B;
    uint32_t oldv = OLD_TAG | ((rounds + r) << 12), newv = NEW_TAG | ((rounds + r) << 12);
    for (int i = 0; i < LINES; i++) out[i] = 0;  // make out[] cache resident
    nc_make_stale(&g, oldv, newv, 1);
    b2bs[b].fn(g.t, out);
    asm volatile("mfence" ::: "memory");
    for (int i = 0; i < LINES; i++) tally(&c2[b], &g, i, out[i], oldv, newv);
    nc_check_landed(&g, newv, 1);
  }

  // ---- Report ----
  printf("lfence_test: cpu=%d, %s (GPU PAT %d), %d rounds\n\n", cpu,
         coherent ? "COHERENT control" : "non-coherent", coherent ? 3 : 2, rounds);
  printf("  %-38s %9s %9s %9s   %s\n", "Part 1: isolated trials", "loads", "stale",
         "stale", "expected");
  for (int s = 0; s < NSEQ; s++) print_row(seqs[s].name, &c1[s], seqs[s].expect, coherent);
  printf("\n  %-38s\n", "Part 2: back to back");
  for (int b = 0; b < NB2B; b++) print_row(b2bs[b].name, &c2[b], b2bs[b].expect, coherent);

  // Check the controls, then summarise the patterns under test.
  int ok = 1;
  long recheck_old = 0, other = 0, test_stale = 0, test_loads = 0;
  for (int s = 0; s < NSEQ; s++) {
    const struct count *c = &c1[s];
    recheck_old += c->recheck_old;
    other += c->other;
    if (coherent) ok &= c->stale == 0;
    else if (seqs[s].expect == 0) ok &= c->stale == 0;
    else if (seqs[s].expect == 1) ok &= c->stale * 10 >= c->loads * 9;
    else { test_stale += c->stale; test_loads += c->loads; }
  }
  for (int b = 0; b < NB2B; b++) {
    const struct count *c = &c2[b];
    recheck_old += c->recheck_old;
    other += c->other;
    if (coherent || b2bs[b].expect == 0) ok &= c->stale == 0;
    else { test_stale += c->stale; test_loads += c->loads; }
  }
  ok &= recheck_old == 0 && other == 0;

  printf("\n  stale loads that re-read OLD after clflushopt;mfence: %ld (must be 0)\n",
         recheck_old);
  printf("  values that were neither OLD nor NEW:                 %ld (must be 0)\n\n", other);
  if (!ok) {
    printf("SETUP CHECK FAILED: a control did not behave as expected, so the\n"
           "results above can't be trusted on this machine (see README).\n");
    return 2;
  }
  if (coherent) {
    printf("Coherent control OK: no stale loads when the GPU snoops the CPU caches.\n");
    return 0;
  }
  printf("Controls OK.\n");
  if (test_stale)
    printf("RESULT: %ld of %ld loads after \"flush; lfence\" returned stale data.\n"
           "        LFENCE does not order a later load after CLFLUSHOPT/CLFLUSH\n"
           "        on this CPU. MFENCE does.\n", test_stale, test_loads);
  else
    printf("RESULT: no stale loads after \"flush; lfence\" in this run. Try more\n"
           "        rounds or another core (see README).\n");
  return 0;
}
