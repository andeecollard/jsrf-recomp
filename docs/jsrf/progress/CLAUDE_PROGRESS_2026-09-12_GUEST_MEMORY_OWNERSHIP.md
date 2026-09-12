# The ownership seam exists and works; it is not what was blocking resident clears

Date: 2026-09-12 (Europe/London)

Continues `CODEX_PROGRESS_2026-09-12_D3D11_SURFACE_COHERENCE.md`. That document
ended with a specification: translated guest accesses need to observe surfaces
the GPU still owns, because `RECOMP_D3D11_RESIDENT_CLEARS=1` made JSRF read
stale framebuffer RAM and stop after its first batch. The seam is now built and
proved by test. The premise turned out to be wrong.

## Windows presents a real frame

The headline is not the seam. With resident colour clears on and the synthetic
controller detached, the Windows/D3D11 build now presents the "Presented by
SEGA" title card. On the CPU clear path every one of the twenty-four flip
captures at guest clock 1000 is pure black; with resident clears, thirteen to
sixteen of them carry 31,013 non-black pixels and the picture is correct.

That is the same anchor, the same 2,977 native batches, the same 4,956
triangles, and zero software fallbacks in both. The clear was overwriting guest
RAM after the draws had been written back into it.

## What the numbers did

Guest clock 1000, Windows/D3D11, OHCI detached:

```
                        CPU clears      resident clears
surface syncs             6932              3964
colour write-backs        2975              1986
clears left on the GPU       0              2968
native batches            2977              2977
software fallbacks           0                 0
wall                       17s               14s
```

Colour write-backs fall by a third and total syncs by 43%. This is the
collapse the plan asked for, and it did not cost a batch or a triangle.

## The ownership seam

`XBOX_PTR` in `templates/runtime/recomp_types.h` is the single boundary every
translated memory access resolves through: the scalar accessors, the signed and
floating forms, the FS-segmented forms, the XMM lane helpers, the locked
read-modify-writes, and -- the reason it had to be this and not the store
helper -- the `MEM32` lvalues the lifter expands `rep movs` and `rep stos`
into, which never reach `RECOMP_MEM_WRITE`. Hooking it covers block operations
by construction rather than by remembering to.

Under `RECOMP_GPU_OWNERSHIP` it becomes a lookup in a 64 KB table, one byte per
64 KB of guest VA, holding a count of GPU-owned surfaces covering that granule.
Compiled, the disabled path is a shift, an indexed byte load and a not-taken
branch. The map is indexed by guest VA rather than host pointer because Xbox
RAM appears at the low window, twenty-eight mirrors, the tiled aperture and the
physical heap alias, and the guest may read a surface through any of them;
`recomp_mem_watch_ram_aliases()` enumerates them and all are armed.

Because the seam cannot tell a load from a block store, the reconcile is
conservative: it downloads the overlapping surface *and* drops GPU ownership of
it. Sync-only would be right for loads and silently wrong for the `rep movs`
stores, which a later write-back would overwrite. Separating the two directions
is worth doing and needs the block operations routed through a store seam
first -- `recomp_mem_watch_guest_block()` already exists in the runtime and the
lifter has never emitted it.

Changing only the header means an existing gen tree picks this up by copying
`recomp_types.h` over `<gen>/recomp_types.h` and rebuilding. No retranslation.

## It is proved, and it never fires

`guest_ownership_boundary` in `d3d11_copy_test.c` drives the seam the way the
recompiled code drives it -- a guest VA through the map -- against a stand-in
guest window. It asserts that a draw arms the granule, that guest RAM stays
stale, that one translated load returns what the GPU drew rather than the seed,
that the neighbouring surface is neither read back nor disarmed, and that a
translated store lands *after* the reconcile rather than being eaten by it.
That last one is the ordering the seam could plausibly get backwards, and it
would read as memory corruption a long way from the cause.

```
  resident surface coherence   ok
  guest ownership boundary     ok
[GPU-OWN] 2 guest touches, 2 hit a resident surface, 0 declined re-entrant
all cases within tolerance
```

In JSRF, to guest clock 1000:

```
[GPU-OWN] 49137 guest touches, 0 hit a resident surface, 0 declined re-entrant
```

