#ifndef RECOMP_GPU_OWN_H
#define RECOMP_GPU_OWN_H

#include <stddef.h>
#include <stdint.h>

/* Guest-memory ownership boundary for GPU-resident surfaces.
 *
 * A retained render target is newer on the GPU than in guest RAM from the
 * moment it is drawn until something copies it back.  Every consumer inside
 * the runtime -- the presenter, the framebuffer dump, render-target-as-texture
 * -- can be taught to ask for that copy at its own call site.  Generated guest
 * code cannot: a static recompile emits an ordinary load, and there is no
 * point in the translated stream where "this load reads a framebuffer" is
 * known ahead of time.
 *
 * This module is that missing observer.  It is a coarse guest-VA map, one byte
 * per 64 KB, holding a count of how many GPU-owned surfaces cover each granule
 * (its aliases included).  The translated load path tests one byte and falls
 * through; only when a surface is actually resident over that granule does it
 * call into the backend.
 *
 * The map is indexed by guest VA rather than by host address on purpose.  Xbox
 * RAM appears at many guest addresses -- the low window, the RAM mirrors, the
 * tiled aperture, the physical heap alias -- and the guest is free to read a
 * surface through any of them.  Arming every alias of a range is what makes
 * the observer complete; a single host-pointer range check would miss three
 * quarters of the address space.
 */

/* 64 KB granules: the table is 64 KB, a 640x480x2 target covers ten slots, and
 * the hot entries stay in L1 next to the recompiled code's own working set. */
#define RECOMP_GPU_OWN_SHIFT 16
#define RECOMP_GPU_OWN_SLOTS (1u << (32 - RECOMP_GPU_OWN_SHIFT))

/* The widest single access the recompiler emits is a 16-byte XMM move.  The
 * reconcile path has only the base address, so it asks the backend about this
 * much, and arming widens one granule below a range so an access that starts
 * just outside it and reaches in is still seen. */
#define RECOMP_GPU_OWN_MAX_ACCESS 16u

extern unsigned char g_recomp_gpu_own_map[RECOMP_GPU_OWN_SLOTS];

/* Slow path for the translated load/store seam in recomp_types.h.  Returns the
 * host pointer for `guest_va`, having first given guest RAM back whatever the
 * GPU still owns over it. */
uintptr_t recomp_gpu_own_reconcile(uint32_t guest_va);

/* Hand guest RAM back the overlapping surface and drop GPU ownership of it.
 * Returns nonzero when a surface actually overlapped.  Installed by the
 * graphics backend; until it is, the map stays empty and nothing is armed. */
typedef int (*RecompGpuOwnSyncFn)(void *host, size_t bytes);
void recomp_gpu_own_set_sync(RecompGpuOwnSyncFn fn);

/* Expand a RAM offset into every guest VA window that names those bytes.
 * Installed by the memory layout, which owns the mirror/tiled/alias map.
 * Without it only the low RAM window is armed, which is what the standalone
 * renderer regressions want. */
typedef unsigned (*RecompGpuOwnAliasFn)(uint32_t ram_offset, size_t span,
                                        uint32_t *out_va, unsigned max);
void recomp_gpu_own_set_aliases(RecompGpuOwnAliasFn fn);

/* Arm and disarm one host range.  Calls must balance: the backend arms a
 * surface when the GPU's copy becomes the newer one and disarms it when guest
 * RAM has been given the pixels back. */
void recomp_gpu_own_hold(const void *host, size_t bytes);
void recomp_gpu_own_release(const void *host, size_t bytes);

/* Cumulative counters, for answering "did the map cost anything, and did it
 * ever fire" with a measurement rather than an argument. */
void recomp_gpu_own_report(void);
void recomp_gpu_own_counters(uint64_t *touches, uint64_t *hits,
                             uint64_t *armed_slots);

#endif
