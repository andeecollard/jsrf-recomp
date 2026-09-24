#!/usr/bin/env python3
"""The switches must obey rules that no human re-checks.

This tree has 204 RECOMP_* switches and its recorded failures are mostly
"the two arms were not actually different" or "the default was not what the
comment said". Those are properties of the SWITCH SET, not of any one file,
so nothing catches them at review time. This does, as a test.

Rules, each of which has already been violated at least once:

  1. ONE GRAMMAR PER SWITCH. recomp_switch.h's helper treats any non-empty
     value that is not "0" as on, so RECOMP_X=false reads ON. The hand-rolled
     `atoi(e) != 0` form reads "on" and "yes" as OFF. A switch read through
     both answers differently depending on which file asked.

  2. A DEFAULT-ON SWITCH MUST TREAT AN EMPTY VALUE AS THE DEFAULT. `VAR= cmd`
     is how a shell unsets a variable for one command, and a harness that did
     that to a default-on switch silently turned the fix OFF -- which is
     exactly how an empty RECOMP_VSH_DP_ZERO would have disabled the fence fix
     while the log still read like a normal run.

  3. EVERY SWITCH A CONFIG EXPORTS MUST BE READ SOMEWHERE. A typo in
     paths.conf is otherwise a silent no-op: the run looks healthy, the arm
     is not the arm you asked for, and nothing says so.

Reported, not enforced, where a rule needs judgement: the point is that the
list is short and reviewed, not that the build breaks.
"""
import os, re, sys

root = sys.argv[1] if len(sys.argv) > 1 else "."
src = os.path.join(root, "src")

sources = []
for dirpath, _, names in os.walk(src):
    for n in names:
        if n.endswith((".c", ".m", ".h")):
            sources.append(os.path.join(dirpath, n))

helper, handrolled, default_on, empty_safe = {}, {}, set(), set()
for path in sources:
    text = open(path, errors="ignore").read()
    # Both helpers count as "via the helper": recomp_switch_on_default states
    # the SAME grammar for a switch that defaults on, which is the property
    # this ratchet is protecting. Matching it explicitly rather than letting
    # the recomp_switch_on pattern catch it by prefix, so a future helper with
    # a different grammar is not silently waved through.
    for pat in (r'recomp_switch_on\(\s*"(RECOMP_[A-Z0-9_]+)"',
                r'recomp_switch_on_default\(\s*"(RECOMP_[A-Z0-9_]+)"'):
        for name in re.findall(pat, text):
            helper.setdefault(name, set()).add(path)
    for name in re.findall(r'getenv\(\s*"(RECOMP_[A-Z0-9_]+)"', text):
        handrolled.setdefault(name, set()).add(path)
    # A default-on hand-rolled read looks like
    #     const char *e = getenv("RECOMP_X");
    #     on = e ? (atoi(e) != 0) : 1;
    # so the ": 1" is in a LATER statement, not before the first semicolon.
    # Looking only as far as that semicolon found zero of them -- a rule that
    # passes by checking nothing, which is the exact instrument failure this
    # tree keeps retiring. Take a window after the match instead.
    for m in re.finditer(r'getenv\(\s*"(RECOMP_[A-Z0-9_]+)"\s*\)', text):
        name = m.group(1)
        tail = text[m.end():m.end() + 240]
        tail = tail.split("\n\n")[0]
        if re.search(r'\?[^;]*:\s*1\b', tail) or re.search(r':\s*1\s*;', tail):
            default_on.add(name)
        head = text[max(0, m.start() - 240):m.end()]
        # Name-agnostic: the guard is written `v && *v` as often as `e && *e`,
        # and assuming the variable is called `e` produced a false positive on
        # bridge_sync_exec_disabled, which tests for empty correctly.
        if re.search(r'&&\s*\*\w+|\w+\[0\]\s*(?:!=|==)', tail + head):
            empty_safe.add(name)

problems = 0

# RULE 4, A RATCHET. Most of the switches parse their own value, and rewriting
# all of them in one change would be a large diff over code whose defaults
# decide whether the picture is correct -- exactly the kind of sweep that flips
# one by accident. So the set is FROZEN instead: migrate downward at leisure,
# but a NEW switch must use the helper, whose grammar is stated in one place.
# The number only ever goes down. It was 128 until 19 Sep 2026, when promoting
# RECOMP_APU_SELFLINK_END, RECOMP_APU_LIST_MOVE_TO_FRONT and
# RECOMP_APU_IDLE_TRAP_EDGE to default-on moved their three hand-rolled reads
# onto recomp_switch_on_default.
HANDROLLED_BASELINE = 125

