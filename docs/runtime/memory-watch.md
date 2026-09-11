# Translated guest memory watch

`RECOMP_MEM_WATCH` answers a common static-recompilation debugging question:
which translated guest instruction wrote this RAM value? It records covered
translated-guest stores that overlap one selected byte range, including writes
that store the value already present.

> **Important coverage warning:** this is not yet a complete write monitor.
> Absence of a record does **not** prove that the bytes were not modified. An
> unsupported translated store, kernel/HLE code, or a device/runtime writer can
> still modify them. There is no generic guest-memory read tracing.

## Running a watch

The syntax is:

```text
RECOMP_MEM_WATCH=<guest_va>:<length>
```

Both fields accept decimal or C-style hexadecimal (`0x...`) numbers. `length`
is a byte count, and a store is reported when any byte of its access overlaps
the half-open range `[guest_va, guest_va + length)`. The requested range must
currently fit wholly inside one mapped RAM view.

For example, on a POSIX shell:

```sh
RECOMP_MEM_WATCH=0x00123450:4 ./mygame
```

On Windows Command Prompt:

```bat
set RECOMP_MEM_WATCH=0x00123450:4
mygame.exe
```

Omit or unset `RECOMP_MEM_WATCH` to disable the facility. An empty value also
leaves it disabled. An invalid or unsupported range produces a diagnostic and
does not arm the watch.

## Trace records

Records are written to stderr as one line per matching store:

```text
[MEM-WATCH] source=guest pc=0x00012157 function=0x00012100 va=0x040DC258 ram=0x040DC258 width=4 old=0x00000003 new=0x00000004
```

| Field | Meaning |
|---|---|
| `source` | Writer class. V1 records only `guest`, meaning translated guest code. |
| `pc` | Exact original Xbox instruction address that performed the store. |
| `function` | Start address of the translated guest function containing `pc`. |
| `va` | Effective 32-bit guest virtual address used by the instruction. |
| `ram` | Normalized byte identity in the shared RAM backing mapping. |
| `width` | Width of the complete store in bytes, not the watch length. |
| `old` | Complete stored-width value read immediately before the store. |
| `new` | Complete stored-width value written by the instruction. |

`old` and `new` are hexadecimal bit patterns. Floating-point values are shown
as their raw IEEE bits rather than converted decimal values. Same-value writes
are intentionally reported because this traces accesses, not only changes.

## RAM aliases

The watch and every covered access are normalized before their ranges are
compared. The base RAM mapping, each generic mirror that actually mapped, the
tiled aperture when mapped, and the optional shared physical-heap window after
it is enabled resolve to one RAM byte identity. A watch expressed through one
of those aliases therefore catches covered writes through another.

The rest of the contiguous aperture is separate storage and is not falsely
treated as an alias. Stores or watch ranges that cross the end of a single RAM
view are unsupported in v1.

## Current coverage

| Store class | Traced? | Notes |
|---|---:|---|
| Scalar `mov` | Yes | 8/16/32-bit memory destinations |
| Arithmetic/bit memory destination | Yes | Ordinary read/modify/write lowering, including indexed bit strings |
| `setcc` and `pop` memory destinations | Yes | Use the ordinary operand writer |
| Scalar SSE | Yes | 32-bit float and 64-bit double stores; values shown as bits |
| Guest stack `push`/call return address | **No** | Still uses `PUSH32` |
| x87 value/status/control stores | Yes | `FST[P]`, `FIST[P]`, `FNSTSW`, and `FNSTCW` memory forms |
| Packed SSE | **No** | `XMM_STORE` helpers |
| MMX | **No** | `MMX_STORE`/`memcpy` helpers |
| MOVS/STOS, REP or scalar | **No** | Direct loops plus `memcpy`/`memset` fast paths |
| XADD/CMPXCHG, locked or unlocked | **No** | Dedicated paths; locked forms must preserve atomicity |
| Kernel/HLE writes | **No** | Separate source without a translated guest instruction PC |
| Device/runtime/DMA writes | **No** | Separate source without a translated guest instruction PC |
| Guest-memory reads | **No** | No generic read-tracing path exists |

Other unimplemented x87 environment/save forms are not claimed as covered.
The bold **No** entries are blind spots. If a watched value changes without a
record, inspect these paths before concluding that the write happened outside
translated code.

## Cost when disabled

Normal game targets compile covered stores with one test of the global enabled
flag followed by the original direct typed store. They do not load the old
value, call the recorder, normalize an address, or format output when the
variable is absent. Standalone conformance builds that do not link
`xbox_kernel` compile the helper directly to the original store with no runtime
tracer dependency.

## Provenance design

The exact `pc` is attached while the Capstone instruction is decoded. The
translator already knows the authoritative start of each function it emits,
so it writes `function` as a second generated constant. This avoids compiling a
large address-to-symbol table or performing a runtime lookup when a watch hits.

To inspect a record manually, search the generated C or disassembly for the PC:

```sh
rg '0x00012157' path/to/gen path/to/disasm/asm
```

The JSRF-only `RECOMP_GUEST_BLOCK` macro remains independent: it has no generic
emitter and represents a recent block rather than the exact writing
instruction. Future block or symbolic names can augment the record without
changing the explicit store seam.

Kernel/HLE and device/runtime writers should remain separate future producers
with truthful `source` values. Read watches would require parallel explicit
read helpers; instrumenting `MEM*` blindly would lose the distinction between
lvalue reads and writes.
