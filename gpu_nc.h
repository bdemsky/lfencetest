// gpu_nc.h - Use an Intel integrated GPU as a *non-coherent* writer to memory
// that the CPU has cached, so we can create stale CPU cache lines on demand.
//
// Why: on non-coherent shared memory (e.g. CXL memory shared by several hosts),
// another host's write goes to the memory device without invalidating our CPU
// caches. Our cached copy of the line becomes stale, and software must
// explicitly flush (clflush/clflushopt) before reading to see the new data.
// A normal single-host machine is fully coherent, so there's no other agent
// that can do this... except the iGPU, when it is told not to snoop.
//
// The recipe used by the tests:
//
//   1. CPU writes OLD to a line, flushes it to DRAM, then reads it back.
//      Now DRAM = OLD and the CPU caches hold a *clean* copy of OLD.
//   2. GPU writes NEW to the same line through a non-snooping mapping.
//      The write goes straight to DRAM; the CPU caches are not invalidated.
//      Now DRAM = NEW, CPU cache = OLD (stale).
//   3. CPU runs the code under test (e.g. "clflushopt x; lfence; mov x").
//      If the load returns OLD, it was served from the stale cache line,
//      i.e. it "passed" the flush. If it returns NEW, it went to DRAM.
//
// The CPU copy must be *clean* in step 1: a dirty line would be written back
// by the flush and clobber the GPU's NEW value.
//
// ---------------------------------------------------------------------------
// i915 primitives used (all via ioctl on the DRM render node):
//
//  * /dev/dri/renderD128 - the unprivileged "render node" for the GPU. It lets
//    a process allocate GPU memory and submit work, but not drive displays.
//    Needs membership in group "render" (or an ACL on the device).
//
//  * GEM buffer object (BO) - a chunk of system memory (shmem pages) the
//    driver manages on behalf of the GPU, identified by a u32 handle.
//    DRM_IOCTL_I915_GEM_CREATE_EXT creates one.
//
//  * PAT index (I915_GEM_CREATE_EXT_SET_PAT) - chooses the GPU-side memory
//    attributes (cacheability and coherency) used when the GPU accesses this
//    BO. The indices are platform specific. For Meteor Lake / Arrow Lake the
//    i915 PAT table is:
//        0 = WB, non-coherent      (GPU does not snoop CPU caches)
//        1 = WT, non-coherent
//        2 = UC, non-coherent      <- default for these tests
//        3 = WB, 1-way coherent    (GPU snoops CPU caches) <- control
//        4 = WB, 2-way coherent
//    Measured on Arrow Lake: indices 0 and 2 leave the CPU copy stale
//    (~100%), and index 3 never does, which confirms the table.
//
//  * DRM_IOCTL_I915_GEM_MMAP_OFFSET with I915_MMAP_OFFSET_WB, then mmap() -
//    maps the BO's pages into our address space as normal write-back (WB)
//    cacheable memory. So the CPU accesses the buffer through its caches and
//    the GPU accesses it bypassing them: exactly the non-coherent setup.
//
//  * Batch buffer - a second BO holding GPU commands. We use the command
//    streamer (the GPU's front end) directly, no shaders:
//        MI_STORE_DWORD_IMM  - the command streamer writes a 32-bit immediate
//                              to a GPU virtual address.
//        MI_BATCH_BUFFER_END - end of batch.
//        MI_NOOP             - padding (batch length must be a multiple of 8).
//
//  * GPU virtual addresses (ppGTT) and softpinning - each DRM file has its
//    own GPU page table (per-process GTT). Instead of letting the kernel pick
//    addresses and patch the batch ("relocations", which newer platforms like
//    MTL/ARL no longer support), we pin each BO at a fixed GPU virtual
//    address we choose (EXEC_OBJECT_PINNED) and bake that address into the
//    commands.
//
//  * DRM_IOCTL_I915_GEM_EXECBUFFER2 - submits the batch to an engine (render
//    or copy/"BLT") with the list of BOs it uses. EXEC_OBJECT_WRITE on the
//    target tells the kernel the batch writes it, so that waiting on the
//    target waits for this batch.
//
//  * DRM_IOCTL_I915_GEM_WAIT - blocks until all outstanding GPU work that
//    touches the BO has completed. The kernel ends each request with a GPU
//    cache flush before signalling completion, so when this returns the GPU's
//    writes are in memory. (The tests double check this: after each round,
//    a full flush of the CPU copies must read back NEW.)
//
//  * DRM_IOCTL_I915_GEM_SET_DOMAIN (CPU) - tells the kernel the CPU wrote to
//    the batch through its caches, so the kernel clflushes it before the GPU
//    reads it. We also clflush the batch explicitly after every rebuild, so we
//    don't depend on the kernel's domain tracking.
//
// Kernel cache maintenance can get in the way: i915 may clflush a
// non-coherent BO's pages when it first binds them for the GPU. nc_open()
// therefore submits one empty batch up front so that happens before any
// test, not between our cache warm-up and the measurement. The "load (no
// flush)" control in each test confirms the lines really stay stale.
// ---------------------------------------------------------------------------
#ifndef GPU_NC_H
#define GPU_NC_H

