# The emulated hard disk: ours, upstream's, and Microsoft's

19 September 2026, evening. No run of the title; every number below is read
from code, from the 360 BC packages under `~/jsrf/BC/`, or from the disposable
HDD trees four earlier runs left in `/tmp`.

Prompted by a measurement: replaying the player's own recorded session from the
**stock** emulated-HDD tree landed ~30 s behind the same session on the
**player's** tree, with the input stream identical and every checkpoint
"aligned". The difference is entirely the disk the title boots from, so the
disk is worth reading properly.

## 0. The one-paragraph answer

Our HDD model is a **path translator over host directories**, and Microsoft's
BC emulator is a **volume manager over real partitions**. Ours is simpler than
the console in three ways that are deliberate and one that is a defect: we
collapse X:, Y: and Z: onto a single `Cache/` directory, we answer every
free-space question with the host's whole disk, we model no I/O latency, and we
give the title a **900x overstatement of its cache space**. The first three are
defensible. The fourth is measurable and should be fixed.

## 1. Where the storage logic lives — the architectural difference

This is the difference that explains most of the others, and it is not a
quality gap.

**JSRF statically links XAPILIB 4134** (`CLAUDE.md:24`). So
`XapiSelectCachePartition`, `XMountUtilityDrive`, `XapiFormatFATVolumeEx`,
`XSetFileCacheSize` and `XCreateSaveGame` are **inside the title's own code**,
recompiled to C by `tools/recomp` like everything else, and they call down to
our `Nt*` layer. Verified: none of those names is implemented anywhere in
`src/kernel/`, and none appears as an import in `default.xbe`.

On the 360 those same names are what the BC title modules **export**: the
per-title `xefu_<guid>.dll` symbol blobs list `_XapiSelectCachePartition@12`,
`_XMountUtilityDrive@4`, `_XapiValidateDiskPartitionEx@8`,
`_XapiFormatFATVolumeEx@8`, `_XapiMapLetterToDirectory@24`,
`_XapiSetupPerTitleDriveLetters@8`, `_XGetDiskClusterSizeA@4`,
`_XCreateSaveGame@24`, `_XapiNukeDirectory@8`. Microsoft **reimplemented the
XAPI storage layer**; we **recompile the title's own copy of it**.

Consequence: Microsoft had to decide what a cache partition *is*, because their
code creates and formats one. We inherit those decisions from the title, and
only have to make its `Nt*` calls come out right. That is why our layer is
1,800 lines and theirs is a volume manager — and also why a wrong answer from
our layer is invisible until the title's own XAPI does something odd with it.

## 2. Path and partition mapping

