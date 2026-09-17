# Upstream status, and what we owe them — 17 September 2026

`origin` is a standalone private repo, not a GitHub fork. `upstream` is
`sp00nznet/xboxrecomp`, a plain remote we fetch and merge. This is a snapshot
of where the two stand and which of our fixes are theirs to have.

## Where we stand

    merge base      051a128, 7 Sep 2026
    upstream ahead  80 commits we do not have
    we are ahead    485 commits they do not have
    upstream HEAD   6f55eaa, 15 Sep 2026 — release v0.10.0 "Negative Control"

Upstream is **active**: v0.10.0 credits 20 merged PRs, and `cd65862` is
"Merge PR #57 from andeecollard: three x86 semantics fixes", so contribution
here is an established, welcomed path rather than a cold approach.

## Merging them into us: real work, do not do it casually

26 files changed on both sides since the merge base. A dry run
(`git merge-tree --write-tree main upstream/main`) reports **14 conflicts**:

    README.md                              src/platform/win32_compat.c
    src/kernel/kernel_bridge.c             src/video/fb_present.c
    src/kernel/kernel_file.c               tools/disasm/functions.py
    src/kernel/nv2a_pb_exec.c              tools/kernel_audit/test_bridge_ordinals.py
    src/kernel/xbox_memory_layout.c        tools/recomp/lifter.py
    tools/recomp/translator.py             tools/recomp/test_fpu_branch.py
    tools/recomp/test_lifter_fpu.py        tools/recomp/test_lifter_sar_width.py (add/add)

**The translator half is the dangerous half.** `lifter.py` and `translator.py`
decide what the generated C says, and CLAUDE.md's standing rule is that
regeneration is not bit-stable across translator changes — so a merge there
invalidates every archived gen and every baseline measured against one. Do it
deliberately, at the top of a session, with a plan for re-verifying a gen, and
never in the middle of an investigation that is quoting numbers.

`nv2a_pb_exec.c` and `kernel_bridge.c` are also live: both were edited on
17 Sep for the sync histogram and the IRQ reading.

**Recommendation: not now.** There is no bug we are chasing that upstream's 80
commits are known to fix, so the merge buys generality rather than progress,
and it costs a re-verified gen.

## What we owe upstream, ranked by how clearly it is a bug on their side

These are all in `src/`, which CLAUDE.md designates as upstream's territory.
Each was found and fixed here, is **not** JSRF-specific, and upstream does not
have it (checked with `git grep` against `upstream/main`).

### 1. The DSP discards thirty of its thirty-two mixbins — **confirmed present upstream**

`upstream/main:src/apu/apu_dsp.c:150` reads, unconditionally:

    float left  = mixbins[0][i];
    float right = mixbins[1][i];

Bins 2–31 are computed and thrown away. The guest routes its **3D voices** —
the sound effects — to bins 6–10, so every effect a title plays is generated
correctly and discarded. Ours is `RECOMP_APU_MIXDOWN_ALL`, default on, and the
player's verdict on it was "sound fx working".

This affects **any** title with 3D audio, is a handful of lines, and is the
single clearest thing we owe them.

### 2. `get_data_ptr`'s bound is dead code — **confirmed present upstream**

`upstream/main:src/apu/apu_vp.c:578` takes `max_sge` and asserts on it; both
call sites at 852 and 876 pass `0xFFFFFFFF`. So a read past the end of the
voice processor's page table silently returns a translation built from
whatever dword sits there. Identical on our side.

We have **not** fixed this (our G7), and the reason transfers: an assert that
starts firing in a player's build is a crash, so it needs counting before
enforcing. Worth reporting as an issue even without a patch — it is a latent
memory-safety bug in their tree and they may know the hardware's MAXSGE
semantics better than we do.

### 3. The decode-pair race, `RECOMP_APU_FEDEC_HOLD` — player-confirmed crash fix

`fe_method` writes FEDECMETH/FEDECPARAM for every method while the guest's ISR
reads them as two separate MMIO loads. A guest method landing between those
loads hands the ISR one method with another's argument; since
`SET_ANTECEDENT_VOICE`'s argument is a voice handle, it passes the ISR's
`h >= 0x100` guard and is dereferenced.

On 17 Sep this crashed the title inside the guest's DirectSound ISR (guest
stack ending `001A25D9`, 62 of 2,555 methods dispatched while trapped). With
the guard on, the next session ran clean with `held=1702`. Generic to any
title using DirectSound, which is all of them.

Caveat for the PR: **one player session**, and the crash is intermittent.

### 4. `RECOMP_VSH_DP_ZERO` — NV2A's multiply-by-zero rule

DP3/DP4 must carry the NV2A's rule that anything times zero is zero, so
`RSQ(0)=+inf` through a dot product stops becoming a NaN texcoord. Ours,
default on, player-confirmed (it fixed the fence rendering). Hardware
semantics, not a JSRF workaround.

### 5. The PCM stereo page straddle

The PCM path translates once per sample then walks channels on the translated
**physical** address, so a stereo sample crossing a 4 KB boundary reads its
second channel from whatever page physically follows. The ADPCM path
re-translates per dword, which is the intended discipline. Latent for us (the
failing voices are mono) and unfixed here — report rather than patch.

## What is NOT a contribution candidate

- **The Metal backend** (`RECOMP_METAL_*`, ~6 switches and a large file).
  macOS-specific where upstream is D3D11-first. That is a conversation about
  a second backend, not a drive-by PR.
- **`recomp_switch_on` and the switch audit.** Good hygiene, but it imposes a
  grammar on 152 switches and upstream will have its own opinion. Offer, do
  not impose.
- **`frame_hist.h`.** Small, generic, tested, and genuinely useful to anyone
  measuring a title — but it is worth little without a caller, and its caller
  is our pushbuffer executor. Bundle it with something else or leave it.

## Suggested order

1. **The mixbin discard.** Small, obviously correct, affects every title, and
   we have a player confirming it by ear.
2. **The `get_data_ptr` bound**, as an issue rather than a PR.
3. **FEDEC_HOLD**, once a second player session backs it.
4. The rest when the merge happens, since they will conflict less afterwards.
