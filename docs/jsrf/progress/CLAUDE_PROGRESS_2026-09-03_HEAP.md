# The heap was full of padding, and then it was full of textures

3 September 2026. Continues `CLAUDE_PROGRESS_2026-09-03_FCMOV.md`, which closed
G8 and left the title raising its own disc-error dialog after
`xbox_HeapAlloc: out of memory`.

**G9 is not closed.** The title still exhausts the arena and still writes
`JSRF_FATAL.ERR`. It now does so later, after a fourth genuine startup screen,
and the cause is measured rather than suspected.

## What the arena was actually holding

The allocator prints an owner breakdown when a request fails. Adding the slack
each owner carries beyond what it asked for turned that into a diagnosis:

```
live 1514 blocks / 49301920 bytes; free 1 blocks / 270336; retained 4581336
  ordinal 166 ra=0x0018E6E9 : 225 blocks, 19226360 bytes ( 288168 slack)
  ordinal 184 ra=0x00145A48 : 281 blocks,  5953592 bytes (1026184 slack)
  ordinal  15 ra=0x001A0DF2 : 706 blocks,  3126516 bytes (2725856 slack)
```

Ordinal 15 is `ExAllocatePoolWithTag`. It held 3,126,516 bytes to satisfy
400,660 bytes of requests — **87% padding**. The reuse path rounded every split
to a 4 KB page regardless of the caller's alignment, and the pool bridges ask
for 16. The page rounding was there for a good reason: it kept remainders on a
boundary a page-aligned caller could use, because a misaligned free block was
skipped outright.

## Three changes, two of which are keepers

**1. Allocation granularity is the caller's alignment, not a page.** The reuse
path now reaches an aligned start *inside* a free block and carves the leading
fragment off as its own free block. Once a misaligned block is usable, the
trailing boundary only has to respect this caller's alignment. Pool blocks cost
16 bytes again; page-granular callers still round to a page, which is what
`NtAllocateVirtualMemory` and `MmAllocateContiguousMemoryEx` do on hardware.

Measured: ordinal 15 went from 706 blocks / 3,126,516 bytes (2,725,856 slack)
to 844 blocks / 435,808 bytes (15,940 slack). Total slack 4,581,336 → 1,936,552.

**2. The free list can now combine with the untouched arena.** The bump frontier
and the free list described the same arena but were only ever consulted
separately, so a free block immediately below the frontier could not combine
with the space above it. `heap_absorb_frontier` folds them. `unreached` at
failure went from 905,028 to 0.

**3. Reclaiming the bump path's alignment padding — tried, reverted.** It looked
like the same class of waste: 354,840 bytes skipped to reach alignment
boundaries, never recorded, so neither reusable nor able to take part in
coalescing. Handing it back stopped the title dead at `IoCreateDevice` with no
draws and no ADX tick.

That padding is not free. `NtAllocateVirtualMemory` and
`MmAllocateContiguousMemoryEx` are page-granular on hardware, so a title that
asks for 5,000 bytes owns the whole 8,192-byte span and writes into it.
Reclaiming the tail put a 16-byte pool block inside a page the title was still
using. Bisected with one build and two env-gated runs
(`bisect-no-absorb`, `bisect-no-pad-reclaim`); the gates are gone and the
comment in `xbox_HeapAlloc` now says why the padding stays.

## The stack reservation was never measured

`XBOX_HEAP_BASE` is literally `XBOX_STACK_BASE + XBOX_STACK_SIZE`, so every byte
reserved for the stack is a byte the title cannot allocate. `XBOX_STACK_SIZE`
was 8 MB.

The mapping is zero-filled at init and the main stack grows down from the top,
so the lowest non-zero word in the region is the high-water mark. Across a full
70-second JSRF startup it is **3 KB of 8192 KB**, and the worker slices in the
bottom 4 MB are untouched — recompiled code keeps its working set in host
locals, so the guest stack stays shallow.

Reduced to 6 MB: the worker slices keep their 4 MB for the tick-driven model,
a main-loop title gets 2 MB against a measured 3 KB, and the arena gains 2 MB.
`RECOMP_STACK_REPORT=1` prints the measurement, and follows the macros so it
stays honest if the reservation is resized again.

