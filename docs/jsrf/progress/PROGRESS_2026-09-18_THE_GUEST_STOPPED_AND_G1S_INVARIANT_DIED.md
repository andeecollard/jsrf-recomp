# The guest stopped allocating, and G1's invariant died — 18 September 2026

One player session (14:59, 388 s, preserved as
`~/Library/Application Support/JSRF/last-run-2026-09-18_1459-fiveinstruments-CRASH.log`).
Four of the five armed instruments read. The fifth was void and said so.

## 1. It did not crash

No `001A2E2E`, no fault, no exception path. The process ran another 28 s after
the screen went black, presenting frames the whole time. What the player saw as
a crash was the guest going black while the engine carried on.

## 2. What actually happened, at t≈360 s

> **CORRECTED THE SAME EVENING, BY THE CONTROL SESSION THIS ONE LACKED.**
> The eleven-ordinal table below is an accurate measurement and a WRONG
> interpretation. A second player session (17:55, 1,523 s) shows **all eleven
> ordinals stopping there too** — across 6.7 million subsequent kernel calls and
> 1,250 seconds — in a run that rendered perfectly throughout and never went
> black. So "every allocator goes to zero" is the title reaching **steady
> state** (loading done, pools warm), not the freeze. It is not diagnostic.
>
> This is the tree's own rule biting the person who wrote it down: *every
> absence-measurement needs a positive control.* I had none for the census, read
> a complete-looking census as a complete-looking answer, and it was neither
> wrong nor useful. What IS specific to 14:59 is the **draw collapse and the
> black screen** — those did not happen at 17:55. See §9.


Eleven ordinals went to **exactly zero** across the 165,000 kernel calls that
followed. This is a complete census, not a sample:

| ord | function | before | after |
|-----|----------|--------|-------|
| 204 | NtProtectVirtualMemory | 11,327 | 0 |
| 149 | KeSetTimer/Ex | 4,655 | 0 |
| 23  | (unknown HAL, stubbed) | 2,833 | 0 |
| 184 | NtAllocateVirtualMemory | 2,023 | 0 |
| 166 | MmAllocateContiguousMemoryEx | 1,978 | 0 |
| 15  | ExAllocatePool(WithTag) | 1,860 | 0 |
| 187 | NtClose | 1,440 | 0 |
| 199 | NtFreeVirtualMemory | 1,341 | 0 |
| 189 | NtCreateEvent | 1,144 | 0 |
| 171 | MmFreeContiguousMemory | 1,057 | 0 |
| 17  | ExFreePool | 974 | 0 |

Every allocator, every free, object create/close, and timer arming. **No new
ordinal appeared**, so the title did not enter an error path. Meanwhile the
per-frame ordinals (161, 129, 277, 294, 119, 175) kept climbing, pad polls held
their exact rate, and the pushbuffer kept flowing — `put`/`get` advanced
*faster* after the freeze (7,900 per 5 s against 6,700) while carrying **11×
fewer methods** (1.5 M against 17.5 M). Empty frames, submitted briskly.
`guest_methods` froze at 86,405, `on=` at 249, `draws` fell 95 %.

### This is not what the reference session did

|  | reference 11:47 | today 14:59 |
|---|---|---|
| APU freeze | t=345 s at 39,529 | t=360 s at 86,405 |
| after it | **kept rendering** — 23,019 draws/5 s, framebuffer full and CHANGED, ran 110 s more | **renderer died with it** — 1,892 draws/5 s, black, 25 s more |

Both froze the APU at ~6 minutes. Only today's took the picture with it. So the
black screen is a **separable symptom**, not G1 recurring. (The goals file's
"two minutes in, every session" is stale: both recent sessions ran 345–360 s
first.)

The ADPCM failure explosion (`fail` 44,478 → 76,417, all `v3 blk 200/201`) is
downstream: a voice stuck at the end of its buffer with nobody updating it.

### What the log cannot say, and why