# VALUE-CARRYING, THEREFORE NOT SUBJECT TO THE RATCHET.
#
# The ratchet's instruction is "use recomp_switch_on()", and its reason is that
# the hand-rolled form disagrees with the shared grammar on "on", "yes" and
# "false". Both are about BOOLEANS. recomp_switch_on() returns int; it cannot
# express a filesystem path, a rectangle or a count, so for a variable that
# carries a value the instruction is impossible to follow and the ratchet is
# asking for something that does not exist.
#
# Exempted BY NAME, not by pattern. A pattern that tried to infer "this one
# carries a value" would quietly exempt a boolean somebody wrote carelessly,
# which is the ratchet's whole purpose defeated. Adding a name here is a
# deliberate act with a reason attached.
#
# Value-carrying switches that predate this list are still inside the frozen
# 128 and are not enumerated here; migrating them out is a separate pass that
# would lower the baseline. This list is for NEW ones.
VALUE_CARRYING = {
    # Milliseconds and a port number, not booleans: the XInput lift's pad
    # enumeration delay and which port the host pad occupies (G49).
    "RECOMP_XINPUT_ENUM_MS",
    "RECOMP_XINPUT_PORT",
    # A list of entry-point names, not a boolean: lifted functions whose
    # original body runs anyway (G49 bisect tool, stage_dsound_census.py).
    "RECOMP_LIFT_SKIP",
    # Milliseconds, not a boolean: how far ahead of consumption the DSOUND
    # lift reports a stream buffer's cursor (G46 intro garble, dsound_lift.c).
    "RECOMP_DSOUND_STREAM_LEAD_MS",
    # A path, not a boolean: where the DSOUND lift tees its mixed output as a
    # WAV (G48 §2a, src/apu/dsound_host_out.c).
    "RECOMP_DSOUND_LIFT_WAV",
    # Names what to tally, not whether: "ring" counts guest stores into the
    # live D3D push-buffer ring by function (G33, recomp_mem_watch.c). Other
    # values are reserved for other regions, so only the literal "ring" arms it.
    "RECOMP_MEM_WATCH_TALLY",
    # off / on / "audit": audit proves each draw's alpha floor but keeps it
    # late and has the shader count discards the proof said were impossible
    # (G38c, nv2a_metal.m). A boolean cannot name the third arm.
    "RECOMP_METAL_EARLY_Z_EXACT",
    # How many consecutive reports of "all-zero presented frame with a scene
    # still being submitted" arm the per-draw capture. A count, not a
    # boolean: the boot's own black stretches armed it at 2 (t=18, on the
    # intro) and the Load screen needed 6. Its sibling
    # RECOMP_FRAG_FORCE_ON_BLACK carries the same count for the same reason.
    "RECOMP_FB_DUMP_DRAW_ON_BLACK",
    # Megabytes of free space to claim on the emulated data partition, in
    # place of measuring it. Forcing it low makes JSRF put up its save-check
    # dialog at boot -- which is the cheapest reproduction of the glyph
    # defect this project has: text-heavy, no player, no gameplay, a handful
    # of draws behind it. 0 is a meaningful value, so presence is the test
    # and recomp_switch_on() cannot express it.
    "RECOMP_HDD_DATA_FREE_MB",
    # A dump path for the indirect-branch feedback database. Was a
    # compile-time constant relative to the working directory, which a
    # GUI-launched .app cannot write -- 91 silent fopen failures per session.
    "RECOMP_ICALL_FEEDBACK_PATH",
    # A parts-per-million threshold deciding when the frame outside the watch
    # rectangle counts as "still", so the glyph trap can fire on "changed while
    # the scene was static" instead of "changed a little" -- the latter fires
    # ~100 times a minute and is ordinary animation.
    "RECOMP_FB_WATCH_STILL_PPM",
    # How many per-voice rows [VOICE-RATE] prints, of the 256-voice pool. The
    # cap was a hardcoded 12 and the rows are the LOWEST-numbered voices ever
    # processed, chosen against a counter that is never reset -- so after
    # twelve low voices have run once they hold every slot for the rest of the
    # run. A 19 Sep 2026 session reporting on=160 printed voices 0-11 from the
    # first report to the last, and "voices 64-67 were retired" was read off
    # rows that had merely been crowded out.
    "RECOMP_VOICE_RATES_ROWS",
    # How many of the Metal staging ring's eight slabs rotate, and how much of
    # a slab the bump pointer may hand out. Both are stress knobs for one
    # question -- can the ring come round onto storage the GPU is still
    # reading? -- and both have to carry a number: the shipping ring is 8 x
    # 8 MB and comes round twice a second, which is far too slow to reach the
    # hazard, so a run that wants to reach it has to say by how much to
    # squeeze. Booleans could not express either. (The third switch of the
    # set, RECOMP_METAL_RING_NOPIN, is a plain boolean and goes through
    # recomp_switch_on.)
    "RECOMP_METAL_RING_SLABS",
    "RECOMP_METAL_RING_SLAB_KB",
    # Where a pad recording is written. It has to be a path and it has to
    # default to an absolute one: a GUI-launched .app has no writable working
    # directory, and the player's own session is precisely the run worth
    # recording. The boolean that turns recording on, RECOMP_PAD_RECORD, goes
    # through the helper like anything else.
    "RECOMP_PAD_RECORD_PATH",
    # The guest-memory pattern search. Both carry values by necessity: one is
    # the byte pattern to look for (text with \xNN escapes, ';'-separated) and
    # the other a delay in seconds before the first scan. Neither can be a
    # boolean -- a search with no pattern is not a search. The instrument is
    # read-only and off unless RECOMP_RAM_FIND is set.
    "RECOMP_RAM_FIND",
    "RECOMP_RAM_FIND_AFTER",
    # Where the XDK symbol table is, and which address in it to believe. The
    # push-buffer pump stopped being per-title when it learned to find
    # D3D8__D3D_g_pDevice by name instead of carrying 0x0019DCE0 by hand, and
    # a path and an address are the two things that lookup needs. A boolean
    # cannot express either, and the override exists so a title whose scan
    # misfires can still be pumped.
    "RECOMP_XDK_SYMBOLS",
    "RECOMP_D3D8_DEVICE_GLOBAL",
    # Now carries the trace's LINE CAP as well as turning it on. The cap was a
    # hardcoded 4,000, which a boot spends in its first seconds -- 400,000 quad
    # lines were gone by t=32 s of one run, all of it menu -- while the banner
    # the instrument was written for arrives minutes into a replay. Presence
    # still means on, so recomp_switch_on() still decides that; the number is
    # read separately.
    "RECOMP_FONT_TRACE",
    # The glyph dump's five knobs. RECOMP_GLYPH_DUMP is a CAP -- how many
    # glyph draws to write out -- not a boolean, because the defect is text
    # that renders wrong and a run that dumps every glyph in a menu fills a
    # disk before the interesting one arrives. The other four say when to
    # start (seconds), how many queued dumps to allow, which texture id to
    # follow (hex), and where to write the texture. recomp_switch_on() returns
    # int and can express none of them.
    "RECOMP_GLYPH_DUMP",
    "RECOMP_GLYPH_DUMP_AFTER",
    "RECOMP_GLYPH_DUMP_MAXQ",
    "RECOMP_GLYPH_DUMP_TEX",
    "RECOMP_GLYPH_DUMP_TEXFILE",
    # How many surface slots the Metal backend keeps. A count: 95020c0 is the
    # commit where raising it rendered black, because eviction is the
    # writeback, so the number is the experiment and a boolean would delete it.
    "RECOMP_METAL_SURFACE_SLOTS",
    # The analogue stick dead zone, as a number. Zero is a meaningful setting
    # -- it is the arm that asks whether the dead zone is what is eating small
    # stick deflections -- so presence is the right test and "0" must not mean
    # "unset".
    "RECOMP_PAD_DEADZONE",
    # Which USB port to attach the pad to. A port number, and port 0 exists.
    "RECOMP_USB_PORT",
    # G51.1, the host's own 2D draws (d3d8_host_2d.c). The switch names a MODE,
    # not a state: "shadow" draws beside the executor and compares, and "draw"
    # is reserved for replacing the executor's pixels -- a boolean cannot name
    # the second arm. Its knobs are a tolerance in 565 steps, a dump directory,
    # a cap on dumps and a report interval in flips; 0 is a meaningful
    # tolerance. The positive control, RECOMP_D3D8_HOST_2D_CONTROL, is a plain
    # boolean and goes through recomp_switch_on.
    "RECOMP_D3D8_HOST_2D",
    "RECOMP_D3D8_HOST_2D_TOL",
    "RECOMP_D3D8_HOST_2D_DUMP",
    "RECOMP_D3D8_HOST_2D_DUMP_MAX",
    "RECOMP_D3D8_HOST_2D_EVERY",
    # G51.3: the fixed-function class's shadow. A mode like RECOMP_D3D8_HOST_2D
    # ("shadow", with "draw" to come), not a boolean.
    "RECOMP_D3D8_HOST_FF",
    # A count of flips: only every Nth is shadowed (a shadowed 3D flip takes ~1 s).
    "RECOMP_D3D8_HOST_FF_STRIDE",
}