## What that bought, and where it stops

The title now allocates past the player-asset request that used to fail and
reaches a fourth genuine startup screen. `claude-heap-28`, fresh HDD,
`RECOMP_PB_EXEC=1`, `RECOMP_REPORT_MS=1500`, 55 dumps:

| frame | md5 | what |
|---|---|---|
| `frame001` | `20b2450ed90b` | Presented by SEGA — still byte-identical to `codex-graphics-09/frame003` |
| `frame002` | `a2a2e38683a2` | **Created by Smilebit** — new |
| `frame004` | `7202ddaef470` | a second Smilebit frame |
| `frame005`+ | `8ac3f44fdc3f` | anti-graffiti notice |

Smilebit renders over a cyan field rather than black because the fade overlay is
an untextured draw and the renderer rejects those, so the raw clear colour shows
through. That is the G8 limitation, unchanged and still reported:
`[TEXTURE] prepared=15705 rejected=126737`.

Then it runs out again, and still writes `JSRF_FATAL.ERR`.

## The measured cause of what is left

The allocator now tallies allocations and frees per kernel export, because "the
heap is full" cannot distinguish a large working set from a missing release
path. `claude-heap-27`:

```
export 15   ExAllocatePoolWithTag      922 allocs /     430,384 bytes
export 17   ExFreePool                   1 free   /         180 bytes
export 166  MmAllocateContiguousMemoryEx 673 allocs /  31,409,696 bytes
export 171  MmFreeContiguousMemory      183 frees  /   1,765,376 bytes
export 184  NtAllocateVirtualMemory     578 allocs / 146,271,184 bytes
export 199  NtFreeVirtualMemory         258 frees  / 131,260,464 bytes
```

`NtAllocateVirtualMemory` is healthy — it churns 146 MB and returns 131 MB.
**Contiguous memory is not: 31.4 MB allocated, 1.77 MB freed.** Roughly 29.6 MB
is taken and never given back, and one call site,
`ordinal 166 ra=0x0018E6E9`, holds 20.3 MB of it across 231 live blocks.

`MmFreeContiguousMemory` is bridged and works — it registers 183 real frees —
so this is not a missing bridge. No memory-related export goes unbridged in the
run (`no bridge for ordinal` fires only for 1, 46, 144, 153 and 204, none of
which are allocators). The title simply is not calling it for most blocks.

The next question is why, and the generated code answers most of it.

`0x0018E6E9` is the return address of an indirect call through `MEM32(0x1C40F8)`
inside `sub_0018E670`, which builds an Xbox D3D resource header
(`MEM32(esi) = 0x1040001`, size masked into `+4`) and returns `0x8007000E`,
`E_OUTOFMEMORY`, when the allocation fails. So `0x1C40F8` is the allocate slot
of the title's own D3D8 allocator vector. Every `ordinal 166` return address in
the owner breakdown -- `0x0018E6E9`, `0x0019103D`, `0x00191DC5`, `0x00191EBC`,
`0x00191F85` -- is a call site of that one vector.

The sibling slot `0x1C40FC` has **exactly one call site, `0x00191A8B`**. That is
the release path, and 183 frees against 673 allocations says it is reached about
a quarter as often as it should be.

So the work is to instrument `0x00191A8B` and the refcount test above it the
same way `instrument_startup.py` brackets `sub_00024700`, and find out whether
the title decides not to release or whether the branch that decides is
mistranslated. The FCMOVcc bug in `CLAUDE_PROGRESS_2026-09-03_FCMOV.md` was
exactly that shape -- a comparison on a resource-management path that silently
went one way -- so a second one is worth ruling out before assuming the title
is at fault.

(An earlier draft of this note guessed the harness's `[D3D8-HLE]` layer was
swallowing the releases. It is not: `main.c` initialises that layer as a probe
and says so in its own comment -- nothing in JSRF routes through it, the title
runs its own statically-linked D3D8. Recorded because the wrong lead is cheaper
to discard once than to re-derive.)

