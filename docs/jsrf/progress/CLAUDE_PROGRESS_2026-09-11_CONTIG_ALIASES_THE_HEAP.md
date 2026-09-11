# Cache slot 0x99 was never wrong: the contiguous arena is parked on the heap

Date: 2026-09-11 (Europe/London), late night. Follows the Windows render work
in `CLAUDE_TO_CODEX_HANDOVER_2026-09-11_WINDOWS_RENDERS.txt` and the crash that
replaced it once the synthetic IRQ thread stopped running the GPU ISR.

## The question, and the answer it did not have

The furthest Windows crash was a render chain

    sub_0005C840 -> sub_0014FDE0 -> sub_0018DF10

passing texture-cache index **0x99**, whose slot resolved to `0xFFFFFFFF`.
`sub_0018DF10` AddRefs every non-null texture (`add dword ptr [ebx], 0x80000`
at 0x0018DFA7), so the sentinel is dereferenced and the guest faults.

The brief was a macOS-versus-Windows differential of that slot: created when,
written when, cleared when, read by whom -- to decide between a missed binding,
a wrong clear, a wrong object/index, and a missing sentinel guard.

**It is none of the four.** Both hosts create, publish and bind slot 0x99
identically. The slot is overwritten afterwards, by our own contiguous
allocator, which on Windows hands out physical offsets that land on top of
guest low memory.

## The differential

`RECOMP_TEXTURE_SLOT=<index>` was added to `jsrf_texture_cache_probe`,
`jsrf_texture_bind_probe` and a new `jsrf_texture_create_probe` (site
0x0014F720, the shared creation body). It prints every create, release, store
and bind of one index **uncapped** -- the existing limits are 512 stores and
8 binds, and slot 0x99 is the 155th creation and the 13,989th bind, so both
caps hid it completely. This is the "print caps are counters" trap again.

Same gen, same tree, one build each:

    macOS    WATCH create=155 index=153 caller=0006E351 slot-was=00000000
             WATCH release=155 index=153 old=00000000
             WATCH store=155 pc=0014F801 index=153 new=045B2520 old=00000000
             WATCH bind x4453  resource=045B2520 caller=0005C948 invalid=0

    Windows  WATCH create=155 index=153 caller=0006E351 slot-was=00000000
             WATCH release=155 index=153 old=00000000
             WATCH store=155 pc=0014F801 index=153 new=045B01E0 old=00000000
             WATCH bind       resource=FFFFFFFF  caller=0005C948 invalid=4

Identical creation number, identical caller, identical publish site
(0x0014F801, the success store -- not 0x0014FA19, which is where the body's
local initialised to -1 would go). A real resource pointer, 0x045B01E0 against
0x045B2520. Then, with no further store or release of that slot anywhere in
the run, the bind reads `0xFFFFFFFF`.

The bind probe dumps the table on the first bad bind. It is not one slot:

    table=0061A000 count=500
    143=04525B80 144=045A90D0 145=045A9100 146=045A9160 147=045A9160
    148=045A9190 149=006A91C0 150=FF002300 151=FF003FFF 152=119F119F
    153=FFFFFFFF 154=FFFFFFFF 155=00000000 156=045B0270 ...

Slots 143-148 hold a clean +0x30 run of resource pointers. Slot 149 is
`045A91C0` with its top two bytes rewritten. 150-155 are debris. 156 is a
valid pointer again. That is a byte range overwritten in place, starting
mid-dword at **0x0061A256** -- not a cache that lost track of an entry.

(`119F119F` in slot 152 is also the `[esp+0x10]` value in the original crash
dump: the bind one call earlier put that slot's contents into stage 0, so
0x0018DF10 recorded it as the outgoing texture. The two observations agree.)

## Where the write comes from

`RECOMP_STORE_WATCH=0x0061A254:0x1C` cannot answer this. The page's first
toucher is the guest's own table-zeroing `rep stosd`, lifted to a CRT `memset`,
and the VEH decoder only completes single stores -- so the guarded page faults
inside ntdll and the run dies before creation 155. **A store watch on a page a
bulk copy touches is not an available instrument.** The run reached nothing,
and its zero hits prove nothing; noted rather than believed.

