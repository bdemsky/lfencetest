// unrolled_test.c - Back-to-back "clflushopt L; FENCE; load L" over many lines.
//
// Unlike gpu_stale.c there is no mfence between trials: each round runs one
// generated, fully unrolled function (see gen_unrolled.py) that does a
// flush/fence/load triple for all LINES lines in a row. That keeps many
// flushes in flight and the pipeline full, which is the stressful case.
//
// Each round: make every line stale with the GPU (gpu_nc.h), run one variant,
// count loads that returned OLD. Rounds cycle through the variants,
// alternating the store version and the register-only version.
//
// Two checks on every stale load seen by a fenced store-version variant:
//  * Immediately re-read that line with "clflushopt; mfence; load". If DRAM
//    still held OLD (the GPU write landing late), this would also return OLD.
//    Returning NEW means the stale load really came from the CPU cache.
//  * Record its position in the sequence (in eighths). Late GPU writes would
//    bunch stale loads at the start; real reordering spreads them out.
// After each round a full flush must read NEW on every line (nc_check_landed).
//
// Usage: ./unrolled_test [rounds=1600]    env CPU=<n> selects the core.
#include "gpu_nc.h"
#include "unrolled.h"

static uint32_t flush_mfence_load(volatile uint32_t *p) {
  uint32_t v;
  asm volatile("clflushopt (%1)\n\tmfence\n\tmov (%1), %0" : "=r"(v) : "r"(p) : "memory");
  return v;
}

#define MAXV 16
int main(int argc, char **argv) {
  int rounds = argc > 1 ? atoi(argv[1]) : 1600;
  int nv = n_uvariants;
  if (nv > MAXV) { fprintf(stderr, "too many variants\n"); return 1; }

  int cpu = pin_cpu();
  struct nc g;
  nc_open(&g, 2, I915_EXEC_RENDER);  // PAT 2: uncached, non-snooping
  static uint32_t out[LINES];

  long st_stale[MAXV] = {0}, st_other[MAXV] = {0}, st_n[MAXV] = {0};
  long rg_stale[MAXV] = {0}, rg_n[MAXV] = {0};
  long pos[MAXV][8] = {{0}}, re_old[MAXV] = {0}, re_new[MAXV] = {0};

  for (int r = 0; r < rounds; r++) {
    int v = (r / 2) % nv, use_reg = r & 1;
    const struct uvariant *u = &uvariants[v];
    // Every line gets the same value (the register version compares against a
    // single newv). OLD < NEW always, as the register version needs.
    uint32_t oldv = 0x0a000000u | (r << 4), newv = 0x0e000000u | (r << 4);

    for (int i = 0; i < LINES; i++) out[i] = 0;  // make out[] cache resident
    nc_make_stale(&g, oldv, newv, 0);

    if (use_reg) {
      rg_stale[v] += u->reg(g.t, newv);
      rg_n[v] += LINES;
    } else {
      u->st(g.t, out);
      for (int i = 0; i < LINES; i++) {
        if (out[i] == oldv) {
          st_stale[v]++;
          if (u->fenced) {
            pos[v][i * 8 / LINES]++;
            if (flush_mfence_load(nc_line(&g, u->order[i])) == oldv) re_old[v]++;
            else re_new[v]++;
          }
        } else if (out[i] != newv) {
          st_other[v]++;
        }
      }
      st_n[v] += LINES;
    }
    nc_check_landed(&g, newv, 0);
  }

  printf("cpu=%d: %d x (clflushopt L; FENCE; load L) back to back, %d rounds\n\n",
         cpu, LINES, rounds);
  printf("%-12s %24s %24s\n", "variant", "store version stale", "register-only stale");
  for (int v = 0; v < nv; v++)
    printf("%-12s %10ld/%-7ld %5.1f%% %10ld/%-7ld %5.1f%%%s\n", uvariants[v].name,
           st_stale[v], st_n[v], st_n[v] ? 100.0 * st_stale[v] / st_n[v] : 0,
           rg_stale[v], rg_n[v], rg_n[v] ? 100.0 * rg_stale[v] / rg_n[v] : 0,
           st_other[v] ? "  (unexpected values seen!)" : "");

  printf("\nStale loads in fenced store-version variants:\n");
  printf("%-12s %-36s %s\n", "variant", "position in sequence (eighths)",
         "re-check with clflushopt;mfence");
  for (int v = 0; v < nv; v++) {
    if (!uvariants[v].fenced || !st_stale[v]) continue;
    char buf[64]; int n = 0;
    for (int k = 0; k < 8; k++) n += snprintf(buf + n, sizeof buf - n, "%ld ", pos[v][k]);
    printf("%-12s %-36s %ld OLD, %ld NEW\n", uvariants[v].name, buf, re_old[v], re_new[v]);
  }
  return 0;
}
