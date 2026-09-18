# Are there debug symbols? — answered, 18 September 2026

Short answer: **no PDB, no source filenames, in either shipped build.** But the
question turned up two things worth keeping, and one standing lead.

## What was checked

| | US retail | EU retail |
|---|---|---|
| `debug_pathname` | `D:\Jet2\Src\JetData\Jet2___Xbox_X_BOX_ReleasePresent\Jet2.exe` | `D:\Jet2\Src_Pal\JetData\Jet2___Xbox_X_BOX_ReleasePresent\Jet2.exe` |
| `debug_filename` | `Jet2.exe` | same |
| `.pdb` path string | none | none |
| `.cpp` / `.cxx` strings | **0** | **0** |
| `.pdb` / `.map` file on disc | none | none |

Both configurations are `Xbox_X_BOX_ReleasePresent` — release builds with the
assert macros compiled out, which is exactly the case
`tools/debug_symbols/README.md` says its recovery pass cannot help with. It
mines `__FILE__` literals from a **debug** build; there are none here to mine.
The EU disc was extracted with `tools.xiso` purely to check, and is no better.

The internal project name is **Jet2**, built from `D:\Jet2\Src\JetData\`
(`Src_Pal` for PAL). That is the only naming information the binaries carry.

## What we have instead: the section table

Already known and used — `docs/jsrf/STATUS.md:62` places the top crash site
inside DSOUND — but worth having in one place, because it is the closest thing
to symbols this title has:

    .text        0x00011000 - 0x0018CB30   1,555,248   the game's own code
    D3D          0x0018CB40 - 0x0019E338      71,672   XDK Direct3D8
    DSOUND       0x0019E340 - 0x001BA89C     116,060   XDK DirectSound
    MMATRIX      0x001BA8A0 - 0x001BBAB0       4,624
    XGRPH        0x001BBAC0 - 0x001BC7BC       3,324
    XPP          0x001BC7C0 - 0x001C3F58      30,616   pad / USB
    .rdata       0x001C3F60 - 0x001EB760     161,792
    .data        0x001EB760 - 0x0027E074     600,340
    DOLBY        0x0027E080 - 0x00284E18      28,056   Dolby encoder
    $$XTIMAGE    0x00284E20 - 0x00287620      10,240
    $$XSIMAGE    0x00287620 - 0x00288620       4,096

The XDK libraries are in their **own sections**, so any address can be
classified as "the game's code" or "library X" for free.

## Two things that classification settles

**The two crashes are in different code, and now provably so.** The known
crash — 09:38 and 16:16 on 17 Sep, and the one G1a's FEDEC_HOLD was built for —
sits at `001A2E2E`, `001A24BE`, `001A25AA`, all inside **DSOUND**: XDK library
code, which is why nobody in the ecosystem can fix it for us and why the only
levers are what our APU model hands it.

Tonight's new crash is at `00011EE0`, `000123E0`, `00013A80`, `00013F80`,
`0006F9E0`, `00147FB4` — every frame inside **.text**. That is **the game's own
code**, not the XDK. Two different failures, confirmed by the section table
rather than by the stacks looking different.

**There is a DOLBY section, and it is 28 KB of real code.** That is a
consequence for G11 nobody had noticed: `XC_AUDIO` used to advertise AC3
(bit 16), and the title ships an encoder to honour it with. Advertising an
encoded output to a title that has the encoder linked in is a different
proposition from advertising it to one that does not — it is a plausible route
into a code path that never otherwise runs. It does not prove the old value was
harmful, and the flip is still gated on a listen, but it raises the prior.

## The standing lead

`tools/debug_symbols/README.md` gives the advice this exercise confirms:

> It is worth hunting for a debug/beta XBE of your target before starting: one
> exists for far more Xbox titles than people expect, and it is worth more than
> any amount of manual reversing.

Neither shipped build is one. A prototype, review or debug build of Jet2 would
carry one `__FILE__` literal per source file with an assert in it, and the
existing tool would turn those straight into function-to-source-file names
across the 1.55 MB `.text`. That is the single highest-leverage artefact anyone
could find for this project, and it is a search, not an engineering task.