Which *thread* stopped. There is no per-thread accounting anywhere in the tree.
The only fingerprint is `esp=`, printed once per periodic summary; bucketed by
stack region it does separate three guest threads, and the `0x0050xxxx` one
drops from ~4 samples per 20 s to ~1. But at one sample a second that cannot
separate "stopped" from "slowed fourfold", and it did not — that thread's last
sample lands at t=386, 26 s after the freeze. **Suggestive, and nothing more.**

## 3. G1's central invariant is refuted

The goals file states it as: *"retired voices stay in the list and the guest
never takes them out — eight guest writes to the 3D list head in a whole session
against thousands of raises."*

```
[VOICE-TOP-RING] 87 head writes seen: unlink=81 during-trap=83 on-trapped-voice=83
                 still-active=7 self-linked=0 into-empty=3 emptied-list=7
[APU-IDLE-DELIVERY] found idle=19 distinct, delivered=19 distinct, never told about=0
```

**81 of 87 head writes are textbook unlinks**, and **83 of 87 happened while the
front end was trapped with `cvl` naming exactly the removed voice** — the guest
took out precisely the voice the idle trap had told it about. `self-linked=0`.
The reference session agrees on delivery (15 of 15, 0 lost) and reads 32 head
writes to today's 87. Both halves of "the guest acknowledges a removal request
and does not perform the removal" are measured false.

Caveat: today's config has `CYCLE_BREAK`, `FEDEC_HOLD`, `SELFLINK_END` and
`LIST_MOVE_TO_FRONT` all armed. This is that stack's behaviour, not bare
behaviour.

## 4. RECOMP_IRQ_THREAD is not a paths.conf switch

```
[IRQ-THREAD] no worker stack slice (XBOX_WORKER_STACK_COUNT=0); not delivering
```

`bridge_irq_thread` borrows a guest stack slice and the pool is **0 by
default** — `xbox_memory_layout.h:396` records why: 16 slices is 4 MB of address
space under the arena, and this title already fails few-hundred-byte allocations
with ~2 MB to spare. It needs a build with `-DXBOX_WORKER_STACK_COUNT=1`, which
is a memory-layout change. The previous handover listed it as one line in
`paths.conf`; it cannot work that way. **The absence of a crash in that run
therefore measures nothing about the window.** The instrument failed loudly, as
designed.

## 5. Also read

- **`RECOMP_ICALL_FEEDBACK_PATH`** — `906 resolved, 1 unresolved`, written to the
  support directory. First data since 15 Sep.
- **`RECOMP_FB_WATCH`** — 31,643 comparisons, 17,872 changed (391 small, 17,481
  large), 2 surfaces, 0 evictions, 0 out-of-bounds. A real measurement, not an
  absence. The 391 small changes are the G2 candidates; the BMPs are unspent.

## 6. Built today: `RECOMP_KERNEL_THREADS`

Per-guest-thread kernel-call accounting, bucketed on `g_fs_base` (the guest TIB,
already `RECOMP_TLS`). Reports `tib=… calls=… last_ordinal=… silent_for=… ms`
beside the ordinal histogram. Gate at the call site, so the hot path costs one
cached int and the function stays testable without the environment.

`jsrf_kernel_thread_census`, 17 assertions. **Negative controls verified, not
asserted:** making the slot search ignore `fs_base` fails "two threads:
distinct" (got 1, want 2); dropping a thread with no TIB instead of folding it
fails "no-TIB thread is still a thread" (got 2, want 3). Both were confirmed
with forced rebuilds and distinct binary hashes — the first attempt reused a
stale object from a same-second mtime and reported the wrong failures.

58/58 ctest.

## 7. The Hacked Xefu Pack is closed, and now for a better reason

Previously closed on "JSRF's title ID is absent from its 116 configs". That is
true of `5345000A`, but the disc Microsoft's BC programme supported is the
bundle, and the pack's `Configs/readme.md` names its donor as `4D53003D` — which
is the **launcher** `default.xbe` on that disc, confirmed by parsing it. JSRF's
own executable there carries `5345000A`, the same ID as ours.

Writing an LZX decoder (`~/jsrf/xex_tools/`) got the primary source out of
Microsoft's own `xefutitle*.xex` data DLLs:

