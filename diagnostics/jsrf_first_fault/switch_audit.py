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
    for name in re.findall(r'recomp_switch_on\(\s*"(RECOMP_[A-Z0-9_]+)"', text):
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

# RULE 4, A RATCHET. 128 of the 152 switches parse their own value, and
# rewriting all of them in one change would be a large diff over code whose
# defaults decide whether the picture is correct -- exactly the kind of sweep
# that flips one by accident. So the set is FROZEN instead: migrate downward at
# leisure, but a NEW switch must use the helper, whose grammar is stated in one
# place. The number only ever goes down.
HANDROLLED_BASELINE = 128
if len(handrolled) > HANDROLLED_BASELINE:
    added = len(handrolled) - HANDROLLED_BASELINE
    print("RATCHET: %d new hand-rolled switch read(s) -- the baseline is %d. "
          "New switches must use recomp_switch_on(), whose grammar is stated "
          "once; the hand-rolled form disagrees with it on \"on\", \"yes\" "
          "and \"false\"." % (added, HANDROLLED_BASELINE))
    problems += 1
elif len(handrolled) < HANDROLLED_BASELINE:
    print("NOTE: hand-rolled reads are down to %d from a baseline of %d -- "
          "lower HANDROLLED_BASELINE to lock the gain in."
          % (len(handrolled), HANDROLLED_BASELINE))

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
conf = os.path.expanduser("~/Library/Application Support/JSRF/paths.conf")
if os.path.exists(conf):
    known = set(helper) | set(handrolled)
    for line in open(conf, errors="ignore"):
        m = re.match(r'\s*export\s+(RECOMP_[A-Z0-9_]+)=', line)
        if m and m.group(1) not in known:
            print("DEAD: %s is exported by paths.conf and read nowhere in src/"
                  % m.group(1))
            problems += 1

print("switch audit: %d switches (%d via the helper, %d hand-rolled, "
      "%d default-on), %d problem(s)"
      % (len(set(helper) | set(handrolled)), len(helper), len(handrolled),
         len(default_on), problems))
sys.exit(1 if problems else 0)
