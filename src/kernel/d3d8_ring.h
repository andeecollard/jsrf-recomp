/*
 * Where the guest's D3D8 push-buffer ring lives -- LEARNED, not hardcoded.
 *
 * A title statically linked against the Xbox D3D8 SDK keeps one device struct
 * and reaches it through a single global. Everything the runtime needs to pump
 * that ring -- the write cursor, the limit, the ring bounds, the submitted
 * index and the fence the title blocks on -- is at a fixed offset inside that
 * struct, so ONE address makes the whole thing title-independent.
 *
 * That address has a name. `Cxbx-Reloaded/XbSymbolDatabase` signature-scans an
 * XBE and reports `D3D8__D3D_g_pDevice`; for JSRF it is 0x0019DCE0, which is
 * exactly the constant the per-title harness had been carrying by hand. The
 * harness's own comment said the pump could not move into the shared runtime
 * "because the addresses are this title's", and that the general fix was to
 * "learn the notifier address from the channel setup". This is that fix, by a
 * different route: learn it from the symbol scan.
 *
 * Field offsets are the SDK's, not a title's, and each one below was read out
 * of the shipped library code rather than assumed:
 *
 *   +0x00  write cursor    D3DDevice_SetVertexData4f writes at [dev] and
 *   +0x04  limit           compares it against [dev+4] before every vertex
 *   +0x24  ring low        D3D_MakeRequestedSpace_8 wraps between these two
 *   +0x28  ring high       and never reads +0x08/+0x0C, which settles which
 *                          of the two candidate pairs is the live one
 *   +0x2C  submitted PUT   CDevice_KickOff shadows the value it just wrote
 *                          to the channel's DMA_PUT register into this field
 *   +0x30  fence COUNTER   the generation D3D_SetFence releases, += 2 a fence
 *   +0x34  fence POINTER   D3D_BlockOnTime spins on *(*(dev+0x34)), comparing
 *                          it against the counter at +0x30
 *
 * +0x2C AND +0x30 ARE NOT THE SAME KIND OF THING, AND THIS FILE USED TO SAY
 * THEY WERE. Until 21 Sep 2026 the +0x30 row above read "submitted PUT" and
 * the macro was called D3D8_DEV_PUT. It is a fence generation counter, and
 * the submitted PUT is one field lower:
 *
 *   CDevice_KickOff   001912FE  mov [eax+0x40],edx   ; the real DMA_PUT
 *                     00191305  mov [edx+0x2c],ecx   ; shadowed at +0x2C
 *
 * The counter's own arithmetic, and why publishing it into the fence made
 * every wait in the title vacuous, is set out under "THE FENCE, WRITTEN WHERE
 * THE HARDWARE WRITES IT" below -- it is not repeated here. What belongs here
 * is the lesson the name carries: D3D8_DEV_PUT made that bug easy to write
 * and hard to see, and it survived the fix by five hours.
 *
 * The fence is the one that matters. Publishing the producer's own PUT into it
 * tells the title its ring is entirely free, whatever the parser has actually
 * read, and it will then lap the parser and overwrite commands and inline
 * vertex data still waiting to be executed.
 */
#ifndef D3D8_RING_H
#define D3D8_RING_H

#include <stdint.h>

#define D3D8_DEV_WRITE_CURSOR 0x00u
#define D3D8_DEV_LIMIT        0x04u
#define D3D8_DEV_RING_LO      0x24u
#define D3D8_DEV_RING_HI      0x28u
#define D3D8_DEV_SUBMITTED_PUT 0x2Cu
#define D3D8_DEV_FENCE_COUNTER 0x30u
#define D3D8_DEV_FENCE_PTR    0x34u

/* WHERE THE GUEST THINKS IT IS DRAWING.
 *
 * These three are not SDK folklore either: they were derived from this
 * title's own default.xbe and they are rows of
 * diagnostics/jsrf_first_fault/jsrf-d3ddevice-offsets.tsv, with their method
 * and confidence recorded there.
 *
 *   +0x2070  m_RenderTarget   D3DDevice_SetRenderTarget stores the new target
 *                             at 0018D390 mov [esi+0x2070],edi and releases
 *                             the old one read at 0018D363; the 4039 OOVPA
 *                             XREF_ENTRY(0x18) on SetRenderTarget@0018D0F0
 *                             reads the same 0x2070
 *   +0x2074  m_DepthStencil   GetDepthStencilSurface 0018DA75 mov eax,[eax+0x2074]
 *   +0x207C  m_BackBuffer[0]  GetBackBuffer 0018D63D mov eax,[ecx+eax*4+0x207C]
 */
