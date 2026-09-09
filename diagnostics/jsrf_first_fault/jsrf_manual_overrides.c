/*
 * Hand-written replacements for guest functions this runtime cannot execute.
 *
 * Fed to the recompiler as --exclude-manual, so it does not generate a body for
 * anything defined here and the direct calls in the generated code link to
 * these instead. That is the project's sanctioned way to replace a guest
 * function that is reached by a DIRECT call; recomp_lookup_manual() only ever
 * sees indirect ones.
 *
 * Register conventions: RECOMP_GENERATED_CODE is not defined in this file, so
 * the eax/esp aliases are inactive and the guest register file is addressed as
 * g_eax / g_esp. A generated `ret N` epilogue is `esp += 4 + N; return;` -- the
 * 4 pops the return address the caller pushed.
 */

#include <stdint.h>
#include "recomp_types.h"

/*
 * sub_001A1769 - DSOUND: submit a command block to the MCPX APU and wait.
 *
 *     0x001A18C1  push 3 / pop eax
 *     0x001A18CE  mov  [obj+0x810], eax     ; submit
 *   L:0x001A18D0  cmp  [obj+0x810], 0
 *     0x001A18D3  jne  L                    ; wait for the APU to consume it
 *
 * Nothing in this runtime consumes that ring. src/apu is initialised and its
 * registers are routed, but src/apu/apu_dsp.c is an explicit stub -- the GP
 * processor that would drain the block is ~3000 lines of DSP56300 emulation
 * that has never been written, upstream included. So the guest waits forever;
 * this was the hang in every run.
 *
 * Replaced rather than emulated. The block carries DSP effects work (reverb,
 * EQ) which is bypassed either way, so skipping the submission loses nothing
 * that was going to happen. It is a substitution of the same kind the project
 * already makes for D3D and the kernel: the guest's hardware driver is not the
 * thing we want to run.
 *
 * THIS MAKES AUDIO SILENT AND IS NOT A FIX FOR AUDIO. It exists so that audio
 * stops gating startup and the rest of the title can be reached. The real fix
 * is to override DirectSound at its interface with src/audio/dsound_device.c;
 * see section 1 of CODEX_HANDOVER.txt.
 *
 * __thiscall, one stack argument, returns an HRESULT in eax. The caller at
 * 0x001A1A50 tests it and bails on a negative value, so report success.
 */
void sub_001A1769(void)
{
    g_eax = 0;          /* S_OK */
    g_esp += 8;         /* ret 4 */
}

/*
 * sub_001A308E - DSOUND: wait for an asynchronous voice update to finish.
 *
 * On the Xbox this fifteen-byte routine spins while object+0x12 bit 15 is
 * set.  The sound completion path at 0x001A2FBE clears precisely that bit and
 * does no other object work in this case.  Our recompiled guest cannot make
 * that progress while this call owns the main execution chain: a live sample
 * after selecting New Game found the main thread here on every sample, while
 * the DSOUND worker was blocked behind the guest's critical section.
 *
 * Complete the pending hand-off with the same state transition the hardware
 * completion path would make.  Preserve all other flags; in particular bit 0
 * still records whether this kind of wait applies to the object.
 *
 * __thiscall, no stack arguments, returns void.
 */
void sub_001A308E(void)
{
    if (MEM8(g_ecx + 0x12) & 1)
        MEM16(g_ecx + 0x12) &= 0x7FFFu;
    g_esp += 4;         /* ret */
}