Two smaller leads, both real but both minor next to 29.6 MB: pool memory is also
never freed (922 allocations, one `ExFreePool`), and two pure `MEM_RESERVE`
calls of 1 MB each are charged real RAM, which `bridge_NtAllocateVirtualMemory`
already documents as a known gap. Everything else JSRF asks for is
`RESERVE|COMMIT`, so separating reserve from commit is worth about 2 MB here,
not 30.

## The low block was placed to clear any XBE, not this one

`XBOX_HEAP_BASE` is `XBOX_STACK_BASE + XBOX_STACK_SIZE`, and `XBOX_STACK_BASE`
sat at a fixed `0x00780000` because the block below it — primary TLS, kernel
data exports, the TLS and PRCB stand-ins — was pinned at `0x00700000` to clear
any title's image. JSRF's is 11 sections ending at `0x00288620`: 2.6 MB. The
4.5 MB between them was dead address space charged to the arena.

`g_xbox_low_base` is now derived from the actual section extents during
`xbox_MemoryLayoutInit`, rounded up to 64 KB, with the old value as the default
until then and a larger image free to push it up. For JSRF:

```
Low block: 0x00290000 (image ends 0x00288620); 4544 KB returned to the heap
Stack: 6144 KB at Xbox VA 0x00310000 (ESP = 0x0090FFF0)
Heap: 54 MB at Xbox VA 0x00910000-0x04000000
```

`XBOX_KERNEL_DATA_BASE` and friends became expressions over that base rather
than literals. Their 58 call sites are all runtime expressions — the `case`
labels that looked like a problem have the macro on the `return` side — so this
is mechanical, and `heap_alloc_test` (whose stub XBE has no sections, so it
keeps the default) still passes unchanged.

## Enlarging the arena does not fix this

That is the finding, and it is worth more than the 6.4 MB the three corrections
recovered. Four arenas, four failures:

| arena | live at failure | share consumed | screens reached |
|---|---|---|---|
| 50,855,936 | 49,301,920 | 97.0% | 3 |
| 50,855,936 (granularity + frontier) | 49,407,608 | 97.2% | 3 |
| 52,953,088 (stack 6 MB) | 52,188,296 | 98.6% | 4 |
| 57,606,144 (derived low block) | 55,424,136 | 96.2% | 4 |

Every increment is consumed and the wall arrives in the same place. The fourth
run reached no screen the third had not, so the extra 4.5 MB bought allocation,
not progress. What did change is the shape of the failure: the title stopped
spinning on a retry — 207 failed requests instead of more than forty thousand —
and the requests it fails on are now 128 to 2,880 bytes rather than megabytes.

A working set larger than the arena settles somewhere and stops. This does not.
Together with 673 contiguous allocations against 183 frees, through a free path
that demonstrably works, the remaining cause is a leak and not a shortfall.
Nothing further should be spent on making the arena bigger.

(The 128 MB devkit map would have shown this in one run and cannot be used:
`xbox_EnablePhysicalHeapAlias` requires a <= 64 MB layout, and JSRF's renderer
depends on that alias. `RECOMP_TOTAL_RAM_MB` is wired up in `main.c` for titles
where it does work, and is labelled as diagnostic-only in its own log line.)

## It is not a leak, and the arena is actively counterproductive

Codex's resource probes settle it. `claude-res-33`, then `claude-worker-34`:

```
alloc=235  main=235/20577600_bytes  main_released=2  main_destroyed=2  main_freed=2
release=126000  last=454  decrement=125546  bind_release=313855  bind_decrement=313855
```

All 235 resource allocations **succeed** — the 20.6 MB texture set loads
completely. Release is called 126,000 times and works: it decrements. Only 2 of
the 235 are ever released, because the title still holds the other 233.

The texture-cache probe says who holds them, and that they are not duplicates:

```
stores=235  distinct indices=233  distinct resources=235  replacements=0
```

235 stores of 235 *distinct* resources into 233 *distinct* slots, nothing
overwritten and nothing evicted. The two repeated indices, 8 and 9, are exactly
the two resources that were released — slot freed, slot refilled. The title is
not reloading anything. **This is its real startup working set, not a leak.**