#define D3D8_DEV_RENDER_TARGET 0x2070u
#define D3D8_DEV_DEPTH_STENCIL 0x2074u
#define D3D8_DEV_BACK_BUFFER0  0x207Cu

/* A D3DSurface, and the ONE FIELD THAT SETTLES THE LOAD-SCREEN QUESTION.
 *
 * Derived 21 September 2026 from SetRenderTarget, because nothing in this tree
 * knew the layout and the offsets table stops at the device struct. The
 * function reads the incoming render target's +0x04 and the incoming depth
 * stencil's +0x04 and then emits both, VERBATIM AND UNMASKED, as the payloads
 * of the two surface-offset methods:
 *
 *     0018D3E2  mov ecx,[edi+4]          ; render target ->Data, to [esp+0x30]
 *     0018D3EB  mov edx,[ebp+4]          ; depth stencil ->Data, to [esp+0x20]
 *     ...
 *     0018D449  mov [eax+8],0x40210      ; NV097_SET_SURFACE_COLOR_OFFSET
 *     0018D450  mov [eax+0xc],ebp        ;   payload = [esp+0x30], unchanged
 *     0018D46B  mov [eax+8],0x40214      ; NV097_SET_SURFACE_ZETA_OFFSET
 *     0018D472  mov [eax+0xc],ebp        ;   payload = [esp+0x20], unchanged
 *
 * NO MASK, NO SHIFT, NO ALIAS BIT STRIPPED anywhere between the field and the
 * method. Our own parser latches that payload just as verbatim
 * (`s_gpu.color_offset = param`), so the two numbers are comparable BYTE FOR
 * BYTE and a difference of any kind is a real disagreement rather than a
 * units problem. That is the whole reason this probe can give a verdict.
 *
 * The same function corroborates the rest of the layout: it reads [edx+0x0C]
 * as Format (shr 0x14, and 0xF -- the U/V size nibbles) and [edx+0x10] as
 * Size (and 0xFFF, inc), which is D3DPixelContainer exactly.
 */
#define D3D8_SURFACE_COMMON 0x00u
#define D3D8_SURFACE_DATA   0x04u
#define D3D8_SURFACE_LOCK   0x08u
#define D3D8_SURFACE_FORMAT 0x0Cu
#define D3D8_SURFACE_SIZE   0x10u

/* What one read of the device says about the target. Every field is raw: no
 * masking, because the guest does none (above). */
typedef struct {
    int      trusted;       /* the ring self-check below held */
    uint32_t device;        /* 0 when the global is not populated yet */
    uint32_t put, ring_lo, ring_hi;   /* what the self-check looked at */
    uint32_t rt_surface,    rt_data;
    uint32_t depth_surface, depth_data;
    uint32_t back0_surface, back0_data;
} D3D8RingTarget;

/* Read the guest's idea of its render target.
 *
 * THIS PROBE VALIDATES ITSELF, AND IT HAS TO. It is a chain of guest-VA
 * dereferences through xbox_GetMemoryOffset(), and a device pointer that is
 * null, stale, or reached through an address-space assumption that does not
 * hold would come back as a MISMATCH -- that is, as the answer, and as the
 * wrong one. So the same read also takes the write cursor at +0x00 and the
 * ring bounds at +0x24/+0x28 and checks that lo < hi and lo <= put <= hi.
 * Those three fields are "certain" in the offsets table and the ring is the
 * one structure whose contents cannot be coincidental.
 *
 * `trusted` is that check. When it is 0 the caller must report NO VERDICT,
 * not a divergence.
 *
 * Returns 1 if a device was found at all, 0 if the global is still empty. */
int d3d8_ring_read_target(D3D8RingTarget *out);

/* A title's own answer, used only when no symbol says otherwise. `global` is
 * the VA of D3D_g_pDevice; `device_fallback` is the device struct itself, for
 * the window before the global has been populated. Either may be 0. */
