# Eleven voices stop one by one — and the table can only ever show 12 of 256

19 September 2026. No run, no build — item 3 of the 19 Sep handover
("THEN read voices 4-11 across a freeze, not 64-67"), read out of
`last-run-2026-09-19_BLACKSCREEN-FULL-GUEST-STOP.log`, which has 2,171
`[VOICE-RATE]` lines across 62 reports.

The handover ordered this **after** a healthy control run. It does not need
one: the contrast is temporal and inside this single run.

---

## 1. The staggered shutdown

`frames` is the count of frames on which a voice was processed
(`voice_rate_note`, `apu_vp.c:3135`, called once per voice per processed
frame at `apu_vp.c:4067`). The last report at which each voice's `frames`
still moved:

| voice | last change | voice | last change |
|---|---|---|---|
| 10 | report 14 | 8 | report 27 |
| 9 | report 19 | 2 | report 30 |
| 1 | report 22 | 7 | report 30 |
| 5 | report 24 | **3** | **report 61 (the last)** |
| 0, 4, 6, 11 | never moved while listed | | |

**They stop one at a time over ~16 reports, and not one of them ever
resumes.** Voice 3 alone carries on, at a flat ~6,950 frames and ~29,000
energy per report, to the end of the run.

`voice-frames` in total climbs the whole time — 1,332,856 → 2,133,121 over
those same reports — so **the mixer is running**. It has simply stopped
fetching eleven of the twelve voices it can see.

`silent_frames` does not move either. This is not "active and rendering
silence": the voices are **not being processed at all**, while remaining
resident in the table.

That is a fourth shape, and G1's three did not include it:

> the guest stops *starting* 2D voices; 2D voices are started but never become
> ACTIVE; 2D voices are ACTIVE and render silence.

## 2. …but the instrument is blind to 94% of the pool

`mcpx_apu_voice_rate_report()` (`apu_vp.c:3947`):

```c
for (v = 0; v < MCPX_HW_MAX_VOICES && shown < 12; v++) {
    const VoiceRate *r = &g_voice_rate[v];
    if (!r->frames) continue;
```

- `MCPX_HW_MAX_VOICES` is **256** (`apu_regs.h:330`).
- The table prints at most **12**, scanning upward from voice 0.
- `g_voice_rate` is a file-scope array that is **never reset** — no memset, no
  per-report clear. `frames`, `energy` and `silent_frames` are cumulative for
  the life of the process. (Only the `w_*` window fields are cleared.)

Those three facts together are the problem. Once twelve low-numbered voices
have each been processed even once, `frames` is nonzero for all twelve
**forever**, they fill the twelve slots **forever**, and **no voice above them
can ever be printed again for the rest of the run.**

That run reports `on=160`. One hundred and sixty voices were started; twelve
could be displayed.

## 3. What this does to the readings already in the handovers

- **"Voices 64–67 disappear / are retired" is an artifact.** They are printed
  early only because fewer than twelve low voices had been touched yet. They
  vanish when voices 0–11 fill the budget — not when they stop.
- **Section 4 of the 19 Sep handover reads 64–67 through this filter**, and
  the reason it "has no healthy capture to compare against" matters less than
  the fact that the rows it does have are selected by scan order, not by
  relevance.
- **"The music died" is NOT established by this table.** Eleven voices
  stopping is real. But the music may have moved to any of the other 244
  voices, and the table became structurally incapable of showing them. A
  frozen row means that voice stopped; the absence of a row means nothing at
  all.

The house rule this is an instance of: *every absence-measurement needs a
positive control.* Here the absence is a **missing table row**, and its cause
is the display cap.

## 4. The fix needs a rebuild, and that is a real conflict

The 12 is a literal in the loop condition. There is no environment override,
so this **cannot** be fixed from `paths.conf` — unlike everything else the
19 Sep handover queues up, which is why it is worth deciding deliberately.

The same pattern appears a second time at `apu_vp.c:1749` with `shown < 8`.

The handover is emphatic that the player bundle must not be rebuilt, because
the three captured failures are all on one binary and that is what makes them
comparable. Both positions are right, and they collide. The cheapest way out:

1. Take the healthy control run **first**, on the unchanged bundle, as
   planned. It costs nothing and preserves comparability.
2. Then rebuild once with the cap raised — ideally as
   `RECOMP_VOICE_RATES_ROWS`, defaulting to 12 so old logs stay comparable —
   and re-measure.

**Positive control for the fix:** with the cap raised, a late report must show
more than twelve rows in a run that reports `on=160`. If it still shows
twelve, the cap was not the only filter.

## 5. What is solid, and what is not

**Solid.** Eleven of the twelve displayable voices stop being processed, at
staggered times between reports 14 and 30, and never resume. Voice 3 runs to
the end. The mixer keeps running throughout.

**Not established.** That this is "the music stopping". That the eleven are
the 2D bin. That anything at all happened to voices 12–255 — the instrument
cannot see them, and did not see them.
