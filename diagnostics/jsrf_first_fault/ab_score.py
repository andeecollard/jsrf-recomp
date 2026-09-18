#!/usr/bin/env python3
"""Score one scripted run, or summarise an arm pair, for ab_switch.sh.

Separate from the shell script on purpose: the scoring is a pure function of a
log file, so it can be re-run against any archived measure/ directory months
later. The A/B that this pair of files replaces could not be re-scored at all,
because its scoring lived in a shell loop that exited with the session.

Two modes:

    ab_score.py --tag t1_batch0 --var RECOMP_METAL_BATCH --value 0 \
                --warmup 20 <stderr.log>
        one key=value line describing that run

    ab_score.py --summarise --var RECOMP_METAL_BATCH --warmup 20 <RESULTS.txt>
        the arm comparison, over the lines the first mode wrote

WHAT IS READ, AND WHY EACH ONE IS THE LINE IT IS
------------------------------------------------
scene       [JSRF-SEQ] now=N, last occurrence. 28-35 is a mission or the
            tutorial. This is the gate: a run below it is excluded from the
            frame comparison rather than averaged in, because it was rendering
            a menu.

frame       [FRAME-WIN] mean=, which is the mean flip interval over the report
            window just ended -- NOT [FRAME], which is cumulative from boot and
            therefore still carries the title screen's 69 fps in it at the end
            of a 280 s run.

            Each window is tagged with the held= of the [JSRF-SEQ] line that
            closes the same report block, i.e. how long the title has been in
            its current sequence state. Windows are then kept only if the state
            is a mission AND held >= warmup. That is the whole reason this file
            is more than a grep: 'the last three windows' is a wall-clock
            selection, and two runs are not at the same point of the mission at
            the same wall-clock second.

crash       'FIRST GUEST FAULT', which is a real fault with a symbolised host
            PC. NOT [JSRF-FATAL], which is a periodic status line that prints
            m_bFatal=0 when everything is fine -- counting those reports nine
            faults for a clean run.

stall       Scheduled [PAD-SCRIPT] events that never fired, BETWEEN two that
            did. The input-poll stall is invisible in every average: a run that
            fired 128 of 202 events had a 76 s silence with a 19 ms mean poll
            interval on either side of it. Anything scheduled inside the gap
            did not happen, so a run with a large one cannot support a claim
            about what the input caused -- including a frame time measured
            while the player was, in fact, standing still.

            IT IS MEASURED AGAINST THE SCHEDULE, NOT AS A BARE GAP, and that
            correction is the reason this needs the pad file. play_scripted.sh
            flags any silence over 10 s, which was calibrated on gameplay.pad.
            gameplay_nobarrage.pad -- the only schedule in this tree that
            reaches a mission -- has a DESIGNED 34 s pause between the last boot
            press at t=71 and the first skate at t=105, because the barrage that
            used to fill it was walking the title into the VS menu. A bare-gap
            rule rejects every healthy run on that pad. A missed-event rule
            reads it correctly: nothing was scheduled in there to miss.

            Events after the last one that fired are not counted either. The run
            is killed at a fixed wall-clock second and the schedule usually
            outlives it; that is the timer, not the guest.

audio       [APU-FRAME] se/total, the sound engine's duty cycle, and
            [APU-VOICE] off=, voices retired. Carried on every run whatever the
            switch is, because they are how you notice that a rendering change
            moved something in the APU.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# The schedule parser lives in test_pad_schedule.py, whose own docstring says
# the point of it is to BE the parser in src/input/xinput_device.c rather than
# a reasonable approximation. Importing it keeps that one copy; a second parser
# here would be a third implementation of the same grammar.
from test_pad_schedule import parse as parse_pad

SEQ_RE = re.compile(r"\[JSRF-SEQ\] now=(\d+) \S+ held=(\d+) ticks")
WIN_RE = re.compile(r"\[FRAME-WIN\] flips=(\d+) mean=([\d.]+) ms")
FIRE_RE = re.compile(r"\[PAD-SCRIPT\] t=\s*([\d.]+) fire ")
APUF_RE = re.compile(r"\[APU-FRAME\] total=(\d+) se=(\d+) trapped=(\d+)")
VOICE_RE = re.compile(r"\[APU-VOICE\] on=(\d+) off=(\d+)")
STAGE_RE = re.compile(r"\[STAGE\] per frame:(.*)\| rest=")
STAGE_ONE = re.compile(r"(\w+)=([\d.]+) ms")
# How many times the guest's USB driver locked a transfer buffer. It separates
# a boot that failed because input never reached the title from one that failed
# for some other reason: runs that reached New Game read 12,963-74,603 here and
# runs stuck at the title read 234-6,684 (pad/gameplay_nobarrage.pad's header).
# Carried on every run so an EXCLUDED one still says something.
ORD175_RE = re.compile(r"\[KERNEL\] ordinals used \(\d+\):(.*)")
# Run length, so USB traffic can be reported as a RATE. See below for why an
# absolute count is not merely less useful but actively misleading.
VBLANK_MS_RE = re.compile(r"\[VBLANK\].* over (\d+) ms")
IDLE_RE = re.compile(r"\[APU-VOICE\].* idle_trap=(\d+)")
SUPP_RE = re.compile(r"\[APU-TRAP\] suppressed=(\d+)")
TERM_RE = re.compile(r"\[APU-SELFLINK\] terminated=(\d+)")
# The storm's own signature: how much of the idle-trap traffic is one voice.
BUSIEST_RE = re.compile(r"\[APU-IDLE\] (\d+) idle-voice traps raised over "
                        r"\d+ distinct voices; busiest: v\d+=(\d+)")
# DID THE ARM ACTUALLY TAKE? Several reports name the state of the switch they
# describe -- "(guard on)", "(coalesce on, se_while_trapped OFF)". Harvesting
# them turns "I set the variable" into "the model read it", which is not the
# same claim: RECOMP_APU_SELFLINK_END tested for the variable's PRESENCE until
# 15 Sep 2026, so an A/B whose control arm passed =0 ran with the guard on in
# both arms and reported "(guard on)" in both. Nobody read the line. The
# summary compares these between arms and refuses an A/B where they match.
SWITCH_RE = re.compile(r"\[APU-(?:TRAP|SELFLINK|REON)\][^(]*\(([^)]*)\)")
# The renderer reports its own state without parentheses, e.g.
#   [VSH-REUSE] reuse=on verify=on hits=... mismatches=1
# Harvested separately so a renderer A/B can verify its arms differ too,
# rather than falling back to "assumes the environment took".
SWITCH_BARE_RE = re.compile(r"\[VSH-REUSE\] \((vsh_reuse \w+)")
# The Metal backend names its own state in parentheses, like the APU reports.
#
# GENERIC ON PURPOSE, 17 Sep 2026, and the specific version is why. It read
#
#     r"\[METAL\][^(]*\((metal_(?:hw|565|batch) \w+)\)"
#
# which could not see `defer_swap` for two independent reasons: the token list
# was hardcoded to three names, and `[^(]*` stops at the FIRST parenthesis, so
# on
#
#     [METAL] swap writeback deferred: 1 (refused: 5747 depth dirty, 0 no
#     slot) (defer_swap on)
#
# it matched "refused: ..." and failed the alternation. ab_score.py's own
# comment beside RECOMP_METAL_DEFER_SWAP asserted the VOID check could run.
# It could not, and the defer_swap A/B was scored with the identical-arms
# check silently skipped -- the same failure that was fixed for RECOMP_SYNC_HIST
# earlier the same day and not generalised.
#
# Now: every `(token on)` or `(token OFF)` group anywhere on a [METAL] line.
# `(refused: 5747 depth dirty, 0 no slot)` does not match, because a refusal
# count is not a word followed by on/OFF.
METAL_SWITCH_RE = re.compile(r"\((\w+ (?:on|OFF))\)")

# WHICH TOKEN IN THAT HARVESTED STATE BELONGS TO WHICH SWITCH.
#
# Without this the VOID rule below was worse than useless. [APU-TRAP] prints
# "(coalesce on, se_while_trapped on)" on EVERY report of EVERY run whatever is
# under test, so two arms of any A/B that is not about those two switches
# harvest identical strings and the rule declares the A/B void. It would have
# voided the RECOMP_APU_REON_HEAD_NOP A/B running when this was found, and it
# would void every renderer A/B ever taken -- RECOMP_METAL_BATCH included.
#
# So the rule now asks a narrower question it can actually answer: does the
# report token for THIS switch differ between the arms? A switch with no entry
# here cannot be checked, and the summary says so rather than guessing.
SWITCH_TOKEN = {
    "RECOMP_APU_SELFLINK_END":     "guard",
    "RECOMP_APU_TRAP_COALESCE":    "coalesce",
    "RECOMP_APU_SE_WHILE_TRAPPED": "se_while_trapped",
    "RECOMP_APU_REON_HEAD_NOP":    "reon_head_nop",
    "RECOMP_VSH_REUSE":            "vsh_reuse",
    "RECOMP_METAL_HW":             "metal_hw",
    "RECOMP_METAL_565":            "metal_565",
    # The switch that decides whether a real frame is correct, and the one this
    # table was missing while it was being A/B'd on frame time alone.
    "RECOMP_METAL_BATCH":          "metal_batch",
    # Added 16 Sep 2026. Each of these was A/B'd that day; without an entry
    # here the harvest works and the VOID check is silently SKIPPED, which is
    # the state every one of them was scored in.
    "RECOMP_APU_IDLE_TRAP_EDGE":   "idle_edge",
    "RECOMP_METAL_FF":             "metal_ff",
    "RECOMP_VSH_DP_ZERO":          "vsh_dp_zero",
    "RECOMP_METAL_VSH":            "metal_vsh",
    "RECOMP_APU_ADPCM_GUARD":      "adpcm_guard",
    "RECOMP_APU_LIST_MOVE_TO_FRONT": "move_to_front",
    # Added 17 Sep 2026, along with the unconditional token that makes it
    # work: nv2a_pb_exec.c prints "sync_hist on|OFF" on every report whether
    # or not the histogram has samples, so the off arm has something to match.
    "RECOMP_SYNC_HIST":            "sync_hist",
    # Added 17 Sep 2026. Rides in the [APU-TRAP] parentheses, which is what
    # the harvester reads; a line of its own would not be seen.
    "RECOMP_APU_IDLE_TRAP_SELFLINK": "idle_selflink",
    # G3 A2. Token printed unconditionally on the [METAL] swap-deferral line,
    # so the off arm has something to match and the VOID check can run.
    "RECOMP_METAL_DEFER_SWAP":     "defer_swap",
    # G3's depth question. Printed unconditionally on the [METAL] depth
    # write-back line, and the generic METAL_SWITCH_RE added 17 Sep can
    # actually see it -- unlike defer_swap, which the old hardcoded regex
    # could not, so its A/B ran with the VOID check skipped.
    "RECOMP_METAL_NO_DEPTH_SYNC":  "no_depth_sync",
    # G3's colour question, 18 Sep -- the same question no_depth_sync asked of
    # depth and won 9.1% with. Printed unconditionally on the [METAL] colour
    # write-back line, so the OFF arm has a token to differ from; the token is
    # `no_colour_sync`, which is not a substring of `no_depth_sync`, so
    # switch_state_for's `token in clause` cannot confuse the two.
    "RECOMP_METAL_NO_COLOUR_SYNC": "no_colour_sync",
}


def switch_state_for(var, states):
    """The part of the harvested state that speaks to `var`, or None."""
    token = SWITCH_TOKEN.get(var)
    if not token:
        return None
    for state in states:
        for field in state.split(";"):
            for clause in field.split(","):
                if token in clause:
                    return clause.strip()
    return None

# A mission action object exists. Covers the mission's loading screen, its
# opening cutscene and its pause menu as well as skating, which is why warmup
# exists; it does not cover any menu.
MISSION = range(28, 36)


# An event fires at the first pad poll at or after its scheduled time, so the
# t= printed on its fire line runs LATE by up to one poll interval -- measured
# at +0.01 s on a healthy run. Matching scheduled times to fired times exactly
# therefore reports every such event as missed; the first version of this file
# did, and called 30 of 170 events missing in a run that fired them all.
#
# 0.25 s, because the tightest spacing any schedule here uses is the 0.35 s
# doubled jump (112.00 / 112.35) and the tolerance must not reach across it.
FIRE_TOL = 0.25


def missed_events(fired, pad_path):
    """(seconds, count) of the worst run of scheduled events that never fired.

    Only gaps bracketed by two events that DID fire count: a schedule that
    outlives the run's time limit is the timer expiring, not the guest going
    deaf, and the designed pauses inside a schedule contain nothing to miss.
    """
    if not pad_path or not fired:
        return 0.0, 0
    with open(pad_path, "r", errors="replace") as fh:
        events, _ = parse_pad(fh.read())
    sched = sorted({round(e[0], 2) for e in events})
    order = sorted(fired)

    # Greedy in time order: each fired line answers for at most one scheduled
    # time, so a burst of fires cannot cover for a neighbour that never came.
    unfired, i = [], 0
    for s in sched:
        while i < len(order) and order[i] < s - FIRE_TOL:
            i += 1
        if i < len(order) and order[i] <= s + FIRE_TOL:
            i += 1
        else:
            unfired.append(s)

    worst_s, worst_n = 0.0, 0
    for a, b in zip(order, order[1:]):
        gone = [s for s in unfired if a < s < b]
        if gone and b - a > worst_s:
            worst_s, worst_n = b - a, len(gone)
    return worst_s, worst_n


def score(path, warmup, pad_path=None):
    """Return a dict of the numbers above for one run's stderr log."""
    scene = None
    held = 0.0
    crashes = 0
    pending = None        # the [FRAME-WIN] of the report block being read
    pending_stage = {}    # the [STAGE] breakdown of the same block
    windows = []          # one dict per closed report block
    fires = []
    apu = None
    voice = None
    ord175 = None
    run_ms = None
    switches = set()
    apu_counters = {}

    with open(path, "r", errors="replace") as fh:
        for line in fh:
            m = WIN_RE.search(line)
            if m:
                # A block can only hold one window; if two arrive before a SEQ
                # line (SEQ_REPORT off, or the sampler failed) the earlier one
                # is unattributable and dropped rather than mis-tagged.
                pending = (int(m.group(1)), float(m.group(2)))
                pending_stage = {}
                continue
            m = STAGE_RE.search(line)
            if m:
                pending_stage = {k: float(v)
                                 for k, v in STAGE_ONE.findall(m.group(1))}
                continue
            m = SEQ_RE.search(line)
            if m:
                scene = int(m.group(1))
                held = int(m.group(2)) / 100.0
                if pending is not None:
                    windows.append({"state": scene, "held": held,
                                    "mean": pending[1], "flips": pending[0],
                                    "stage": pending_stage})
                    pending, pending_stage = None, {}
                continue
            m = ORD175_RE.search(line)
            if m:
                for field in m.group(1).split():
                    if field.startswith("175="):
                        ord175 = int(field[4:])
                continue
            m = VBLANK_MS_RE.search(line)
            if m:
                run_ms = int(m.group(1))
            m = SWITCH_RE.search(line)
            if m:
                switches.add(m.group(1).strip())
            m = SWITCH_BARE_RE.search(line)
            if m:
                switches.add(m.group(1).strip())
            if line.startswith("  [METAL]") or line.lstrip().startswith("[METAL]"):
                # findall, not search: one [METAL] line can carry more than one
                # switch, and search would keep only the first.
                for tok in METAL_SWITCH_RE.findall(line):
                    switches.add(tok.strip())
            m = IDLE_RE.search(line)
            if m:
                apu_counters["idle_trap"] = int(m.group(1))
                continue
            m = SUPP_RE.search(line)
            if m:
                apu_counters["suppressed"] = int(m.group(1))
                continue
            m = TERM_RE.search(line)
            if m:
                apu_counters["terminated"] = int(m.group(1))
                continue
            m = BUSIEST_RE.search(line)
            if m:
                apu_counters["traps"] = int(m.group(1))
                apu_counters["busiest"] = int(m.group(2))
                continue
            m = FIRE_RE.search(line)
            if m:
                fires.append(float(m.group(1)))
                continue
            m = APUF_RE.search(line)
            if m:
                apu = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
                continue
            m = VOICE_RE.search(line)
            if m:
                voice = (int(m.group(1)), int(m.group(2)))
                continue
            if "FIRST GUEST FAULT" in line:
                crashes += 1

    gap, gap_n = missed_events(fires, pad_path)

    kept = [w for w in windows
            if w["state"] in MISSION and w["held"] >= warmup]
    # Flip-weighted, because a 30 s window holding 1005 flips and one holding
    # 624 are not equal evidence about the mean flip interval.
    flips = sum(w["flips"] for w in kept)
    mean = sum(w["mean"] * w["flips"] for w in kept) / flips if flips else None

    # Where the frame went, over the same windows. For a switch that changes
    # how draws are submitted, the submit column is the one that should move,
    # and a win that shows up somewhere else wants explaining before it is
    # believed.
    stage = {}
    if flips:
        names = set()
        for w in kept:
            names |= set(w["stage"])
        for k in names:
            num = sum(w["stage"].get(k, 0.0) * w["flips"] for w in kept)
            stage[k] = num / flips

    return {
        "scene": scene,
        "held": held,
        "mission": scene in MISSION if scene is not None else False,
        "windows": windows,
        "kept": kept,
        "mean_ms": mean,
        "stage": stage,
        "flips": flips,
        "crash": crashes,
        "gap": gap,
        "gap_n": gap_n,
        "fired": len(fires),
        "apu": apu,
        "voice": voice,
        "ord175": ord175,
        "run_ms": run_ms,
        "switches": sorted(switches),
        "counters": apu_counters,
    }