The allocator answers it directly. Windows `xbox_ContiguousAlloc` is a bump
arena over physical offsets, floored only above the loaded image. A one-shot
report of blocks whose range crosses `XBOX_STACK_BASE`:

    [CONTIG] arena starts above the image: 0x80290000 (image ends 0x00288620)
    [CONTIG] block 0x00291000..0x00311000 overlaps guest low memory
             (stacks 0x00310000, heap 0x00610000)
    [CONTIG] block 0x00314000..0x003AC000   <- the framebuffer ([FB] 0x00314000)
    [CONTIG] block 0x004D8000..0x00604000   <- the depth surface
    ...
    [CONTIG] block 0x00618000..0x00620000   <- covers the texture cache table
                                               at 0x0061A000..0x0061B194

**Every contiguous block, from the first one, is inside the guest's stacks or
heap.** The 32 KB block at 0x00618000 is the one sitting on the cache table;
heap block #4 (`size=4500 align=4096 -> 0x0061A000`) is that table. Whatever
the title writes into its contiguous block writes over the table.

The floor raised in a0cbb2a was correct and insufficient: it moved the arena
off `.text`/`.data`, but the stacks (0x00310000) and the heap (0x00610000) sit
above the image too, and the heap grows to the top of RAM. **There is no floor
that clears them.** The POSIX branch has never had this problem for the stated
reason that it allocates from the guest heap -- one pool, which is what the
hardware has.

## The A/B

`RECOMP_CONTIG_FROM_HEAP=1` makes the Windows branch do what POSIX does:
`xbox_HeapAlloc`, returning the heap VA with `XBOX_CONTIG_BASE` set so D3D's
DMA_GET comparison still holds and the GPU's physical offset still resolves to
the same bytes (guest RAM is mapped 1:1). Opt-in; the default path is
unchanged. Two 110 s runs, same build, same gen, same disposable HDD:

                                    two pools      one pool
    guest access violations                 1             0
    invalid texture binds                   5             0
    contiguous blocks on guest RAM        64+             0
    textures prepared                   2,181       106,840
    triangles rasterised               32,000    22,234,500

No allocation failure, no arena exhaustion, no heap miss in the one-pool run.
Slot 0x99 binds `045B1820` for the whole run, `invalid=0`.

**And it is not the fix.** Those numbers say the corruption stopped; they do not
say the GPU can see what the guest wrote, and the draw totals rise either way.
On Windows `g_contig_memory` is `VirtualAlloc`'d storage that is deliberately
*not* a view of the RAM mapping, so a block the guest fills through
`0x80XXXXXX` is invisible to a GPU resolving `dma_base + offset` with no mask.

`RECOMP_CONTIG_VERIFY=1` compares the two views at the moment the model
resolves a surface (`prepare_texture_copy`), summing 4 KB from the middle of
the buffer rather than the first dword -- a surface's top-left corner is black,
and four leading zeroes equal to four other leading zeroes is an agreement that
cannot come out any other way. `BOTH-EMPTY` keeps a vacuous agreement legible.

    macOS (physical heap alias on, the harness enables it by default)
        232 resolves: 142 match on real data, 90 BOTH-EMPTY, 0 MISMATCH
        e.g. low 0063B000 sum=39113343 | high 8063B000 sum=39113343

    Windows, RECOMP_CONTIG_FROM_HEAP=1
        119 resolves: 0 match, 74 BOTH-EMPTY, 45 MISMATCH
        e.g. low 00CE6000 sum=00000000 | high 80CE6000 sum=9BAA1BD1

A third arm, Windows with the flag off, is **inconclusive**: it reproduced the
original fault (1 access violation, 5 invalid binds, 64 overlap reports, 35,000
triangles) and died before any resolve carried data, so its 20 resolves are all
BOTH-EMPTY and say nothing about the two views. Recorded rather than counted --
a crashing arm that stops early is not a measurement of anything else.

macOS is the positive control and must match by construction; it does, on
non-zero data, which is what makes the Windows result readable. The guest's
texture bytes are in the contiguous window; the GPU samples the heap block,
which is zeros. One line shows the same split the other way -- `low` holding
`FFFFFFFF FFFFFFFF` against an empty `high` -- the GPU clearing a render target
into memory the guest never reads.

