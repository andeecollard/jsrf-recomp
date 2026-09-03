# JSRF goals — renderer bring-up, 3 September 2026

Supersedes the goal list in `JSRF_GOALS_2026-09-03.md`, whose loading and
startup goals are met. Evidence for the current state is in
`CLAUDE_HANDOVER_2026-09-03_HEAP.txt` and the two commits after it.

## Where we are

Startup and loading are no longer the blocker. A 60-second fresh-HDD run has
no guest fault, no allocation failure, no stack damage, a stable root pointer
and 290 distinct asset paths, and writes a 117 MB first-run cache. Every
vertex batch the title submits now completes the vertex stage: rejections went
70,228 to zero when `UB_D3D` vertex colours were decoded.

The screen is still black, and the reason is measured rather than assumed:
the executor transforms the title's real geometry with the wrong program.
Batch 20,000 arrives as an ordinary interleaved vertex buffer and comes out as
`oPos=(0.53125 0.53125 0 inf)` for every vertex, because `slots=12 start=0` is
still the twelve-instruction full-screen blit shader from startup, unchanged
across 135,802 batches.

## Closed by measurement, 3 September

G1 and G4 below are answered; their text is kept so the reasoning survives.
The vertex programs arrive on subchannel 0, undropped, and the title selects
one twelve-instruction program — there was no program switch to miss. The
first-boot cache completes: nine `JSRF_CACHE_COMPLETE*.CMP` markers, no fatal
file, a file set stable at 259 files / 117 MB.

### G6 — The fragment stage refuses two draws in three

`913,580` of `1,370,379` draws are rejected with `combiner / texture program`.
`nv2a_texture_copy` implements one measured RGB565 blit configuration and
rejects every other by design. Group the distinct combiner setups the title
actually uses before writing any combiner code, and implement the one that
accounts for most of them first.

**Completion:** the rejection count falls substantially, and the draws that
newly pass write non-zero pixels.

### G7 — Something must write non-zero pixels

The draws that already pass are a copy whose source surface is all zeros, and
the rasterised position range is degenerate (`x -0.5..-0.5` over 6,851,877
indices). A working combiner over an empty source still yields black.

**Completion:** a surface with non-zero content that the title produced.

## Goals, in order

Each goal is finished when its completion test passes on a fresh-HDD run, not
when the code looks right.

### G1 — CLOSED. The title's vertex programs do take effect

`nv2a_pb_exec.c` consumes `SET_TRANSFORM_PROGRAM`, `_PROGRAM_LOAD`,
`_PROGRAM_START` and `_CONSTANT`, yet the decoded program never changes, while
the pusher's own histogram counts `0x0B00`–`0x0B4C` unhandled 1,231,748 times
each. Two layers disagree about who consumes those methods. Establish which
before writing any renderer code.

**Completion:** a late-batch sample reports a `slots` and `start` that change
with the title's uploads, and `oPos` varies between vertices of one batch.

### G2 — Shader inputs with no vertex array

The program at batch 20,000 reads inputs 0,1,3,4,7,8,9,10,11,12
(`reads=0x1F9B`) and only 0,3,4,9 have declared arrays. The rest silently take
current-vertex defaults. Establish whether the title sets them through
`SET_VERTEX_DATA4F`/`2F` and whether those registers are tracked.

**Completion:** every input a running program reads is either backed by an
array or by a value the title actually set; no input is served an unset
default without that being reported.

### G3 — Geometry that lands somewhere real

Only after G1 and G2. 269 distinct methods reach the executor unhandled;
`SET_CLIP_MIN`/`MAX`, the window clip, `SET_TEXTURE_CONTROL0` and the zeta
surface offset are the most frequent. Implement what the measured draws need,
in the order their absence is shown to matter.

**Completion:** a dumped frame that is not byte-identical black, produced by
the title's own geometry rather than a synthetic pattern.

### G4 — CLOSED. The cache completes

Asset opens stop at 290 distinct paths and the run then cycles reads and
allocations. That is equally consistent with cache construction still in
progress and with a loop that never completes. This is a measurement, not a
theory, and it gates everything after loading.

**Completion:** either `JSRF_CACHE_COMPLETE01.CMP` is finished and the file
set stops growing, or the repeating work is identified by call site.

### G5 — Real host input to the guest

Unchanged and still open. The OHCI root hub has ports and completes reset, but
nothing enumerates; that needs descriptors and control transfers through the
HCCA, which is device emulation. Not started.

**Completion:** a host key or pad action changes guest-visible state.

## Non-goals for now

Sound stays bypassed. No Windows/D3D11 build. No full JSRF regeneration. No
upstream merge. Playable gameplay is a later milestone with no date.