def run_line(tag, var, value, path, warmup, pad_path=None):
    s = score(path, warmup, pad_path)
    bits = ["%-16s" % tag, "%s=%s" % (var, value)]
    if s["scene"] is None:
        bits.append("scene=NONE")
    else:
        bits.append("scene=%d%s" % (s["scene"], "" if s["mission"] else "!"))
    bits.append("held=%.0fs" % s["held"])
    if s["mean_ms"] is None:
        bits.append("frame=NONE")
    else:
        bits.append("frame=%.2fms" % s["mean_ms"])
    bits.append("win=%d" % len(s["kept"]))
    bits.append("flips=%d" % s["flips"])
    bits.append("crash=%d" % s["crash"])
    bits.append("missed=%.0fs/%d" % (s["gap"], s["gap_n"]))
    if s["ord175"] is not None:
        # PER SECOND, not the raw count.
        #
        # The raw count reads as a USB stall on any run that ended early, and
        # that misled this file's own author: three crashed runs showed
        # ord175 of 4709/3578 against a healthy 32547 and were written up as
        # "the USB driver had stalled". Normalised they are 117.7, 119.4 and
        # 120.5 transfers per second -- identical. They were 30-40 s runs that
        # crashed, not stalls.
        #
        # A real stall looks nothing like that: pad/gameplay_nobarrage.pad
        # records 234 transfers in a 300 s run, which is 0.78/s.
        if s["run_ms"]:
            bits.append("usb=%.0f/s" % (s["ord175"] * 1000.0 / s["run_ms"]))
        else:
            bits.append("ord175=%d(no duration)" % s["ord175"])
    if s["apu"]:
        total, se, trapped = s["apu"]
        bits.append("duty=%.1f%%" % (100.0 * se / total if total else 0.0))
        bits.append("trapped=%d" % trapped)
    if s["voice"]:
        bits.append("off=%d" % s["voice"][1])
    c = s["counters"]
    for k in ("idle_trap", "suppressed", "terminated"):
        if k in c:
            bits.append("%s=%d" % (k, c[k]))
    if c.get("traps"):
        bits.append("busiest=%.0f%%" % (100.0 * c["busiest"] / c["traps"]))
    if s["switches"]:
        bits.append("switch{%s}" % "; ".join(s["switches"]))
    if s["stage"]:
        bits.append("stage[" + " ".join(
            "%s=%.2f" % (k, s["stage"][k]) for k in sorted(s["stage"])) + "]")
    # The per-window detail, so the summary can be argued with rather than
    # merely believed.
    if s["kept"]:
        bits.append("| " + " ".join("%.0fs:%.1f" % (w["held"], w["mean"])
                                    for w in s["kept"]))
    return " ".join(bits)


