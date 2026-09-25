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

/* ── CRI ADX lock/unlock: restore the uniprocessor guarantee ──────────────
 *
 * sub_0013B0A0 and sub_0013B0E0 are CRI's ADX lock and unlock. They protect
 * two GLOBAL guest words with no atomics and no per-thread storage:
 *
 *     0x0025EFA0   the lock refcount
 *     0x0027D0F8   ONE slot holding the saved priority of whoever locked
 *
 * That is safe on an Xbox and only on an Xbox. The lock's first act is to
 * raise the CALLER to base priority 16 (XAPI maps 15 -> 16), and on a single
 * CPU a thread at 16 cannot be preempted by another guest thread at all. The
 * elevation IS the mutual exclusion; the refcount never needs to be atomic
 * because only one thread can ever reach it.
 *
 * This runtime records priority and does not enact it -- a frozen session on
 * 21 Sep 2026 reported `priority applied=0 failed=0` -- and guest threads run
 * concurrently on real cores. Two of them (tid 1006 and 1008) interleave:
 *
 *     A locks    count 0->1, A raised to 16, A's real priority saved
 *     B locks    count 1->2, the `jne` skips, B untouched
 *     A unlocks  count 2->1, the `jne` skips -- A IS STILL AT 16
 *     B unlocks  count 1->0, restores A's saved value onto B
 *     A locks    A is already at 16, so it saves 15. POISONED.
 *
 * From there CRI's own guard on file I/O (sub_001437B0) spins while
 * GetThreadPriority(self) reads 15, calling this unlock every pass. The one
 * pass where the count reaches zero restores 15 -> base 16 -> no change, the
 * count runs negative, and both routines become permanent no-ops because both
 * open with a `jne` on it. The thread can never leave 16. Measured at the
 * freeze: lock_count=-32680066 against queries=32680727 -- the same loop
 * driving both.
 *
 * SERIALISING THE TWO BODIES AGAINST EACH OTHER DOES NOT FIX THIS, and the
 * first version of this override did exactly that. Read the interleave above
 * again: every one of its five steps is a complete call, and they are already
 * in a serial order. A mutex taken and dropped inside each body permits that
 * order unchanged, so the poison still forms. What the elevation prevented was
 * B ENTERING THE REGION WHILE A IS INSIDE IT, which is a statement about the
 * span between A's lock and A's unlock -- not about either body.
 *
 * So the guard is taken by the lock and released by the MATCHING UNLOCK, with
 * the guest's own work in between. Under it B's lock blocks until A's unlock
 * and the interleave cannot be written down. adx_guard.c holds the mechanism
 * and the reasons for its three properties -- recursive, owner-tracked,
 * bounded -- because the unmatched unlocks in the spin at sub_001437B0 and a
 * lock that is never released both need an answer that is not a deadlock.
 *
 * NOT a general fix for priority being unenacted. This restores exclusion for
 * exactly the routine pair that was measured to need it; anything else in the
 * title relying on TIME_CRITICAL for exclusion is still exposed.
 *
 * Both are cdecl with no arguments, so the epilogue is `g_esp += 4`. The
 * bodies below are transcribed instruction-for-instruction from the generated
 * ones; the additions are the guard calls and, since G66, the per-thread
 * ledger's three decisions (adx_guard.h, WHOSE PRIORITY THE ONE SLOT HOLDS):
 *
 *   entry           a thread OWED its own pre-raise priority collects it
 *                   (RECOMP_ADX_PER_THREAD_RESTORE)
 *   unlock at <= 0  not applied; a caller that holds nothing and reads 15 is
 *                   given its own last pre-raise priority back
 *                   (RECOMP_ADX_UNMATCHED_SAFE)
 *   unlock to 0     the slot is restored onto the unlocking thread only if it
 *                   is the thread that raised (RECOMP_ADX_PER_THREAD_RESTORE)
 *
 * The lock's write of 0x0027D0F8 is untouched: the guest reads that slot and
 * must go on finding what it wrote there. RECOMP_ADX_TRACE records each call.
 */
