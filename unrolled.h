// unrolled.h - Interface to the generated unrolled sequences (see gen_unrolled.py).
#ifndef UNROLLED_H
#define UNROLLED_H
#include <stdint.h>

struct uvariant {
  const char *name;
  int fenced;                // 1 if a fence sits between flush and load
  const uint16_t *order;     // order[i] = line touched at step i
  // Store version: out[i] = value loaded at step i.
  void (*st)(volatile uint32_t *base, uint32_t *out);
  // Register-only version: returns the number of loads that returned < newv.
  long (*reg)(volatile uint32_t *base, uint32_t newv);
};

extern const struct uvariant uvariants[];
extern const int n_uvariants;
#endif
