# G19 is fixed in the translator, and waiting on a regeneration

19 September 2026. `7c42b84`.

## State

The result-setter family (`and`/`or`/`xor`, `add`/`sub`, `adc`/`sbb`, `neg`,
`shl`/`shr`/`sar`, `shld`/`shrd`) now publishes its flags into `_fa`/`_fas`
next to the write, and `_make_condition` reads that pair instead of re-reading
a destination anything may have overwritten since. `add` and `sub` also capture
their source into `_fb` **before** the write, because `sub eax, eax` would
otherwise snapshot an operand it had already destroyed.

`MERGED_RESULT_SETTER` reads the snapshot too. That is the half that matters:
the cross-block join is the shape G19 was found in, and every predecessor now
publishes on its own edge.

## THE GENERATED TREE IS UNCHANGED

**Nothing was regenerated.** `~/jsrf-build/jsrf-first-fault/gen` is still the
pre-fix tree, the player bundle is byte-identical, and the 22:25 / 22:30 /
black-screen captures remain comparable to each other. **The fix is not in any
running binary.** It lands at the next regeneration, and until then G19 is
fixed in the tool and live in the title.

Before regenerating, remember CLAUDE.md: regeneration is not bit-stable across
translator changes, so a regen for this reason re-bases every other comparison
as well.

## How it is proved, given it cannot be run against the title

`tools/recomp/test_lifter_result_clobber.py`:

- compiles the lifter's own output for `and / mov / jcc` at 8, 16 and 32 bits
  and sweeps it against x86's answer computed independently;
- **negative control**: substitutes the pre-G19 expression back in and requires
  the sweep to catch it. The suite is green because the fix works, not because
  the sweep is blind;
- pins the cross-block join;
- `DeclarationCoverageTest` walks every mnemonic the dispatcher routes and
  requires the emitting end and the declaring end to agree.

That last one exists because the change nearly shipped broken. `sal` shares
`_lift_shift` with `shl`, so it emits a snapshot, but it never becomes a flag
setter and so is not in `_RESULT_ZF_SF_SETTERS` — a function whose only member
of the family was a `sal` would have referenced an undeclared `_fa` and failed
to compile. No unit test could have caught that; it needed a real function
shaped that way, and there was none.

## What is not established

- **That any of the six previously-identified live sites change behaviour.**
  They were never confirmed to execute. The fix is correct independently of
  that, but do not claim a bug was cured until a regenerated build shows it.
- **The size and speed cost.** One extra store per result-setter, at 629+
  clobber-prone sites and many more total. `_fa` is a local the optimiser can
  usually keep in a register, but that is a claim about `-O2`, not a
  measurement. Size the gen tree and re-run the frame-time A/B after the
  regeneration — and per the rules, that A/B owns the machine.

## Suites at this commit

193 translator tests, 70/70 ctest, switch audit 0 problems.