#include "adx_guard.h"

/* The XAPI entry points this pair calls. recomp_funcs.h is generated and is
 * not included here, so name them directly; the generated bodies are what
 * these resolve to at link time. */
void sub_00147CC0(void);     /* XAPILIB::SetThreadPriority */
void sub_00147D12(void);     /* XAPILIB::GetThreadPriority */
void sub_00147DAC(void);     /* XAPILIB::SuspendThread     */
void sub_00147DD2(void);     /* XAPILIB::ResumeThread      */

static int adx_read_count(void) { return (int)(int32_t)MEM32(0x25EFA0); }

/* SetThreadPriority(NtCurrentThread, prio) / GetThreadPriority(NtCurrentThread)
 * through the guest's own XAPI, exactly as the bodies call them. `site` is
 * pushed as the return address: the routine's own entry, so a KeSetBasePriority
 * trace names the call as ours rather than inventing a guest call site. Both
 * are stdcall and pop their arguments; eax/ecx/edx are the cdecl caller's to
 * lose, as they are to the bodies. */
static void adx_set_own_priority(uint32_t site, int prio)
{
    PUSH32(g_esp, (uint32_t)prio);
    PUSH32(g_esp, 0xFFFFFFFEu);
    PUSH32(g_esp, site); RECOMP_ABI_CALL(0x00147CC0u, sub_00147CC0);
}

static int adx_get_own_priority(uint32_t site)
{
    PUSH32(g_esp, 0xFFFFFFFEu);
    PUSH32(g_esp, site); RECOMP_ABI_CALL(0x00147D12u, sub_00147D12);
    return (int)(int32_t)g_eax;
}

/* A raiser whose count reached zero on another thread collects its own
 * priority here, on its own thread, before the body can read or save it. */
static void adx_pay_owed(uint32_t site, uint32_t ra)
{
    int p;
    static int count_source_set;
    if (!count_source_set) {
        adx_trace_set_count_source(adx_read_count);
        count_source_set = 1;
    }
    if (adx_prio_take_owed(&p)) {
        adx_set_own_priority(site, p);
        adx_trace_note(ADX_EV_OWED, ra, adx_read_count(), adx_read_count(),
                       0, p);
    }
}

void sub_0013B0A0(void)
{
    uint32_t ra = MEM32(g_esp);     /* the guest caller, for the trace */
    int before, pre_raise = 0, raised = 0, raise_to = ADX_PRIO_NONE;

    adx_pay_owed(0x0013B0A0u, ra);
    adx_guard_lock_enter();     /* held past this body, to the matching unlock */

    before = adx_read_count();
    g_eax = MEM32(0x25EFA0);
    if (g_eax != 0) goto loc_0013B0D3;      /* jne */

    PUSH32(g_esp, g_esi);
    PUSH32(g_esp, 0xFFFFFFFEu);
    PUSH32(g_esp, 0x0013B0B1u); RECOMP_ABI_CALL(0x00147D12u, sub_00147D12);

    g_esi = g_eax;
    pre_raise = (int)(int32_t)g_eax;
    raised = 1;
    g_eax = MEM32(0x25EF8C);
    raise_to = (int)(int32_t)g_eax;
    PUSH32(g_esp, g_eax);
    PUSH32(g_esp, 0xFFFFFFFEu);
    PUSH32(g_esp, 0x0013B0C0u); RECOMP_ABI_CALL(0x00147CC0u, sub_00147CC0);

    g_ecx = MEM32(0x27D0E0);
    PUSH32(g_esp, g_ecx);
    RECOMP_MEM_WRITE32(0x0013B0C7u, 0x0013B0A0u, 0x27D0F8, g_esi);
    PUSH32(g_esp, 0x0013B0D2u); RECOMP_ABI_CALL(0x00147DD2u, sub_00147DD2);

    POP32(g_esp, g_esi);

loc_0013B0D3:
    RECOMP_MEM_WRITE32(0x0013B0D3u, 0x0013B0A0u, 0x25EFA0, MEM32(0x25EFA0) + 1);
    adx_prio_note_lock(raised, pre_raise);
    adx_trace_note(ADX_EV_LOCK, ra, before, adx_read_count(), 0, raise_to);

    g_esp += 4;         /* ret -- the guard stays held on purpose */
}