```
xefutitle5/6   XDK 3215   80/94 records    9 with payload   JSRF: NO RECORD
xefutitle7/7b/2019/2021  XDK 5426  127-132 records  16 with payload
                                            JSRF: 2 records, payload all 0xFF
```

JSRF enters the table at the 5426 generation and **Microsoft attached no
per-title data to it, in any version**. Every audio-fixing config in the corpus
carries a non-zero word1, `word8=1` and a `word9` address; JSRF's carries none
of it. The pack holds no encoded knowledge about what JSRF does that an emulator
gets wrong, because as far as their emulator was concerned, it didn't.

The decoder works on all six single-block DLLs and **fails on multi-block
streams** (every `xefu*.xex` emulator binary). Parked with its state, ruled-out
hypotheses and resumption point in `~/jsrf/xex_tools/README.md`.

## 8. Standing rule added

Before *asking* for a session, and before believing any counter from one:

```sh
APP=~/jsrf-build/JSRF.app/Contents/MacOS/jsrf-engine
find src diagnostics \( -name '*.c' -o -name '*.h' -o -name '*.m' \) -newer "$APP"
strings -a "$APP" | grep -c '^RECOMP_<switch>$'
```

The first must print nothing; the second needs a known-present positive control
and a nonsense name as negative. A build-tree binary being current is **not** the
bundle being current — `cp` + `install_name_tool` + `codesign` change the copy's
size, so compare `dwarfdump --uuid`, not bytes.

## Next

1. **A session with `RECOMP_KERNEL_THREADS=1`**, armed now. When it goes black,
   the report names the thread that stopped and its last kernel call.
   `RECOMP_IRQ_THREAD` is commented out for this one — it changes timing
   globally and the freeze must read clean.
2. **Then** a `-DXBOX_WORKER_STACK_COUNT=1` build for the IRQ-thread timing
   experiment, on its own session.
3. G1 needs rewriting against §3 rather than extending.

## 9. The 17:55 control session — what survived and what did not

1,523 s, `RECOMP_KERNEL_THREADS=1`, `RECOMP_IRQ_THREAD` commented out.

**Never went black.** Framebuffer full and CHANGED at the end; ~29,000 draws per
window, flat, for the whole run.

**The APU freeze reproduces**: `guest_methods` stopped at 30,026 at **t=270 s**
and the run continued **1,250 s** after it with rendering untouched. So G1's
"music dies" is real and repeatable, and it does not take anything else with it.

**Scene caveat, and it matters.** `on=27` here against `on=249` at 14:59.
CLAUDE.md puts gameplay at 148–453. These are different scenes, so this session
does NOT establish that the black screen is non-deterministic — only that it
does not follow from the APU freeze.

**No thread dies.** The census settles that:

    tib=0x00001000  calls=6,635,713  last_ordinal=145  silent_for=7 ms
    tib=0x00982000  calls=933,017    last_ordinal=161  silent_for=8 ms
    tib=0x009A4000  calls=95,871     last_ordinal=231  silent_for=7 ms
    tib=0x00993000  calls=793,997    last_ordinal=119  silent_for=0 ms
    tib=0x00971000  calls=2          last_ordinal=294  silent_for=1,523,087 ms

Four threads alive and calling 1,250 s after the APU stopped; rates near
constant across t=270. The fifth made two calls at t≈0 and never ran again —
dormant since before the freeze, so not its cause either.

**One real signal:** `tib=0x00982000` drops ~35% (780 → 510 calls/s) exactly at
t=270 and holds there. It does not stop; it does less.

**Why the kernel census cannot go further.** APU submission is not a kernel
call. It is MMIO into the trapped aperture — `[MCPX-TRAP] vp` equals
`guest_methods` exactly — so no ordinal histogram can see it. Per-ordinal rates
across t=270 confirm it: nothing ceases, several paired ordinals drop modestly
(277/294 by 112/s, 161/160 by 68/s) and that is all.

**So the next instrument is per-thread attribution on the MCPX aperture trap,
not the kernel.** The trap handler runs on the faulting thread, so `g_fs_base`
is available there exactly as it was in the bridge.