#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm/i915_drm.h>

#define LINE 64      // cache line size
#define LINES 2048   // test lines per round (128 KiB: stays resident in L2)

// Fixed GPU virtual addresses we softpin the two BOs at (arbitrary, below 2^47
// so they need no sign extension to be canonical).
#define TARGET_VA 0x10000000ull
#define BATCH_VA  0x20000000ull

// Command streamer ("MI_*") command encodings. MI commands have bits 31:29 = 0,
// the opcode in bits 28:23, and for multi-dword commands the length in bits
// 7:0, given as (total dwords - 2).
//   MI_STORE_DWORD_IMM: opcode 0x20; bit 22 = 0 selects the ppGTT (not the
//   global GTT); 4 dwords total: header, addr[31:0], addr[63:32], data.
#define MI_NOOP             0x00000000u
#define MI_BATCH_BUFFER_END (0x0Au << 23)
#define MI_STORE_DWORD_IMM  ((0x20u << 23) | (4 - 2))

static inline void die(const char *m) { perror(m); exit(1); }

// Write back and invalidate every line in [p, p+n), then mfence so all the
// flushes have finished before we return.
static inline void clflush_range(const volatile void *p, size_t n) {
  for (size_t i = 0; i < n; i += LINE)
    asm volatile("clflush (%0)" ::"r"((const char *)p + i) : "memory");
  asm volatile("mfence" ::: "memory");
}

// Pin the calling thread to one CPU ($CPU, default 2) so all cache state
// lives in one core's private caches. On a hybrid part, check
// /sys/devices/cpu_core/cpus and /sys/devices/cpu_atom/cpus for P and E cores.
static inline int pin_cpu(void) {
  int cpu = getenv("CPU") ? atoi(getenv("CPU")) : 2;
  cpu_set_t cs;
  CPU_ZERO(&cs);
  CPU_SET(cpu, &cs);
  if (sched_setaffinity(0, sizeof cs, &cs)) die("sched_setaffinity");
  return cpu;
}

struct nc {
  int fd;                  // render node
  uint32_t target, batch;  // BO handles
  volatile uint32_t *t;    // CPU (WB) mapping of the target: LINES lines
  uint32_t *b;             // CPU (WB) mapping of the batch
  uint64_t engine;         // I915_EXEC_RENDER or I915_EXEC_BLT
};

// Create a GEM BO. pat >= 0 selects a GPU PAT index; pat < 0 keeps the
// kernel's default attributes.
static inline uint32_t nc_create_bo(int fd, uint64_t size, int pat) {
  struct drm_i915_gem_create_ext_set_pat ext = {
      .base = {.name = I915_GEM_CREATE_EXT_SET_PAT}, .pat_index = (uint32_t)pat};
  struct drm_i915_gem_create_ext c = {.size = size};
  if (pat >= 0) c.extensions = (uintptr_t)&ext;
  if (ioctl(fd, DRM_IOCTL_I915_GEM_CREATE_EXT, &c)) die("GEM_CREATE_EXT");
  return c.handle;
}

// Map a BO into our address space with ordinary write-back caching.
static inline void *nc_map_wb(int fd, uint32_t h, uint64_t size) {
  struct drm_i915_gem_mmap_offset mo = {.handle = h, .flags = I915_MMAP_OFFSET_WB};
  if (ioctl(fd, DRM_IOCTL_I915_GEM_MMAP_OFFSET, &mo)) die("GEM_MMAP_OFFSET(WB)");
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mo.offset);
  if (p == MAP_FAILED) die("mmap");
  return p;
}

// Submit the first `len` bytes of the batch BO and return right away (the
// GPU runs asynchronously). Both BOs are softpinned at their fixed addresses.
static inline void nc_exec(struct nc *g, uint32_t len) {
  struct drm_i915_gem_exec_object2 obj[2] = {
      {.handle = g->target, .offset = TARGET_VA,
       .flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS | EXEC_OBJECT_WRITE},
      // By default the last object in the list is the batch.
      {.handle = g->batch, .offset = BATCH_VA,
       .flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS}};
  struct drm_i915_gem_execbuffer2 eb = {
      .buffers_ptr = (uintptr_t)obj, .buffer_count = 2, .batch_len = len,
      .flags = g->engine | I915_EXEC_NO_RELOC};
  if (ioctl(g->fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &eb)) die("GEM_EXECBUFFER2");
}

