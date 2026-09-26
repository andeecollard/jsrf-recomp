/*
 * RECOMP_FRAME_SPLIT=1 (G76): where a frame goes, thread by thread.
 *
 * The lift took the executor out of the frame and left three things that
 * can set its length: the title's own thread (game logic plus D3D's
 * bookkeeping, which is lifted guest code), the pusher (the push-buffer walk
 * plus the host's draws, built and encoded on that thread), and the GPU.
 * [STAGE] times the pusher's executor stages and leaves the rest in `rest`;
 * nothing timed the title's thread at all except the mirror's hooks. This is
 * the one place those threads' figures meet, printed per frame over the same
 * window as [STAGE] ([FRAME-SPLIT] lines).
 *
 * Read-only and opt-in. Off, every call site costs one predictable branch
 * on a cached int. On, a timed boundary costs two clock reads (~40 ns on
 * Apple silicon); the title's D3D calls are timed at the OUTERMOST call only,
 * ~2,000-5,000 a frame, so the instrument adds ~0.1-0.2 ms to the thread it
 * measures -- say so beside any number taken from it.
 *
 * What each bucket brackets:
 *   title d3d      every game-called D3D entry point, outermost call,
 *                  inclusive (the mirror's hooks and D3D's waits inside it)
 *   title wait     D3D_BlockOnTime (0x191440): D3D spinning on its fence --
 *                  the title waiting for the pusher
 *   title space    D3D_MakeRequestedSpace_8 (0x191530): the ring full; it may
 *                  call BlockOnTime, so it overlaps `wait`
 *   title state    CDevice_SetStateVB/UP (0x196520/0x1966C0): D3D's lazy
 *                  state flush before a draw
 *   title hooks    the mirror's before/after draw hooks
 *   title kernel   kernel calls the title's thread makes outside D3D, and
 *                  (kernel-in-d3d) inside it
 *   other d3d      D3D calls from any other thread (JSRF calls
 *                  BlockUntilVerticalBlank from one), and `other inner` the
 *                  waits and flushes above when another thread ran them --
 *                  kept apart, because they overlap the title's time
 *   pusher check / replace / pre   the host tokens by kind (d3d8_host.c)
 *   pusher swm     the pusher waiting for the title's interrupt handler to
 *                  acknowledge a software method (PB-NOTIFY)
 *   pusher regs / build / fetch / encode   inside `replace`: the FF register
 *                  file, the draw's build (of it, `fetch`: the GPU-unit
 *                  path's vertex fetch and conversion loop), and the Metal
 *                  backend's external draw (the host encode)
 *   gpu            the union of the command buffers' GPUStartTime..GPUEndTime
 *                  intervals that completed in the window -- coverage, not
 *                  occupancy (see mtl_cb_gpu_watch)
 * "Game" is what the title's thread did that is none of the above: the
 * frame's wall time less title d3d and title kernel. The title thread is the
 * one that calls D3DDevice_Swap.
 */
#ifndef RECOMP_FRAME_SPLIT_H
#define RECOMP_FRAME_SPLIT_H

#include <stdint.h>

enum {
    RFS_T_D3D, RFS_T_WAIT, RFS_T_SPACE, RFS_T_STATE, RFS_T_KICK, RFS_T_HOOKS,
    RFS_T_KERNEL, RFS_T_KERNEL_D3D,
    RFS_O_D3D, RFS_O_INNER,
    RFS_P_CHECK, RFS_P_REPLACE, RFS_P_PRE, RFS_P_SWM,
    RFS_P_REGS, RFS_P_BUILD, RFS_P_FETCH, RFS_P_ENCODE,
    RFS_GPU,
    RFS_N
};

#if defined(_WIN32)
static inline int recomp_fs_on(void) { return 0; }
static inline unsigned long long recomp_fs_now(void) { return 0; }
static inline void recomp_fs_add(unsigned b, unsigned long long ns) { (void)b; (void)ns; }
static inline int recomp_fs_is_title(void) { return 0; }
static inline int recomp_fs_in_d3d(void) { return 0; }
static inline void recomp_fs_kernel(unsigned ordinal, unsigned long long ns) { (void)ordinal; (void)ns; }
static inline void recomp_fs_report(unsigned long long frames, unsigned long long frame_us) { (void)frames; (void)frame_us; }
#else
extern int g_recomp_fs;                    /* -1 unread, 0 off, 1 on */
int recomp_fs_read(void);
static inline int recomp_fs_on(void) { return g_recomp_fs < 0 ? recomp_fs_read() : g_recomp_fs; }
unsigned long long recomp_fs_now(void);    /* ns, monotonic */
void recomp_fs_add(unsigned bucket, unsigned long long ns);
/* The same for a bucket of the title's thread: from any other thread it
 * goes to RFS_O_INNER instead (the census stager's internal timers). */
void recomp_fs_add_t(unsigned bucket, unsigned long long ns);
/* The title's thread: the one that calls D3DDevice_Swap. */
int  recomp_fs_is_title(void);
int  recomp_fs_in_d3d(void);
void recomp_fs_kernel(unsigned ordinal, unsigned long long ns);
/* The census wrappers (stage_d3d8_census.py): names once, then each
 * game-called entry point. enter returns the start (0 when nested). */
void recomp_fs_d3d_names(const char *const *names, const uint32_t *addrs, unsigned n, unsigned swap_idx);
unsigned long long recomp_fs_d3d_enter(unsigned idx);
void recomp_fs_d3d_exit(unsigned idx, unsigned long long t0);
/* Printed by the [STAGE] report over its window (frames flips, frame_us of wall time). */
void recomp_fs_report(unsigned long long frames, unsigned long long frame_us);
#endif

#endif
