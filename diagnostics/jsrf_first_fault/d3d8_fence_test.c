/*
 * Does the guest's GPU fence actually make the guest WAIT?
 *
 * Until 20 Sep 2026 it did not, and nothing in the tree said so. The pump
 * published [dev+0x30] -- the counter of the NEXT fence to be allocated --
 * into the word the title spins on. D3D_SetFence (0x00191390) emits the
 * release packet carrying the PRE-increment value and only then does
 * [dev+0x30] += 2, so the hardware invariant is
 *
 *     *fence <= [dev+0x30] - 2
 *
 * and [dev+0x30] itself is the one value the semaphore can never hold.
 * Writing it made every wait vacuous, because D3D_BlockOnTime's test is
 * UNSIGNED:
 *
 *     edx = cur - *fence ;  ecx = cur - want ;  if (ecx >= edx) return;
 *
 * with *fence == cur, edx is 0 and every possible `want` returns immediately.
 * D3DVertexBuffer_Lock's block, D3D_BlockOnResource, all of it: no-ops. The
 * corrupt text (G2) was the title refilling a vertex buffer at OffsetToLock=0
 * while the draw that reads it was still queued.
 *
 * THE FIRST CASE BELOW IS THE ONE THAT MATTERS. It reimplements the guest's
 * arithmetic from the disassembly and asserts the OLD value does not block
 * and the CORRECT one does. A test that only checked "a word was written"
 * would have passed throughout the entire period the bug existed.
 *
 * WHAT IS NOT TESTED HERE, AND WHERE IT IS INSTEAD. Writing through the
 * pointer at +0x34, and the suppression of the pump's fallback once the GPU
 * drives the fence, both need a mapped guest RAM. xbox_MemoryLayoutInit
 * cannot be called with no XBE -- it faults inside itself, after its banner --
 * and faking an XBE to unit-test two stores would be more machinery than the
 * stores. Those two are covered by counters in the run instead, and they are
 * positive controls rather than statistics:
 *
 *   [D3D8-RING] GPU fence live: first release value=N at 0xADDR
 *        the case was reached and the pointer resolved. Absent => the
 *        executor never saw 0x1D70 and the fallback is still driving.
 *   [D3D8-RING] fence published=.. refused=.. suppressed=.. releases=..
 *        releases>0 with suppressed>0 is the fix working; releases>0 with
 *        suppressed==0 would mean the gate never engaged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/kernel/d3d8_ring.h"

static int failures;

static void check(const char *what, unsigned long long got,
                  unsigned long long want)
{
    if (got == want) { printf("  ok   %-44s %llu\n", what, got); return; }
    printf("  FAIL %-44s got %llu want %llu\n", what, got, want);
    ++failures;
}

/* D3D_BlockOnTime, 0x00191440, transcribed from the disassembly:
 *     0019144C  mov eax,[edi+0x34] / mov ecx,[eax]   ; *fence
 *     00191451  mov eax,[edi+0x30]                   ; cur
 *     00191456  sub edx,ecx                          ; edx = cur - *fence
 *     0019145A  sub ecx,esi                          ; ecx = cur - want
 *     0019145C  cmp ecx,edx / jae <ret>              ; UNSIGNED
 * Returns 1 when the guest sails through without waiting. */
static int block_on_time_returns(uint32_t cur, uint32_t fence, uint32_t want)
{
    uint32_t edx = cur - fence;
    uint32_t ecx = cur - want;
    return ecx >= edx;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "arithmetic";

    if (!strcmp(mode, "arithmetic")) {
        /* The title issued fence 98, then [dev+0x30] became 100. It waits for
         * 98. The GPU has only reached 96. */
        const uint32_t cur = 100, want = 98, gpu_reached = 96;

        check("OLD: publishing [dev+0x30] lets it through",
              block_on_time_returns(cur, /*fence=*/cur, want), 1);
        check("NEW: honest fence BLOCKS while work outstanding",
              block_on_time_returns(cur, gpu_reached, want), 0);
        check("NEW: releases once the GPU reaches the value",
              block_on_time_returns(cur, want, want), 1);
        /* The bound-buffer path waits for [dev+0x30] itself, which is the
         * case G2 actually takes -- 0x001917C9 loads [edx+0x30] as the
         * target. It must also block. */
        check("NEW: the bound path (want == cur) blocks too",
              block_on_time_returns(cur, gpu_reached, cur), 0);
        check("OLD: the bound path let even THAT through",
              block_on_time_returns(cur, cur, cur), 1);

    } else if (!strcmp(mode, "nodevice")) {
        /* Before the title creates its device there is nothing to write. */
        d3d8_ring_set_defaults(0, 0);
        check("release is a no-op with no device",
              d3d8_ring_fence_release(1u), 0);
        check("nothing counted", d3d8_ring_fence_release_count(), 0);

    } else {
        fprintf(stderr, "unknown case '%s'\n", mode);
        return 2;
    }

    printf("%s: %s\n", mode, failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