So the flag removed the crash by making the contiguous window **inert**. The
22M triangles were rasterised from blank memory. It stays an opt-in escape
hatch and a diagnostic, not a fix.

## What this does and does not settle

It settles the crash and the texture debris. It does not make the CrossOver
window show anything -- the framebuffer path is still POSIX-only in `main.c`,
and `[FB] nonzero=0` there means only that, as the handover already says.

It is also very likely the standing explanation for other Windows-only oddities
that have been read as separate bugs, because everything the title allocates
contiguously has been landing on its own stacks and heap since the first frame.
The 0xBC60 voice link in handover item (b) is the obvious one to re-check with
the flag on before spending a session on it.

## Upstream has this too, in a worse form

`git show upstream/main:src/kernel/xbox_memory_layout.c` is the bare bump arena
from `XBOX_CONTIG_BASE` with **no image floor at all** -- 0 matches for "arena
starts above the image". Upstream's first contiguous allocation has physical
offset 0 and climbs through the loaded image, the stacks and the heap in turn,
and Windows is upstream's only supported host. This is not a JSRF defect and
not a porting artefact; it affects every title on the toolkit. We are keeping
the fix local for now, but it is worth raising if the project ever wants it.

## The fix, and the first Windows frame

`xbox_EnablePhysicalHeapAlias` now has a Windows implementation. The window is
rebuilt at the end of layout init -- while it is still single-threaded, and
late enough that the heap bounds are final -- as committed storage below
`XBOX_HEAP_BASE`, a view of the RAM section across the heap, and committed
storage above it. The low slice is copied out and back rather than assumed
empty, because the kernel's fake PE header lives at window offset 0x10000. It
proves the alias with a write and a read the way the tiled aperture does, and
`xbox_ContiguousAlloc` then takes from the heap whenever the view is live,
falling back to the floored arena (and its overlap report) when it is not.
`RECOMP_PHYSICAL_HEAP_ALIAS=0` is the way out. `RECOMP_CONTIG_FROM_HEAP` is
gone -- the alias decides.

    Physical heap alias: 0x80610000..0x84000000 shares low RAM;
    contiguous allocations now come from the heap

Same texture, same block, before and after:

    one pool   low 00CE6000 sum=00000000 | high 80CE6000 sum=9BAA1BD1  MISMATCH
    aliased    low 00CE6000 sum=9BAA1BD1 | high 80CE6000 sum=9BAA1BD1  match

                              two pools   one pool    aliased
    CONTIG-VERIFY mismatches     n/a          45           0
    access violations              1           0           0
    invalid texture binds          5           0           0
    contiguous blocks on RAM     64+           0           0
    textures rejected          4,938      10,294           0

**The framebuffer has pixels.** Every previous Windows run reported
`nonzero=0/153600` for its whole life. This one changes continuously from
t=12 s, twice reaching a fully covered frame:

    [FB] t=  12.00 0x006F0000 sum=63719352 nonzero=  7207/153600 CHANGED
    [FB] t=  60.00 0x006F0000 sum=73C200E0 nonzero=153600/153600 CHANGED
    [FB] t= 108.00 0x006F0000 sum=17218E1E nonzero=  8108/153600 CHANGED

Texture rejection is 0 for the entire run, against roughly half in every run
before it. The triangle total is *lower* (5,000 against the one-pool run's
22.2M) and that is expected: the earlier figure was a software rasteriser
sweeping blank memory as fast as it could, and this one is doing real textured
work at frame pace.

macOS is unregressed on the same build: 157 matches, 0 mismatches, no faults,
no invalid binds, still presenting (`presented nonzero=290856`). `ctest` shows
the same six failures this gen has always had, and `jsrf_heap_alloc`, which
exercises the alias directly, passes.

## Next

The oracle renders now, so the two open items from the previous handover are
worth re-measuring rather than reasoned about: the 0xBC60 voice link, and where
submission stops. Both were observed on a host whose GPU was reading blank
memory.
