# Game-data tools and unattended launchers (24 Sep 2026)

Built during the 24 Sep night session from KeybadeBlox's JSRF-Decompilation,
JSRF-ChapterSelect and GG-Notebook (codeberg.org/KeybadeBlox; see the licence
note in the project memory before publishing anything derived from the
decomp or ChapterSelect). All Python runs on `/usr/bin/python3`; `disx.py`
needs capstone there.

## Save data

- `progress.py FILE` -- decodes `JSRFDATA.SAV` (the on-disk save: 20-byte
  HMAC-SHA1 signature keyed with the recomp kernel's all-zero
  XboxSignatureKey, three 0x3560-byte slots, each a 0x50-byte key plus the
  scrambled sdData; `CSaveData::Decode` 0x3A920 ported) or a raw memory dump
  of `CSaveData` at 0x1EFFB0 (`RECOMP_STAGE_MEM=1effb0:8000`). Prints return
  chapter/mission, flags, souls, per-stage tag state (3 bits per G mark,
  7 = unpainted), misc objectives, events seen.
- `savetool.py IN.SAV --prove` -- round-trip proof (decode -> encode -> sign
  reproduces the file byte for byte). `savetool.py IN.SAV -o OUT.SAV [edits]`
  edits flags / chapter / mission / spawn / events seen / stage tags and
  re-signs. `--make-checkpoints DIR` writes the cutscene checkpoints whose
  descriptions are in `checkpoints/*/dump.txt` (the .SAV files are not
  committed: they are derived from the player's own save). It refuses to
  write into the player's HDD.

## Missions and events

- `missions.py`, `missions-all.txt`, `chapter-stage-map.txt` -- mission
  bytecode scanner (GG-Notebook `missions/mission_bin.hexpat`) and its output:
  which mission plays which event under which flags.
- `evdump.py 034 [111 ...] [--png=DIR]` -- event `.dat` parser
  (GG-Notebook `events/event_dat.hexpat`): per-scene timeline of camera,
  models, animations, fades, ADX cues, talk lines, texture formats.
- `evsec.py`, `xbe.py`, `disx.py` -- helpers (event section counts, XBE
  reader, disassembler).
- `match_reads.py`, `summ.py` -- match `[READ]` lines in a log against the
  .adx files to prove how long a music stream stayed alive.

## Unattended launchers (`harness/`)

Output goes to `$JSRF_RUNS` (default `~/jsrf-build/runs`). All run the
build-feav binary with audio silenced; one game instance at a time.

- `jump_run.sh NAME CAPTURES GAP MAXWAIT [--env K=V ...]` -- Garage boot,
  then `RECOMP_CHAPTER_JUMP=<chapter>:<mission>` (chapter_select.c) and
  captures. With `RECOMP_CHAPTER_JUMP_MARK=1` and `RECOMP_FLIGHT_FRAMES=N`
  the whole intro event of the target mission is flight-recorded. Example:
  the chapter-2 intro, `--env RECOMP_CHAPTER_JUMP=2:96`.
- `replay_arm.sh`, `replay_timeline.sh`, `replay_burst.sh` -- replay a player
  `.padrec` in observe mode and capture at the end / periodically / in a
  burst. Replays drift; use them for "what does this draw do", not to reach
  a specific moment.
- `checkpoint_run.sh NAME HDD N EVERY` + `load.json` -- boot an HDD copy
  holding a checkpoint save and capture after loading slot 0.

Known flake: occasionally the harness's first input is not delivered at the
title ("Input was not delivered before its lease expired"); retry.