void d3d8_ring_set_defaults(uint32_t global, uint32_t device_fallback);

/* VA of D3D_g_pDevice: the symbol if one was found, else the default. 0 if
 * neither. Resolution happens once, and reports which source won. */
uint32_t d3d8_ring_device_global(void);

/* The device struct's VA: *global if that is populated, else the fallback. */
uint32_t d3d8_ring_device(void);

/* VA of one field, or 0 when the device is not known yet. */
uint32_t d3d8_ring_field_va(unsigned offset);

/* Publish into the fence the title spins on.
 *
 * `fence_counter` is [dev+0x30], written verbatim, and ONLY when
 * `parser_drained` says everything submitted has been consumed.
 *
 * THAT VALUE IS ONE GENERATION HIGH BY CONSTRUCTION -- see the invariant at
 * the top of this file -- so this path tells the title every wait is already
 * satisfied. It is kept because it is the only thing that carries a title
 * whose D3D8 never emits 0x1D70, and it is SUPPRESSED the moment
 * d3d8_ring_fence_release sees a real release packet. Which arm a run took is
 * a counter, not a guess.
 *
 * Publishing with work outstanding would tell the title its ring is free while
 * the parser is still reading it, so the function refuses and counts it: for a correct pump that
 * counter stays at zero, which makes it a positive control rather than a knob.
 *
 * WHY A VERDICT AND NOT A CURSOR. The first version took the parser's ring
 * cursor and carried it into "the guest's address space". The field is not an
 * address: it is a COUNTER, reading 11 while the cursor was 5,695,356, so the
 * title computed PUT - *fence as a huge unsigned value, decided it had no
 * room, and wedged at flips=1 without ever reaching gameplay. Withholding
 * needs no unit conversion; translating invents one.
 *
 * Returns 1 if the word was changed. */
int d3d8_ring_publish_fence(uint32_t fence_word_va, int parser_drained,
                            uint32_t fence_counter);

/* THE FENCE, WRITTEN WHERE THE HARDWARE WRITES IT.
 *
 * D3D_SetFence (0x00191390) emits NV097_BACK_END_WRITE_SEMAPHORE_RELEASE
 * carrying the CURRENT value of [dev+0x30], and only THEN does [dev+0x30] += 2:
 *
 *     001913AD  mov edi, [esi+0x30]        ; the value being issued
 *     001913B8  mov dword [eax],   0x41d70 ; method 0x1D70, count 1
 *     001913BE  mov dword [eax+4], edi     ;   payload = that value
 *     00191420  mov ecx, [esi+0x30] / add ecx,2 / mov [esi+0x30], ecx
 *
 * So the hardware invariant is  *fence <= [dev+0x30] - 2 : the semaphore only
 * ever receives values that have ALREADY been issued, and it can never equal
 * the counter. Publishing [dev+0x30] itself -- which is what this runtime did
 * -- is the one value the field cannot hold, and it makes every wait vacuous:
 *
 *     0019144C  mov eax,[edi+0x34] / mov ecx,[eax]   ; *fence
 *     00191451  mov eax,[edi+0x30]                   ; cur
 *     00191456  sub edx,ecx                          ; edx = cur - *fence
 *     0019145A  sub ecx,esi                          ; ecx = cur - want
 *     0019145C  cmp ecx,edx / jae <return>           ; UNSIGNED
 *
 * With *fence == cur, edx is 0 and `jae` is taken for every possible `want`.
 * D3D_BlockOnTime returns immediately, always, and so does every caller:
 * D3DVertexBuffer_Lock's block via 0x001917B0, and D3D_BlockOnResource.
 *
 * Call this as the parser EXECUTES the release packet, with the payload the
 * packet carries. No unit conversion: the payload is already in [dev+0x30]'s
 * counter space because D3D_SetFence copied it from there. Returns 1 if the
 * word was written. */
int d3d8_ring_fence_release(uint32_t value);

/* How many release packets have been executed. Zero means the case was never
 * reached -- the positive control for the fallback below, not a statistic. */
unsigned long d3d8_ring_fence_release_count(void);

#endif /* D3D8_RING_H */