ratcheted = set(handrolled) - VALUE_CARRYING
if len(ratcheted) > HANDROLLED_BASELINE:
    added = len(ratcheted) - HANDROLLED_BASELINE
    print("RATCHET: %d new hand-rolled switch read(s) -- the baseline is %d. "
          "New switches must use recomp_switch_on(), whose grammar is stated "
          "once; the hand-rolled form disagrees with it on \"on\", \"yes\" "
          "and \"false\". If the new switch carries a VALUE rather than a "
          "boolean, add it to VALUE_CARRYING with a reason instead."
          % (added, HANDROLLED_BASELINE))
    problems += 1
elif len(ratcheted) < HANDROLLED_BASELINE:
    print("NOTE: hand-rolled reads are down to %d from a baseline of %d -- "
          "lower HANDROLLED_BASELINE to lock the gain in."
          % (len(ratcheted), HANDROLLED_BASELINE))

# Rule 1
both = sorted(set(helper) & set(handrolled))
for name in both:
    files = helper[name] | handrolled[name]
    # one file reading it once and delegating is fine; two files is drift
    if len({os.path.basename(f) for f in files}) > 1:
        print("GRAMMAR: %s is read through both recomp_switch_on and a raw "
              "getenv, in %s" % (name, ", ".join(sorted(os.path.basename(f) for f in files))))
        problems += 1