void sub_0013B0E0(void)
{
    uint32_t ra = MEM32(g_esp);
    int before, admitted, matched, prio_set = ADX_PRIO_NONE;

    adx_pay_owed(0x0013B0E0u, ra);
    before = adx_read_count();

    /* An unlock from a thread that never locked, arriving while another
     * thread is inside the region, is a spin pass that an elevated holder
     * would have prevented from running at all. Drop it: the body would
     * decrement the refcount and rewrite the one saved-priority slot
     * underneath the holder, which is how 0x0027D0F8 came to hold 15 on
     * 21 Sep 2026. CRI's guard at sub_001437B0 calls this again next pass. */
    admitted = adx_guard_unlock_enter();
    if (admitted < 0) {
        adx_trace_note(ADX_EV_UNLOCK, ra, before, before, admitted,
                       ADX_PRIO_NONE);
        g_esp += 4;     /* ret, with the guest's words untouched */
        return;
    }
    matched = adx_prio_note_unlock();

    /* UNMATCHED-SAFE. At count <= 0 there is nothing to unlock: on hardware
     * no unlock runs here, and the decrement is what drove 0x0025EFA0 to
     * -32,680,066 on 21 Sep and negative again in every poisoned G66 run. The
     * caller is sub_001437B0's `while (GetThreadPriority(self) == 15) unlock`;
     * if it holds nothing and still reads 15, nothing the unlock can do will
     * ever lower it, so hand it back its own pre-raise priority and let the
     * loop exit. */
    if ((int32_t)MEM32(0x25EFA0) <= 0 && adx_unmatched_safe_on()) {
        adx_prio_note_clamp();
        if (!matched && adx_get_own_priority(0x0013B0E0u) == 15) {
            int p;
            adx_prio_rescue(&p);
            adx_set_own_priority(0x0013B0E0u, p);
            prio_set = p;
        }
        adx_trace_note(ADX_EV_UNLOCK, ra, before, before, admitted, prio_set);
        adx_trace_note(ADX_EV_CLAMP, ra, before, before, admitted, prio_set);
        adx_guard_unlock_leave();
        g_esp += 4;
        return;
    }

    RECOMP_MEM_WRITE32(0x0013B0E0u, 0x0013B0E0u, 0x25EFA0, MEM32(0x25EFA0) - 1);
    if (MEM32(0x25EFA0) != 0) goto loc_0013B101;    /* jne */

    g_eax = MEM32(0x27D0E0);
    PUSH32(g_esp, g_eax);
    PUSH32(g_esp, 0x0013B0F3u); RECOMP_ABI_CALL(0x00147DACu, sub_00147DAC);

    /* PER-THREAD RESTORE: the slot belongs to the thread that raised. When
     * that is not this thread, this thread was never raised by this region
     * and keeps its priority; the raiser is owed its own (adx_pay_owed). */
    if (!adx_prio_restore_here()) goto loc_0013B101;

    g_ecx = MEM32(0x27D0F8);
    prio_set = (int)(int32_t)g_ecx;
    PUSH32(g_esp, g_ecx);
    PUSH32(g_esp, 0xFFFFFFFEu);
    PUSH32(g_esp, 0x0013B101u); RECOMP_ABI_CALL(0x00147CC0u, sub_00147CC0);

loc_0013B101:
    adx_trace_note(ADX_EV_UNLOCK, ra, before, adx_read_count(), admitted,
                   prio_set);
    adx_guard_unlock_leave();   /* releases what the matching lock took */
    g_esp += 4;         /* ret */
}