// Block until the GPU has finished all work that writes the target BO.
static inline void nc_wait(struct nc *g) {
  struct drm_i915_gem_wait w = {.bo_handle = g->target, .timeout_ns = -1};
  if (ioctl(g->fd, DRM_IOCTL_I915_GEM_WAIT, &w)) die("GEM_WAIT");
}

// Open the GPU, create the target BO (with GPU PAT index `pat`) and the batch
// BO, map both WB on the CPU, and submit one empty batch so the kernel does
// its first-bind work now.
static inline void nc_open(struct nc *g, int pat, uint64_t engine) {
  g->fd = open("/dev/dri/renderD128", O_RDWR);
  if (g->fd < 0) die("open /dev/dri/renderD128 (need group 'render')");
  g->engine = engine;

  const uint64_t tsize = (uint64_t)LINES * LINE;
  const uint64_t bsize = 4 * 4 * LINES + 4096;  // 4 dwords per store + tail
  g->target = nc_create_bo(g->fd, tsize, pat);
  g->batch = nc_create_bo(g->fd, bsize, -1);
  g->t = nc_map_wb(g->fd, g->target, tsize);
  g->b = nc_map_wb(g->fd, g->batch, bsize);

  struct drm_i915_gem_set_domain sd = {.handle = g->batch,
      .read_domains = I915_GEM_DOMAIN_CPU, .write_domain = I915_GEM_DOMAIN_CPU};
  if (ioctl(g->fd, DRM_IOCTL_I915_GEM_SET_DOMAIN, &sd)) die("GEM_SET_DOMAIN");
  g->b[0] = MI_BATCH_BUFFER_END;
  g->b[1] = MI_NOOP;
  clflush_range(g->b, LINE);
  nc_exec(g, 8);
  nc_wait(g);
}

// Pointer to the first dword of test line i, as the CPU sees it.
static inline volatile uint32_t *nc_line(struct nc *g, int i) {
  return &g->t[(size_t)i * LINE / 4];
}

// Value stored in line i in a round: base, optionally tagged with the line
// index so a misdirected write can be told apart from a real one.
static inline uint32_t nc_val(uint32_t base, int tag, int i) {
  return tag ? base | (uint32_t)i : base;
}

// Steps 1 and 2 of the recipe for one round:
//   put oldv in DRAM, cache a clean copy on the CPU, then have the GPU write
//   newv to DRAM behind the CPU's back and wait for it to land.
// When this returns, every test line is (normally) stale in the CPU cache.
static inline void nc_make_stale(struct nc *g, uint32_t oldv, uint32_t newv, int tag) {
  // Build the batch: one MI_STORE_DWORD_IMM per test line, then the end.
  uint32_t *c = g->b;
  for (int i = 0; i < LINES; i++) {
    uint64_t a = TARGET_VA + (uint64_t)i * LINE;
    *c++ = MI_STORE_DWORD_IMM;
    *c++ = (uint32_t)a;
    *c++ = (uint32_t)(a >> 32);
    *c++ = nc_val(newv, tag, i);
  }
  *c++ = MI_BATCH_BUFFER_END;
  *c++ = MI_NOOP;
  uint32_t blen = (uint32_t)((char *)c - (char *)g->b);
  clflush_range(g->b, blen);  // the GPU reads the batch from DRAM

  // DRAM := OLD. Then read it back so a clean OLD copy sits in the CPU caches.
  for (int i = 0; i < LINES; i++) *nc_line(g, i) = nc_val(oldv, tag, i);
  clflush_range(g->t, (size_t)LINES * LINE);
  for (int i = 0; i < LINES; i++) (void)*nc_line(g, i);
  asm volatile("mfence" ::: "memory");

  // DRAM := NEW via the GPU. The CPU's cached copies are not invalidated.
  nc_exec(g, blen);
  nc_wait(g);
  asm volatile("mfence" ::: "memory");
}

// Sanity check after a round: once every line is properly flushed, the CPU
// must read NEW. If not, the GPU write never reached memory and the round's
// results would be meaningless.
static inline void nc_check_landed(struct nc *g, uint32_t newv, int tag) {
  clflush_range(g->t, (size_t)LINES * LINE);
  for (int i = 0; i < LINES; i++)
    if (*nc_line(g, i) != nc_val(newv, tag, i)) {
      fprintf(stderr, "GPU write to line %d did not reach memory\n", i);
      exit(1);
    }
}

#endif
