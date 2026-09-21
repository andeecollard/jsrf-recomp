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
 *   +0x30  submitted PUT   D3D_BlockOnTime computes free space as
 *   +0x34  fence POINTER   PUT - *(*(dev+0x34)) and spins on it
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
#define D3D8_DEV_PUT          0x30u
#define D3D8_DEV_FENCE_PTR    0x34u

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
 * `submitted` is written verbatim, and ONLY when `parser_drained` says
 * everything submitted has been consumed. Publishing with work outstanding
 * would tell the title its ring is free while the parser is still reading it,
 * so the function refuses and counts the refusal: for a correct pump that
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
                            uint32_t submitted);

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