# Rule 2
for name in sorted(default_on):
    if name in helper:
        continue                     # helper path: empty is off, but so is the default
    if name not in empty_safe:
        print("EMPTY: %s defaults ON but does not test for an empty value, so "
              "`%s= cmd` turns it off instead of leaving the default" % (name, name))
        problems += 1

# Rule 3
#
# SCOPE. This rule asks a different question from the ratchet above and needs a
# wider answer. The ratchet is about discipline in src/ -- the runtime the guest
# executes against. Rule 3 asks only "does this paths.conf line do ANYTHING",
# and a switch the HARNESS reads is not dead: paths.conf feeds the launcher,
# which feeds the harness as much as the runtime.
#
# Scanning diagnostics/ for the ratchet would be wrong -- it would sweep the
# harness's own hand-rolled getenvs into a baseline meant to police src/ -- so
# the wider scan is used for this rule and nothing else.
#
# Found the hard way: RECOMP_GUEST_NAMES was added on 18 Sep 2026, is read in
# diagnostics/jsrf_first_fault/guest_names.c, and was reported DEAD.
harness = os.path.join(root, "diagnostics")
harness_text = ""
for dirpath, _, names in os.walk(harness):
    for n in names:
        if n.endswith((".c", ".m", ".h")):
            harness_text += open(os.path.join(dirpath, n), errors="ignore").read()

conf = os.path.expanduser("~/Library/Application Support/JSRF/paths.conf")
if os.path.exists(conf):
    known = set(helper) | set(handrolled)
    for line in open(conf, errors="ignore"):
        m = re.match(r'\s*export\s+(RECOMP_[A-Z0-9_]+)=', line)
        if not m or m.group(1) in known:
            continue
        if ('"%s"' % m.group(1)) in harness_text:
            continue                 # read by the harness: not dead
        print("DEAD: %s is exported by paths.conf and read nowhere in "
              "src/ or diagnostics/" % m.group(1))
        problems += 1

print("switch audit: %d switches (%d via the helper, %d hand-rolled, "
      "%d default-on), %d problem(s)"
      % (len(set(helper) | set(handrolled)), len(helper), len(handrolled),
         len(default_on), problems))
sys.exit(1 if problems else 0)