| | ours (`kernel_path.c:44-81`) | upstream | BC emulator (`xefu7` strings) |
|---|---|---|---|
| disc | `\Device\CdRom0\`, `D:\` → game dir | same | `\Device\CdRom0\default.xbe` |
| writable data | `Partition1\` → **save side** | → **game dump** | `\Device\Harddisk0\Partition1` |
| system | `Partition2\` → game dir; bare → `SystemData/` | same | `Partition2\xboxdash.xbe` |
| caches | `Partition3/4/5` → **one `Cache/` dir** | same | `\Device\Harddisk0\Cache0/1/2`, `\??\cache:\Xbox0\`, `\Xbox1\`, `\Xbox2\` |
| drive letters | `T:`→TitleData, `U:`→UserData, `Z:`→Cache, `Y:`→game dir, **`X:` unmapped** | same | `\??\X:\Dash\`, `\??\X:\config.bin`, `\??\Y:\Xbox1\TDATA\%08x`, `\??\Y:\$X\content` |
| raw devices | Partition0 = 512 KiB in-process buffer (`kernel_file.c:49-56`); Partition5 = 750 MiB sparse page store (:58-64) | Win32 only: sparse `.img` files | real partitions; `FsdxGlue` dispatcher, "Unexpected volume %08X (expected 0-6)" |

**Three real differences.**

*Upstream routes `Partition1\` into the game dump*, so a title writing TDATA
writes into the disc directory. We send it to the save side
(`kernel_path.c:47-48`). Ours is right and is worth sending upstream.

*We collapse three cache volumes into one directory.* The console has three
(`Cache0/1/2` → X:, Y:, Z:), and BC keeps them distinct. JSRF only ever uses
Z:, so this has never bitten — but a title that selects a cache partition with
`XapiSelectCachePartition` and expects independent free space would see one
directory wearing three hats.

*X: is unmapped in our table.* BC opens `\??\X:\config.bin` and `\??\X:\Dash\`.
If JSRF ever probes X: it gets "Unrecognized Xbox path". It does not today.

## 3. The cache partitions at launch — the faithfulness question

**What we do: nothing.** Neither our layer nor upstream's clears, formats, or
retains a cache by policy, and neither has any title-ID logic. The tree is
whatever was staged. `ensure_parent_dirs()` (`kernel_file.c:1095-1140`) creates
intermediate directories the title does not, which is the only automatic
behaviour in the path.

**What the console does**, and this is the part that matters: the cache
partitions belong to *whichever title launched last*. Launch the same title
again and its cache is still there. Launch a different one and the cache is
formatted. So there are two faithful states, and **a populated cache is the
steady state for a title you have played before**.

**What BC does: undetermined.** The emulator names exactly three cache roots on
the 360's own `Cache%u` partition, and keeps per-title data separately under
`Y:\Xbox1\TDATA\%08x`. Nothing in the packages states whether Xbox0–2 survive a
relaunch, and no partition-size or free-space constant appears anywhere. The
placement on the 360's Cache partition — which the 360 treats as volatile —
*suggests* no persistence guarantee, but that is inference and is labelled as
such. The per-title config tables carry no storage override for JSRF
(`5345000A`: `cfg1=0x02000010 cfg3=0xd8 cfg5=0x40`, a thread-cap tweak) or for
the Sega GT bundle launcher (`4D53003D`). The only storage-related field
documented across 190 titles is **cfg3**, an Fsdx bitfield whose low two bits
force synchronous I/O on read/write opens; JSRF, the bundle and Conker all
carry the common value `0xd8`, i.e. no sync forcing.

**Measured here, and this is the useful part.** The first boot from an empty
cache costs **1,410 file opens**; from a populated one, **131**. The build
writes 258 files, 119 MB, under `Cache/Media/`, ending with the marker
`Z:\Media\Cache\JSRF_CACHE_COMPLETE%02d.CMP` — a path string in the title's own
image, beside `D:\Media\Cache\DmCache%02d.tbl`, so the title is copying media
from the disc into the cache drive. **The build is deterministic**: the
player's cache and a run-built cache are byte-identical, and two independently
built caches differ in only two files.

Those two files are `Cache/Media/Mark/DEFAULT/JSRF_TEXS0.JTX` and `…S1.JTX`,
1 MiB each, differing across ~295 KB of their middles with texture-like
statistics on both sides (entropy 3.7 bits/byte, zero fraction 0.34 vs 0.42).
These are graffiti tag textures written into the cache at boot. **Filed as a
lead, not a finding**: the checkerboard rectangles where tags belong were in the
player's 19 Sep session, and a texture the title renders before saving would be
exactly the kind of thing to differ run to run. Nothing yet shows the title
renders them rather than decoding them.

**The consequence for every measurement in this project.** The harness copies
the stock tree — empty cache — for every run, so all 782 corpus runs replayed a
*first launch*, spending their first ~30 s on a cache build the player never
pays. A warm copy now exists at `~/jsrf-build/emulated-hdd-warm` (stock tree,
title-built cache). Nothing points at it yet; switching `JSRF_HDD_SRC` to it
would make harness boots match the player's, and would re-base boot-window
numbers exactly as the render-path change did this afternoon.

## 4. Free space — the defect

`NtQueryVolumeInformationFile` answers one class, `FileFsSizeInformation`, with
**FATX geometry hard-coded correctly**: 512-byte sectors × 32 = 16 KB clusters
(`kernel_file.c:775-785`). That number is load-bearing and the comment says why:
Half-Life 2's CRT startup requires exactly 0x4000 and returns
`STATUS_DEVICE_NOT_READY` otherwise, aborting before `main`.

The **capacity** beside it is not right. On POSIX we `fstatvfs` the host volume
(`:1467-1480`); on Win32, upstream and we both call `GetDiskFreeSpaceExW(NULL,…)`
— the *current directory's* volume, not the handle's. On this machine that
means the title asking how much room its cache drive has is told:

| | reported | real Xbox |
|---|---:|---:|
| total | 1,858 GB | 750 MB (cache partition) |
| available | 690 GB | ≤ 750 MB |

**900x over.** BC's title modules call `_GetDiskFreeSpaceExA@16` and
`_XGetDiskClusterSizeA@4`, so the console's numbers reach the title through the
same shape of query; Microsoft's emulator necessarily answers with partition
sizes because it manages partitions.

Why it might matter, stated as a hypothesis with its test: JSRF decides at boot
how much media to cache, and a title that believes it has 690 GB may cache
differently from one told 750 MB — possibly *all* of it, possibly changing what
it streams later. The test costs one pair of runs: clamp the reply per mapped
volume (750 MB for the cache, ~4.8 GB for Partition1) and compare the cache
build's file count and byte size against today's 258 / 119 MB.

## 5. FATX semantics we do not model

Ours and upstream's are the same here, and the list is the honest one:

- **No 42-character name limit**, no forbidden-character rejection. A host file
  the title could never have created will enumerate happily.
- **Case**: `fnmatch(FNM_CASEFOLD)` on POSIX enumeration, but the underlying
  filesystem decides; on a case-sensitive host, two names FATX would consider
  identical are distinct.
- **Enumeration**: only `FileDirectoryInformation`; dot directories hidden
  (`kernel_file.c:980`). Real FATX order is directory-entry order; ours is
  whatever `readdir` gives.
- **No timestamps or attributes** carried faithfully.
- `NtSetInformationFile` on POSIX **lacks `FileAllocationInformation`**, in both
  trees.
- `NtFsControlFile`/`NtDeviceIoControlFile` are stubs upstream; we serve the
  ioctls the synthetic devices need (`kernel_file.c:1742-1832`) and accept
  `FSCTL_DISMOUNT_VOLUME` on the Partition5 handle (:1697-1740).

One import JSRF makes is unresolved in **both** the thunk table and the bridge:
ordinal 91, `IoDismountVolumeByName` (`xbox_kernel.log:6-8`). Upstream has a
stub. This is the cheapest thing on the list to close.

## 6. I/O timing

Neither we, nor upstream, nor — as far as the packages show — BC model any seek
latency or transfer rate. No key, string or constant for throttling exists in
any of the four BC configs or in the decompressed emulator; the only
storage-behaviour knob is cfg3's synchronous-I/O bits.

Host I/O is effectively instantaneous, so the cache build that takes a console
tens of seconds of real seeking takes us 1,410 opens' worth of nothing. That is
a *fidelity* gap with no evidence yet that it is a *behaviour* gap, and it is
not worth closing speculatively. It is worth remembering when reading any
boot-window timing.

## 7. Save data

Ours: `U:\` → `UserData/`, `T:\` → `TitleData/`, with `TDATA/UDATA/<titleid>/`
trees underneath as ordinary directories. The player's disk carries a real save
at `UDATA/5345000a/99271B32E8BB/` with `SaveMeta.xbx`; the stock tree has none.
BC keeps the equivalent under `Y:\Xbox1\TDATA\%08x` and `\??\Y:\$X\content`, and
the GDK layer maps it to cloud save containers — a concern we do not have.

The harness copies the whole tree per run into `/tmp` and deletes it after, so
staging the player's disk is safe: their save is never written through. That is
what makes replay-from-the-player's-disk viable at all.

## 8. What to do, ranked

1. **Clamp free space per volume.** 750 MB for the cache drives, ~4.8 GB for
   Partition1, instead of the host's disk. One pair of runs measures whether the
   cache build changes. *Evidence: 690 GB reported against 750 MB real.*
2. **Point the harness at a warm HDD by default.** `~/jsrf-build/emulated-hdd-warm`
   exists. Every corpus run so far replayed a first launch the player never
   performs. Re-bases boot-window numbers; say so where they are quoted.
3. **Record the HDD identity in a pad recording** and refuse or warn on
   mismatch, as the header already does for the generated tree. A hash over
   `UDATA/` plus whether `Cache/` is populated would have caught this
   afternoon's misaligned replay immediately instead of after two runs.
4. **Stub `IoDismountVolumeByName`** (ordinal 91) from upstream — JSRF's one
   genuinely unresolved import.
5. **Pull upstream's RootDirectory handling on the bridge side.** Our
   `kernel_file.c` supports RootDirectory-relative names on Win32 but the bridge
   sets `oa->RootDirectory = NULL`, so it is unreachable from guest code; POSIX
   supports it on neither side.

And worth sending upstream, in this order: Partition1 → save side (upstream
writes TDATA into the game dump), the synthetic Partition0/Partition5 devices
with their ioctls, `ensure_parent_dirs`, and the enumeration-context release on
`NtClose`.

## What this does not answer

- Whether the cache-drive free space changes what JSRF caches. Item 1 above.
- Whether BC retains or clears its cache roots between launches. Not
  determinable from the packages; the per-title tables for three of the four
  donors are inside an encrypted `xefutitle.xex`, and the PC-side `Emu.exe` is
  not in any extraction.
- Whether the two differing `JSRF_TEXS*.JTX` files are a rendering fault. They
  are a lead with a mechanism worth one look, not a finding.
- Why replaying the player's recording **from a copy of their own disk** still
  did not reproduce their frame timing. The cache explains the stock case and
  not that one. That is a recorder question, not an HDD one.