LINE_RE = re.compile(
    r"^(\S+)\s+(\S+)=(\d+)\s+scene=(\S+)\s+held=\S+\s+frame=(\S+)\s+"
    r"win=(\d+)\s+flips=(\d+)\s+crash=(\d+)\s+missed=([\d.]+)s/(\d+)")


SWITCH_FIELD_RE = re.compile(r"switch\{([^}]*)\}")


def summarise(path, var, warmup):
    arms = {"0": [], "1": []}
    states = {"0": set(), "1": set()}
    excluded = []
    for raw in open(path):
        m = LINE_RE.match(raw.strip())
        if not m or m.group(2) != var:
            continue
        sm = SWITCH_FIELD_RE.search(raw)
        if sm:
            states[m.group(3)].add(sm.group(1))
        tag, value, scene, frame = m.group(1), m.group(3), m.group(4), m.group(5)
        crash, gap, gap_n = int(m.group(8)), float(m.group(9)), int(m.group(10))
        bad = []
        if scene.endswith("!") or scene == "NONE":
            bad.append("never reached a mission (scene=%s)" % scene.rstrip("!"))
        if frame == "NONE":
            bad.append("no frame window inside the mission past warmup")
        # Any missed event at all is a stall: the schedule is deterministic and
        # a healthy run fires every event up to the moment it is killed. The
        # threshold is on the duration only so a single dropped press does not
        # void a run that was otherwise poll-alive throughout.
        if gap >= 10:
            bad.append("input-poll stall: %d scheduled event(s) never fired,"
                       " over %.0fs" % (gap_n, gap))
        if bad:
            excluded.append((tag, "; ".join(bad)))
        else:
            arms[value].append((tag, float(frame.rstrip("ms")), crash))

    out = []
    for value in ("0", "1"):
        runs = arms[value]
        if not runs:
            out.append("  %s=%s   no usable run" % (var, value))
            continue
        means = [r[1] for r in runs]
        out.append("  %s=%s   frame %s ms   (mean %.2f, n=%d)   crashes %d"
                   % (var, value, " ".join("%.2f" % m for m in means),
                      sum(means) / len(means), len(means),
                      sum(r[2] for r in runs)))
    if excluded:
        out.append("  excluded, and NOT counted as either arm:")
        for tag, why in excluded:
            out.append("    %-16s %s" % (tag, why))

    # AND IS THERE ENOUGH OF EACH ARM TO COMPARE AT ALL?
    #
    # One run per arm cannot see the effect sizes this project reports. On
    # 16 Sep 2026 a single pair read 59.4 fps against 62.4 and was very nearly
    # written up as a 5% win; the next pair's OFF arm read 63.2, i.e. the
    # "effect" was smaller than the spread between two runs of the SAME arm.
    # Run-to-run spread on this host is several per cent, so a one-run arm is
    # a lean, and the rules this tree already states call that not a
    # measurement.
    #
    # Refused rather than warned, because a warning above a table of numbers
    # is read as a caveat and the numbers are read as a result.
    n0, n1 = len(arms["0"]), len(arms["1"])
    if min(n0, n1) < 2 and (n0 or n1):
        out.append("")
        out.append("  VOID: ONE RUN PER ARM IS NOT A MEASUREMENT (n=%d vs %d)."
                   % (n0, n1))
        out.append("  Run-to-run spread on this host is several per cent, so a"
                   " single pair cannot separate an effect of that size from"
                   " noise. Take at least two usable runs per arm -- and note"
                   " that runs excluded above do not count toward either.")
        return "\n".join(out)

    # Before any comparison: did the arms actually differ? Only the token that
    # belongs to THIS switch can answer that -- see SWITCH_TOKEN above for why
    # comparing the whole harvested string voided everything.
    s0 = switch_state_for(var, states["0"])
    s1 = switch_state_for(var, states["1"])
    if s0 is not None and s1 is not None:
        if s0 == s1:
            out.append("")
            out.append("  VOID: BOTH ARMS RAN THE SAME CONFIGURATION.")
            out.append("  Every run reported: %s" % s0)
            out.append("  The model did not read %s the way this A/B set it."
                       " Check that the switch tests the variable's VALUE and"
                       " not merely its presence -- `getenv(X) != NULL` makes"
                       " X=0 enable the thing it was meant to disable." % var)
            return "\n".join(out)
        out.append("  arms verified distinct: %s=0 reported \"%s\", =1 reported"
                   " \"%s\"" % (var, s0, s1))
    elif states["0"] or states["1"]:
        # Not a failure: most switches do not name themselves in any report.
        # Saying so is better than a check that silently passes.
        out.append("  arms NOT verified distinct: no report names %s. The"
                   " comparison below assumes the environment took." % var)

    a, b = arms["0"], arms["1"]
    if a and b:
        ma = sum(r[1] for r in a) / len(a)
        mb = sum(r[1] for r in b) / len(b)
        # Ranges, not a t-test. n=2 per arm supports "these do not overlap" and
        # does not support a p-value, and writing one down would lend the
        # result a confidence the sample size cannot carry.
        lo_a, hi_a = min(r[1] for r in a), max(r[1] for r in a)
        lo_b, hi_b = min(r[1] for r in b), max(r[1] for r in b)
        n = min(len(a), len(b))
        out.append("")
        out.append("  %s=0  %.2f - %.2f ms  (n=%d)" % (var, lo_a, hi_a, len(a)))
        out.append("  %s=1  %.2f - %.2f ms  (n=%d)" % (var, lo_b, hi_b, len(b)))
        if n < 2:
            # A single run per arm has a range of zero width, so ANY difference
            # "does not overlap". The first version of this file printed exactly
            # that for a 1-v-1 result, which is a statement about arithmetic and
            # not about the switch. Run-to-run spread is the quantity being
            # compared against and one run does not have any.
            out.append("  NO VERDICT: %d usable run(s) in the smaller arm. A"
                       " range needs at least two." % n)
            out.append("  The difference of means is %.2f ms (%.1f%%), and it is"
                       " a difference between two individual runs, not between"
                       " the arms." % (abs(ma - mb), 100.0 * abs(ma - mb)
                                       / max(ma, mb)))
            out.append("  Re-run and pool the RESULTS.txt files:  cat */RESULTS.txt"
                       " > all.txt && ab_score.py --summarise --var %s all.txt"
                       % var)
        elif hi_b < lo_a:
            out.append("  ON is faster and the ranges DO NOT OVERLAP"
                       " (%.1f%% less frame time at n=%d per arm)."
                       % (100.0 * (ma - mb) / ma, n))
        elif hi_a < lo_b:
            out.append("  OFF is faster and the ranges DO NOT OVERLAP"
                       " (%.1f%% less frame time at n=%d per arm)."
                       % (100.0 * (mb - ma) / mb, n))
        else:
            out.append("  THE RANGES OVERLAP. This A/B does not separate the"
                       " arms; the difference of means (%.2f ms) is inside the"
                       " run-to-run spread." % abs(ma - mb))
    else:
        out.append("")
        out.append("  NO COMPARISON: an arm has no usable run. Re-run it.")

    out.append("")
    out.append("  Boot safety is NOT measured here. %d run(s) reached a mission;"
               % (len(a) + len(b)))
    out.append("  a hang seen once interactively is not excluded by that.")
    return "\n".join(out)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("path")
    p.add_argument("--tag")
    p.add_argument("--var", required=True)
    p.add_argument("--value")
    p.add_argument("--warmup", type=float, default=20.0)
    p.add_argument("--pad", help="the schedule the run was given; without it"
                                 " no input-poll stall can be detected")
    p.add_argument("--summarise", action="store_true")
    a = p.parse_args()
    if a.summarise:
        print(summarise(a.path, a.var, a.warmup))
    else:
        print(run_line(a.tag, a.var, a.value, a.path, a.warmup, a.pad))
    return 0


if __name__ == "__main__":
    sys.exit(main())