That corrects the conclusion in the section above. The four-arena table was
right that the title consumes whatever it is given; the inference that this
meant unbounded demand was wrong. The mechanism is `ordinal 184 ra=0x0014903F`,
which is a doubling allocator that retains every previous segment:

```
arena 52,953,088   3 blocks:            1 + 2 + 4 MB  =  7,340,032
arena 61,800,448   4 blocks:        1 + 2 + 4 + 8 MB  = 15,728,640
```

Give it 4 MB more and it doubles once more, taking 8 MB more and holding 15 MB
to provide 8. **Enlarging the arena does not merely fail to help — past a
threshold it costs more than it gives.** That is the sharpest form of the
earlier finding, and it is the reason to stop.

Where that leaves the arithmetic at failure, with a 58 MB arena:

```
ordinal 166 ra=0x0018E6E9  233 blocks  20,660,224   the texture set
ordinal 184 ra=0x0014903F    4 blocks  15,728,640   the doubling heap
ordinal 184 ra=0x00145A48  319 blocks  10,046,352
ordinal 166 ra=0x00199789  339 blocks   6,465,168
ordinal 255 ra=0x00147F92   16 blocks   2,097,600   thread stacks
```

The open question is no longer "what leaks" but "why does this title's own heap
want 15 MB of segments here when the same code fits on a 64 MB console". The
doubling site 0x0014903F is where to look, and whether our
`NtAllocateVirtualMemory` is making its growth decision take a different branch
is worth checking before anything else -- three of this session's four bugs
have been exactly that.

## The last measured dead space, reclaimed

`XBOX_WORKER_STACK_COUNT` was 16, reserving 4 MB directly under the arena for
the tick-driven model. Nothing in this tree calls `xbox_worker_stack_alloc`, and
the header already said the pool is "unused address space and dead code" for
every main-loop title. It now defaults to 0 slices, so the stack region is the
2 MB main stack (measured high-water: 3 KB) and a tick-driven title builds with
`-DXBOX_WORKER_STACK_COUNT=16`.

Arena 57,606,144 -> 61,800,448. It did not fix G9, for the reason above.

## Changes

- `src/kernel/xbox_memory_layout.c` — leading-split reuse at the caller's
  alignment; `heap_absorb_frontier`; per-owner slack, per-export alloc/free
  tallies, and the failing caller's ordinal and return address in the
  out-of-memory line.
- `src/kernel/xbox_memory_layout.h` — `XBOX_STACK_SIZE` 8 MB → 6 MB, with the
  measurement that justifies it.
- `src/kernel/xbox_memory_layout.h`/`.c` — `g_xbox_low_base`, derived from the
  loaded image; `XBOX_KERNEL_DATA_BASE`, `XBOX_STACK_BASE`, `XBOX_HEAP_BASE` and
  the primary-TLS addresses now follow it.
- `diagnostics/jsrf_first_fault/main.c` — `RECOMP_TOTAL_RAM_MB`, diagnostic only.
- `src/kernel/kernel_bridge.c` — rate-limited reserve/commit log for
  `NtAllocateVirtualMemory`.
- `diagnostics/jsrf_first_fault/heap_alloc_test.c` — sixteen 16-byte pool blocks
  must fit in one page; a page-aligned request must be servable from a
  misaligned free block; the skipped lead must come back reusable.
- `diagnostics/jsrf_first_fault/main.c` — `RECOMP_HEAP_REPORT` and
  `RECOMP_STACK_REPORT`.

Tests: `ctest` 10/10, `test_combiner_trace.py` OK.

## Runs

- `claude-heap-19/20` — the growth curve and the slack breakdown, before any fix.
- `claude-heap-21` — granularity fix alone. Slack 4.58 MB → 1.94 MB.
- `claude-heap-22` — padding reclaim. Stalled at `IoCreateDevice`, no draws.
- `bisect-no-absorb`, `bisect-no-pad-reclaim` — which of the two did it.
- `claude-heap-24` — after the stack resize. Gets further, fails on a later
  request.
- `claude-heap-27` — per-export alloc/free tallies. **The diagnosis.**
- `claude-heap-28` — final state: four genuine screens, then the disc error.