Forty-nine thousand touches, none of which overlapped a surface. All of them
are the one-access-width widening below a granule-aligned surface: the seam
sees only an access's base address, so the granule beneath a framebuffer is
armed too, and accesses there take a slow path that finds nothing and returns.

## The negative control: the seam is not load-bearing here

`RECOMP_GPU_OWN=0` leaves the check compiled in and never arms the map, which
is behaviourally a build without the seam. With resident clears on and OHCI
detached, that control reaches guest clock 1000 with **byte-identical**
counters -- 2977 batches, 3964 syncs, 1986 colour write-backs, 254716211
non-black pixels, 2968 resident clears -- and produces the same frames.

So the stall recorded in the previous progress document was not a guest read of
stale framebuffer RAM. It was the OHCI fault that document isolates two
sections later, and resident clears were never retried with the controller
detached. At this anchor JSRF's guest CPU does not read its own framebuffer
between the clear and the write-back, and the observer that was specified to
make resident clears safe was not what made them work.

The seam is still the right thing to have: it closes the hole in principle, it
is proved to fire correctly when an access really does overlap, and it is what
lets the next residency step -- keeping the flip on the GPU -- be attempted
without guessing. But nothing here has yet needed it, and that should be said
plainly rather than discovered again later.

## What it costs

Three runs each at guest clock 1000, resident clears on, OHCI detached:

```
compiled out   16s  15s  14s
compiled in    18s  14s  14s
```

Both bottom out at 14 s; the first run of each set pays for a cold bottle. At
one-second resolution the seam is free, and 14 s matches the D3D11 figure the
handover records, so resident clears plus the seam are not a regression. On
macOS nothing arms the map at all -- the Metal backend has not been taught to
-- so the check there is pure cost, currently unmeasurable but real.

## Two failures worth not rediscovering

The first device lock was non-recursive and hung at 100% CPU with no output on
the first rejected draw: `reject()` invalidates every surface through the
public entry point, which had just been given a lock the draw already held. The
lock counts re-entry now; the inner/outer split is kept because it stops the
reconciliation running twice, not because correctness depends on it.

The ownership regression first failed on `disarmed=0` with everything else
passing. The surfaces were one granule apart, and arming widens one access
width below a range, so a granule-aligned neighbour shared a map byte. That was
a property of the test's addresses, not of the model.

## Where this leaves the plan

Done: the range API (previous document), the ownership seam, both regressions,
resident colour clears measured safe and beneficial on Windows.

Not done, in the order the evidence now suggests:

1. **Presentation should consume the retained target directly.** The flip still
   downloads through `sync_range`. Thirteen to sixteen of twenty-four flips
   carry content and the black set varies run to run in both configurations,
   which is the surface/flip race, not the clear path. That race is now the
   thing between here and a stable picture.
2. **Render-target-as-texture should bind an SRV** rather than the current
   GPU-to-RAM-to-decoded-texture round trip.
3. **Split reads from writes at the seam**, once `rep movs`/`rep stos` are
   routed through a store helper. Only worth the regeneration if a later scene
   makes the touch counter show real hits.
4. **Depth stays on the CPU.** Unchanged: CrossOver returns 0x34567812 for a
   requested 0x12345678, and the API still refuses.
5. **Teach the Metal backend to arm the map**, or gate the seam off on macOS.
   Right now macOS pays for an observer nothing feeds.

## Pre-existing failures found on the way

Not caused by this work, but they block the suite:

* Five ctest targets do not link on macOS. `xbox_memory_layout.c` references
  `g_apu_trap_host_pc`, which only `src/apu/apu_mmio_hook.c` defines, and
  `xbox_kernel` does not link `xbox_apu`. Any test that links `xbox_kernel`
  alone cannot build: `jsrf_pgraph_notify`, `jsrf_av_encoder_option`,
  `jsrf_heap_alloc`, `jsrf_ohci_register`, `jsrf_counted_file`.
* Six backport tests fail against the gen tree in `build-macos`, looking for
  patterns that are not in it -- `fcmove` in `recomp_0007.c`, `sub_00014885`,
  and four flag-merge cases. Those generated files predate this session and
  were not touched by it.
* `diagnostics/jsrf_first_fault/main 2.c`, `src/kernel/kernel_bridge 2.c` and
  `src/kernel/xbox_memory_layout 2.c` are untracked iCloud conflict copies.
