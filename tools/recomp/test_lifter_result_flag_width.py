"""The rest of the flag setters read SF at 32 bits over narrow destinations.

`add` and `sub` were fixed for this; `neg`, `adc`/`sbb`, `and`/`or`/`xor`,
`shl`/`shr`/`sar` and `shld`/`shrd` were not. Every one of them emitted
`(int32_t)LO8(eax) < 0` for `js`, and `LO8`/`LO16`/`MEM8`/`MEM16` all
ZERO-extend on integer promotion, so that expression compares 0..255 against
zero: false for every result the instruction can produce. `or al, al; js` is
MSVC's "is this byte negative" and could not be taken at any 8- or 16-bit site
in the image.

`neg` had the second defect too, and it is the one that actually bites, because
`neg` is the setter whose OF is not rare. It sets OF=1 exactly when the operand
was the width's most negative value -- the one value whose negation does not
fit -- so the ordered signed conditions are SF != OF and part company with SF
at that value. Emitted as the sign bit, `jl` after `neg al` was wrong in both
directions: at al=1 the result is 0xFF (SF=1, OF=0, so jl IS taken) and the
zero-extended form said it was not; at al=0x80 the result is 0x80 (SF=1, OF=1,
so jl is NOT taken) and merely widening the cast would say it was.

Two further disagreements turned up while building the reference, neither of
them a width problem:

  * `jbe`/`ja` after `and`/`or`/`xor` were answered 0 and 1. Only CF is zero
    after those; ZF is not, so jbe is ZF and ja is !ZF. `and eax, eax; jbe` is
    taken when eax is zero and came out never taken.
  * `jbe`/`ja` after `neg` had no case at all, so they fell through to the
    `_flags` fallback that nothing assigns -- always false. After a neg, CF is
    "result != 0" and ZF is "result == 0", so jbe is always taken and came out
    never taken. `ja` was accidentally right: it is never taken either way.

The sweep is the real test: it compiles the lifter's own emitted write and its
emitted condition, runs them, and checks every answer against ZF/SF/OF/CF
computed from x86's definitions in int64, where no width can overflow. `neg` is
swept exhaustively -- all 256 bytes and all 65536 words, with junk in the bits
above them.

Alongside it is a second sweep that puts a value in the destination directly
and asks the condition about it, with no instruction in between. ZF and SF are
by definition questions about the destination, so that is a complete test of
the expression -- and it is the only one that can see two of these fixes at
all: `shr`, and `sar` as the lifter currently writes it, cannot leave a
negative byte behind for any nonzero count, so end to end their sign conditions
are unfalsifiable.

`NegativeControlTest` at the bottom substitutes the expressions these replaced,
one defect at a time, and requires each affected setter and width to be named
in the resulting disagreement list -- a blanket "something failed" would leave
ten of the eleven fixes untested. It also lists what must NOT be reported: at
32 bits the old sign cast IS the new one, `jb`/`jae` after and/or/xor really
are constants, and `ja` after neg was already right.

One caveat, established by running it rather than by reading the source: for
`sar` at 8 and 16 bits, and for `shld`/`shrd` at 16 bits, the lifter's
*arithmetic* is also wrong (`sar al,1` at 0x80 leaves 0x40 where x86 leaves
0xC0, because LO8 zero-extends before the arithmetic shift; 16-bit `shld`
shifts the incoming bits in from bit 32 instead of bit 16). That is a separate
defect in `lift_instruction`, not in the condition, and is NOT fixed here. The
end-to-end probes for those therefore ask the condition about the destination
the write actually left, rather than about x86's result -- which is still
exactly x86's definition of SF, just not an assertion that the write was right.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import Instruction, Operand
from .lifter import Lifter, _make_condition

_ROOT = Path(__file__).resolve().parents[2]

# (width, destination register, source register, how the harness reads the
# destination back after the write)
_WIDTHS = (
    (1, "al", "bl", "LO8(eax)"),
    (2, "ax", "bx", "LO16(eax)"),
    (4, "eax", "ebx", "eax"),
)

_DEST_READ = {1: "LO8(eax)", 2: "LO16(eax)", 4: "eax"}
_CAST = {1: "(int8_t)", 2: "(int16_t)", 4: "(int32_t)"}

# Chosen so every width sees zero, both signs, its own extremes and its own
# most negative value, and so the narrow cases carry junk in the bits above
# them: a condition that reads the whole 32-bit register fails here instead of
# passing on operands that happen to be small.
_VALUES = (0x00000000, 0x00000001, 0x00000002, 0x0000007F, 0x00000080,
           0x000000FF, 0x00007FFF, 0x00008000, 0x0000FFFF, 0x7FFFFFFF,
           0x80000000, 0xFFFFFFFF, 0xDEADBE80, 0x12345678)

_SIGN_JCCS = (("js", "J_S"), ("jns", "J_NS"), ("je", "J_Z"), ("jne", "J_NZ"))
_ORDERED_JCCS = (("jl", "J_L"), ("jge", "J_GE"), ("jle", "J_LE"),
                 ("jg", "J_G"))
_UNSIGNED_JCCS = (("jb", "J_B"), ("jae", "J_AE"), ("jbe", "J_BE"),
                  ("ja", "J_A"))


def _reg(name):
    return Operand(type="reg", reg=name)


def _lifted_write(mnemonic, operands, op_str):
    insn = Instruction(0, 3, mnemonic, op_str, "", operands=operands)
    lifted = " ".join(Lifter().lift_instruction(insn))
    assert "bad operands" not in lifted and "unknown" not in lifted, lifted
    return lifted


def _condition(jcc, mnemonic, operands):
    made = _make_condition(jcc, mnemonic, operands)
    assert made is not None, f"{mnemonic} + {jcc} refused a condition"
    return made[0]


class _Probe:
    """One (instruction, width, jcc) the harness will compile and sweep."""

    def __init__(self, mnemonic, operands, op_str, width, group, kind,
                 from_dest, jccs, direct=False):
        self.direct = direct
        self.mnemonic = mnemonic
        self.operands = operands
        self.op_str = op_str
        self.width = width
        self.group = group
        self.kind = kind
        self.from_dest = from_dest
        self.jccs = jccs

    @property
    def dest(self):
        return _DEST_READ[self.width]


# Setters whose ZF and SF are, by x86's definition, a question about the
# destination alone. The condition is then a pure function of that destination,
# so it can be checked by putting a value there directly -- no write involved.
# That matters because two of these have a write bug of their own (module
# docstring), and because `shr` and the mis-written `sar` can never leave a
# negative byte at all: end to end their sign conditions are unfalsifiable, and
# only a direct destination proves the expression itself.
_DIRECT = (("adc", "K_ADC", _SIGN_JCCS), ("sbb", "K_SBB", _SIGN_JCCS),
           ("and", "K_AND", None), ("or", "K_OR", None),
           ("xor", "K_XOR", None), ("neg", "K_NEG", None),
           ("shl", "K_SHL", _SIGN_JCCS), ("shr", "K_SHR", _SIGN_JCCS),
           ("sar", "K_SAR", _SIGN_JCCS), ("shld", "K_SHLD", _SIGN_JCCS),
           ("shrd", "K_SHRD", _SIGN_JCCS))


def _probes():
    out = []
    for width, lo, hi, _dest in _WIDTHS:
        for mnemonic, kind, jccs in _DIRECT:
            if mnemonic in ("shld", "shrd"):
                if width == 1:
                    continue
                operands = [_reg(lo), _reg(hi), _reg("cl")]
            elif mnemonic == "neg":
                operands = [_reg(lo)]
            elif mnemonic in ("shl", "shr", "sar"):
                operands = [_reg(lo), _reg("cl")]
            else:
                operands = [_reg(lo), _reg(hi)]
            out.append(_Probe(
                mnemonic, operands, "(destination)", width, "G_DIRECT", kind,
                True,
                jccs or (_SIGN_JCCS + _ORDERED_JCCS + _UNSIGNED_JCCS),
                direct=True))
        two, two_str = [_reg(lo), _reg(hi)], f"{lo}, {hi}"
        for mnemonic, kind in (("adc", "K_ADC"), ("sbb", "K_SBB")):
            out.append(_Probe(mnemonic, two, two_str, width, "G_AB", kind,
                              False, _SIGN_JCCS))
        for mnemonic, kind in (("and", "K_AND"), ("or", "K_OR"),
                               ("xor", "K_XOR")):
            out.append(_Probe(mnemonic, two, two_str, width, "G_AB", kind,
                              False,
                              _SIGN_JCCS + _ORDERED_JCCS + _UNSIGNED_JCCS))
        out.append(_Probe("neg", [_reg(lo)], lo, width, "G_NEG", "K_NEG",
                          False,
                          _SIGN_JCCS + _ORDERED_JCCS + _UNSIGNED_JCCS))
        shift, shift_str = [_reg(lo), _reg("cl")], f"{lo}, cl"
        for mnemonic, kind in (("shl", "K_SHL"), ("shr", "K_SHR"),
                               ("sar", "K_SAR")):
            # sar's own arithmetic is wrong at 8 and 16 bits (module
            # docstring), so there the reference reads the destination the
            # write left rather than recomputing x86's result.
            out.append(_Probe(mnemonic, shift, shift_str, width, "G_SHIFT",
                              kind, mnemonic == "sar" and width != 4,
                              _SIGN_JCCS))
        if width != 1:  # shld/shrd have no 8-bit form
            dbl, dbl_str = [_reg(lo), _reg(hi), _reg("cl")], f"{lo}, {hi}, cl"
            for mnemonic, kind in (("shld", "K_SHLD"), ("shrd", "K_SHRD")):
                out.append(_Probe(mnemonic, dbl, dbl_str, width, "G_SHIFT",
                                  kind, width != 4, _SIGN_JCCS))
    return out


def _build_source(condition_for):
    """Render the C harness. condition_for(jcc, mnemonic, operands) -> str."""
    cases, table = [], []
    for probe in _probes():
        if probe.direct:
            # G19: the condition is a pure function of the RESULT, but since
            # the family started publishing that result it reads `_fa` rather
            # than the live destination. A direct probe therefore has to
            # publish the value it is planting, exactly as the real setter
            # would -- masked and sign-extended at the probe's own width.
            # Without this the probe measures an uninitialised `_fa`, which is
            # not a statement about the expression at all.
            mask = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}[probe.width]
            write = (f"eax = a; (void)b; (void)cnt;"
                     f" _fa = (uint32_t)({probe.dest}) & {mask};"
                     f" _fas = (int32_t){_CAST[probe.width]}(_fa);")
        else:
            write = _lifted_write(probe.mnemonic, probe.operands, probe.op_str)
        for jcc, code in probe.jccs:
            name = (f"case_{'direct_' if probe.direct else ''}"
                    f"{probe.mnemonic}_{probe.width}_{jcc}")
            cases.append(
                f"static int {name}(uint32_t a, uint32_t b, uint32_t cnt,\n"
                f"                  int cf_in) {{\n"
                f"    eax = a; ebx = b; ecx = cnt; _cf = cf_in;\n"
                f"    {write}\n"
                f"    g_dest = {probe.dest};\n"
                f"    return ("
                f"{condition_for(jcc, probe.mnemonic, probe.operands)})"
                f" ? 1 : 0;\n"
                f"}}")
            table.append(
                f"    {{ {name}, {code}, {probe.kind}, {probe.group}, "
                f"{int(probe.from_dest)}, {probe.width}, "
                f'"{probe.mnemonic} {probe.op_str} {jcc} w{probe.width}" }},')
    return (_HARNESS
            .replace("/*CASES*/", "\n\n".join(cases))
            .replace("/*TABLE*/", "\n".join(table))
            .replace("/*VALUES*/", ", ".join(f"0x{v:08X}u" for v in _VALUES)))


class _CompileAndRun(unittest.TestCase):
    def build_and_run(self, source, expect_success=True):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("no C compiler available")
        with tempfile.TemporaryDirectory(prefix="flag-width-") as tmp:
            tmp = Path(tmp)
            src, exe = tmp / "test.c", tmp / "test"
            src.write_text(source, encoding="utf-8")
            built = subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(_ROOT / "templates" / "runtime"),
                 str(src), "-o", str(exe)],
                capture_output=True, text=True)
            self.assertEqual(0, built.returncode, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            if expect_success:
                self.assertEqual(0, ran.returncode, ran.stdout + ran.stderr)
            return ran


class ResultFlagWidthEmissionTest(unittest.TestCase):
    """Static shape checks. The runtime sweep is what actually proves them."""

    def test_sign_conditions_cast_back_to_the_operand_width(self):
        for width, lo, hi, _dest in _WIDTHS:
            shapes = [("adc", [_reg(lo), _reg(hi)]),
                      ("sbb", [_reg(lo), _reg(hi)]),
                      ("and", [_reg(lo), _reg(hi)]),
                      ("or", [_reg(lo), _reg(hi)]),
                      ("xor", [_reg(lo), _reg(hi)]),
                      ("neg", [_reg(lo)]),
                      ("shl", [_reg(lo), _reg("cl")]),
                      ("shr", [_reg(lo), _reg("cl")]),
                      ("sar", [_reg(lo), _reg("cl")])]
            if width != 1:
                shapes += [("shld", [_reg(lo), _reg(hi), _reg("cl")]),
                           ("shrd", [_reg(lo), _reg(hi), _reg("cl")])]
            for mnemonic, operands in shapes:
                for jcc in ("js", "jns"):
                    with self.subTest(mnemonic=mnemonic, w=width, jcc=jcc):
                        cond = _condition(jcc, mnemonic, operands)
                        self.assertIn(_CAST[width], cond)
                        if width != 4:
                            self.assertNotIn("(int32_t)", cond)

    def test_and_or_xor_keep_answering_the_signed_forms_with_SF(self):
        # OF is architecturally 0 after these, so SF != OF really is SF. The
        # width fix must not have grown a spurious overflow reconstruction.
        for mnemonic in ("and", "or", "xor"):
            for jcc, op in (("jl", "<"), ("jge", ">="),
                            ("jle", "<="), ("jg", ">")):
                with self.subTest(mnemonic=mnemonic, jcc=jcc):
                    cond = _condition(jcc, mnemonic, [_reg("al"), _reg("bl")])
                    # Reads the published result (G19), still at 8 bits, and
                    # still with no overflow reconstruction -- which is the
                    # property this test is about.
                    self.assertEqual(f"((int8_t)(_fa) {op} 0)", cond)

    def test_neg_signed_conditions_no_longer_answer_with_the_sign_bit(self):
        for width, lo, _hi, _d in _WIDTHS:
            for jcc in ("jl", "jge", "jle", "jg"):
                with self.subTest(w=width, jcc=jcc):
                    cond = _condition(jcc, "neg", [_reg(lo)])
                    dest = _DEST_READ[width]
                    for op in ("<", ">", "<=", ">="):
                        # Every shape the sign-bit answer took, at both the
                        # old width and the merely-widened one.
                        self.assertNotEqual(f"((int32_t){dest} {op} 0)", cond)
                        self.assertNotEqual(
                            f"({_CAST[width]}{dest} {op} 0)", cond)
                    # OF is only recoverable by getting the original operand
                    # back out of the result.
                    self.assertIn("0u -", cond)

    def test_touched_branches_read_the_published_result(self):
        # INVERTED BY G19, DELIBERATELY. This test used to assert the opposite
        # -- that these branches must never mention _fa -- for one stated
        # reason: translator.py declared the pair only for functions holding a
        # cmp/test/bsf/bsr/inc/dec/cmpxchg, so referencing it anywhere else
        # produced C that would not compile.
        #
        # That was a constraint about the DECLARATION, not about what the
        # condition should read, and re-reading a destination at the branch is
        # wrong whenever anything has written it since. The declaration list
        # now includes the whole result-setter family, so the branches read
        # the snapshot their setter published -- and the guard becomes the
        # assertion that they do. The declaration half is pinned separately by
        # test_translator_declares_the_pair_for_the_family below.
        setters = (
            ("adc", [_reg("al"), _reg("bl")]),
            ("sbb", [_reg("al"), _reg("bl")]),
            ("and", [_reg("al"), _reg("bl")]),
            ("or", [_reg("al"), _reg("bl")]),
            ("xor", [_reg("al"), _reg("bl")]),
            ("neg", [_reg("al")]),
            ("shl", [_reg("al"), _reg("cl")]),
            ("shr", [_reg("al"), _reg("cl")]),
            ("sar", [_reg("al"), _reg("cl")]),
            ("shld", [_reg("ax"), _reg("bx"), _reg("cl")]),
            ("shrd", [_reg("ax"), _reg("bx"), _reg("cl")]),
        )
        for mnemonic, operands in setters:
            for jcc, _code in _SIGN_JCCS + _ORDERED_JCCS + _UNSIGNED_JCCS:
                made = _make_condition(jcc, mnemonic, operands)
                if made is None:
                    continue
                with self.subTest(mnemonic=mnemonic, jcc=jcc):
                    # The invariant is that NOTHING here re-reads the live
                    # destination. Some conditions reach that by reading _fa,
                    # others by reading only _cf, and a couple are outright
                    # constants (and/or/xor's jb is 0 because CF is). All
                    # three are fine; a mention of LO8(eax) is not.
                    self.assertNotIn("LO8(eax)", made[0])
                    self.assertNotIn("LO16(eax)", made[0])

    def test_translator_declares_the_pair_for_the_family(self):
        """The other half: every setter that publishes must be declared for."""
        src = (Path(__file__).resolve().parent / "translator.py").read_text()
        decl = src.split("uint32_t _fa = 0, _fb = 0;")[0].rsplit("if any(", 1)[1]
        self.assertIn("_RESULT_ZF_SF_SETTERS", decl,
                      "the family publishes _fa but the declaration gate "
                      "does not cover it -- the generated C will not compile")

    def test_adc_and_sbb_still_refuse_the_ordered_signed_conditions(self):
        # OF after adc/sbb needs the original destination and the carry-in;
        # the result alone does not recover it. Refusing beats guessing, and is
        # what sub does in the same situation.
        for mnemonic in ("adc", "sbb"):
            for jcc in ("jl", "jge", "jle", "jg"):
                with self.subTest(mnemonic=mnemonic, jcc=jcc):
                    self.assertIsNone(
                        _make_condition(jcc, mnemonic,
                                        [_reg("al"), _reg("bl")]))


class ResultFlagWidthRuntimeTest(_CompileAndRun):
    def test_every_condition_matches_x86_at_every_width(self):
        self.build_and_run(_build_source(_pick_current))


def _pick_current(jcc, mnemonic, operands):
    return _condition(jcc, mnemonic, operands)


class NegativeControlTest(_CompileAndRun):
    """Each fix, reverted one at a time, must fail the sweep above.

    Without these the sweep could be passing because it asks nothing. Each
    substitutes only the expression its own fix replaced and asserts the
    harness reports a disagreement.
    """

    def _run_with_old(self, old_for):
        def pick(jcc, mnemonic, operands):
            replaced = old_for(jcc, mnemonic, operands)
            if replaced is not None:
                return replaced
            return _condition(jcc, mnemonic, operands)
        return self.build_and_run(_build_source(pick), expect_success=False)

    @staticmethod
    def _width_of(operands):
        return {"al": 1, "ax": 2, "eax": 4}[str(operands[0].reg)]

    def _assert_rejected(self, ran, expected, forbid=()):
        """Every named probe must have disagreed, and nothing in `forbid`."""
        self.assertNotEqual(
            0, ran.returncode,
            "the old expression passed the sweep, so the sweep does not test "
            "what it claims to:\n" + ran.stdout)
        reported = {line.split("  ")[0] for line in ran.stdout.splitlines()
                    if "got" in line}
        for label in expected:
            self.assertIn(
                label, reported,
                f"{label} was NOT rejected -- the fix for it is untested.\n"
                + ran.stdout)
        for label in forbid:
            self.assertNotIn(label, reported, ran.stdout)

    @staticmethod
    def _direct(mnemonic, jcc, width):
        return f"{mnemonic} (destination) {jcc} w{width}"

    # Widths where a 32-bit cast is a different expression from the right one.
    _NARROW = (1, 2)
    _ORDERED = {"jl": "<", "jge": ">=", "jle": "<=", "jg": ">"}

    def test_old_32_bit_sign_cast_is_rejected(self):
        # The defect shared by every setter touched: SF read at 32 bits over a
        # zero-extending narrow accessor. Each setter is named individually --
        # one blanket "something failed" would leave ten of the eleven fixes
        # untested.
        targets = ("adc", "sbb", "and", "or", "xor", "neg",
                   "shl", "shr", "sar", "shld", "shrd")

        def old(jcc, mnemonic, operands):
            if mnemonic in targets and jcc in ("js", "jns"):
                dest = _DEST_READ[self._width_of(operands)]
                return f"((int32_t){dest} {'<' if jcc == 'js' else '>='} 0)"
            return None
        expected = [self._direct(m, jcc, w)
                    for m in targets for w in self._NARROW
                    for jcc in ("js", "jns")
                    if not (w == 1 and m in ("shld", "shrd"))]
        # At 32 bits the old expression IS the new one, so nothing there may
        # be reported -- that is what makes this control specific to width.
        forbid = [self._direct(m, jcc, 4)
                  for m in targets for jcc in ("js", "jns")]
        self._assert_rejected(self._run_with_old(old), expected, forbid)

    def test_old_and_or_xor_signed_forms_are_rejected(self):
        def old(jcc, mnemonic, operands):
            if mnemonic in ("and", "or", "xor") and jcc in self._ORDERED:
                dest = _DEST_READ[self._width_of(operands)]
                return f"((int32_t){dest} {self._ORDERED[jcc]} 0)"
            return None
        expected = [self._direct(m, jcc, w)
                    for m in ("and", "or", "xor") for w in self._NARROW
                    for jcc in self._ORDERED]
        self._assert_rejected(self._run_with_old(old), expected)

    def test_old_neg_sign_bit_answer_for_the_ordered_conditions_is_rejected(
            self):
        def old(jcc, mnemonic, operands):
            if mnemonic == "neg" and jcc in self._ORDERED:
                dest = _DEST_READ[self._width_of(operands)]
                return f"((int32_t){dest} {self._ORDERED[jcc]} 0)"
            return None
        expected = [self._direct("neg", jcc, w)
                    for w in (1, 2, 4) for jcc in self._ORDERED]
        expected += [f"neg {reg} {jcc} w{w}"
                     for w, reg in ((1, "al"), (2, "ax"), (4, "eax"))
                     for jcc in self._ORDERED]
        self._assert_rejected(self._run_with_old(old), expected)

    def test_neg_ordered_conditions_need_OF_not_just_a_wider_cast(self):
        # Width alone is not enough. At the most negative operand OF is set and
        # SF != OF parts company with SF, so this control -- correct width,
        # sign-bit answer -- must still fail. It is the half of the neg fix the
        # width control above cannot see.
        def old(jcc, mnemonic, operands):
            if mnemonic == "neg" and jcc in self._ORDERED:
                width = self._width_of(operands)
                dest = _DEST_READ[width]
                return f"({_CAST[width]}{dest} {self._ORDERED[jcc]} 0)"
            return None
        ran = self._run_with_old(old)
        expected = [self._direct("neg", jcc, w)
                    for w in (1, 2, 4) for jcc in self._ORDERED]
        expected += [f"neg {reg} {jcc} w{w}"
                     for w, reg in ((1, "al"), (2, "ax"), (4, "eax"))
                     for jcc in self._ORDERED]
        self._assert_rejected(ran, expected)
        # And it must fail ONLY at the most negative value -- anywhere else and
        # the reference, not the expression, would be the thing in question.
        for line in ran.stdout.splitlines():
            if line.startswith("neg ") and " w1 " in line:
                self.assertIn("a=0x00000080", line, line)

    def test_old_and_or_xor_unsigned_constants_are_rejected(self):
        def old(jcc, mnemonic, operands):
            if mnemonic in ("and", "or", "xor"):
                if jcc in ("jb", "jbe"):
                    return "0"
                if jcc in ("jae", "ja"):
                    return "1"
            return None
        expected = [self._direct(m, jcc, w)
                    for m in ("and", "or", "xor") for w in (1, 2, 4)
                    for jcc in ("jbe", "ja")]
        # jb/jae really are constants after these -- the control reverts them
        # too, and they must NOT be reported.
        forbid = [self._direct(m, jcc, w)
                  for m in ("and", "or", "xor") for w in (1, 2, 4)
                  for jcc in ("jb", "jae")]
        self._assert_rejected(self._run_with_old(old), expected, forbid)

    def test_neg_jbe_used_to_be_unanswerable(self):
        # jbe and ja returned None, which the lifter turns into the `_flags`
        # fallback -- a variable nothing ever assigns, so both came out never
        # taken. Model that as the constant it became.
        #
        # Only jbe is a behaviour change: after a neg, CF is "result != 0" and
        # ZF is "result == 0", so jbe is always taken (and was never taken)
        # while ja is never taken -- which is what the always-false fallback
        # already said. So ja is listed under `forbid`, not `expected`: it was
        # accidentally right, and claiming the fix changed it would overstate
        # what was measured.
        def old(jcc, mnemonic, operands):
            if mnemonic == "neg" and jcc in ("jbe", "ja"):
                return "0 /* _flags fallback */"
            return None
        expected = [self._direct("neg", "jbe", w) for w in (1, 2, 4)]
        forbid = [self._direct("neg", "ja", w) for w in (1, 2, 4)]
        self._assert_rejected(self._run_with_old(old), expected, forbid)


_HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;
static uint32_t eax, ebx, ecx;
/* G19: the result-setter family publishes its flags into this pair
   where the result is computed, so a jcc reads the snapshot rather
   than a destination something may have overwritten since. */
uint32_t _fa, _fb;
int32_t _fas, _fbs;
static int _cf;
static uint32_t g_dest;

enum { J_S, J_NS, J_Z, J_NZ, J_L, J_GE, J_LE, J_G, J_B, J_AE, J_BE, J_A };
enum { K_ADC, K_SBB, K_AND, K_OR, K_XOR, K_NEG,
       K_SHL, K_SHR, K_SAR, K_SHLD, K_SHRD };
enum { G_AB, G_NEG, G_SHIFT, G_DIRECT };

static int64_t sx(uint32_t v, int w) {
    if (w == 1) return (int8_t)v;
    if (w == 2) return (int16_t)v;
    return (int32_t)v;
}

static uint32_t narrow(uint32_t v, int w) {
    if (w == 1) return v & 0xFFu;
    if (w == 2) return v & 0xFFFFu;
    return v;
}

/* x86's own definitions. The exact value is computed in int64, where no
   operand width can overflow, so OF is "the truncated result is not the exact
   one" rather than a sign-juggling rule that could be wrong the same way the
   code under test was. CF is per-instruction and written out per case. */
static int reference(int kind, int from_dest, int w, uint32_t a, uint32_t b,
                     uint32_t cnt, int cf_in, uint32_t dest, int code) {
    uint32_t av = narrow(a, w), bv = narrow(b, w);
    uint32_t result;
    int64_t exact;
    int of = 0, cf = 0;
    int bits = w * 8;
    unsigned n = (unsigned)(cnt & 0xFFu);

    if (from_dest) {
        /* ZF and SF are, by x86's definition, questions about the destination
           alone, so ask them of the destination rather than recomputing the
           instruction -- which is what lets this check the condition without
           also asserting that the lifter's arithmetic was right (for sar at
           8/16 and 16-bit shld/shrd it is not, and that is a different
           defect). neg is the one setter here whose OF and CF are also
           recoverable from the destination: negation is its own inverse
           modulo 2^width, so OF -- "the operand was the most negative value"
           -- is exactly "the result is". */
        result = narrow(dest, w);
        exact = sx(result, w);
        if (kind == K_NEG) {
            of = (result == (1u << (bits - 1)));
            cf = (result != 0);
        }
    } else {
        switch (kind) {
        case K_ADC:
            exact = sx(a, w) + sx(b, w) + cf_in;
            result = narrow(av + bv + (uint32_t)cf_in, w);
            cf = (((uint64_t)av + bv + (uint64_t)(unsigned)cf_in)
                  >> bits) ? 1 : 0;
            break;
        case K_SBB:
            exact = sx(a, w) - sx(b, w) - cf_in;
            result = narrow(av - bv - (uint32_t)cf_in, w);
            cf = ((uint64_t)bv + (uint64_t)(unsigned)cf_in) > av;
            break;
        case K_AND: result = narrow(av & bv, w); exact = sx(result, w); break;
        case K_OR:  result = narrow(av | bv, w); exact = sx(result, w); break;
        case K_XOR: result = narrow(av ^ bv, w); exact = sx(result, w); break;
        case K_NEG:
            exact = -sx(a, w);
            result = narrow(0u - av, w);
            cf = (av != 0);
            break;
        case K_SHL:
            result = narrow(av << n, w); exact = sx(result, w); break;
        case K_SHR:
            result = narrow(av >> n, w); exact = sx(result, w); break;
        case K_SAR:
            result = narrow((uint32_t)(sx(av, w) >> n), w);
            exact = sx(result, w);
            break;
        case K_SHLD:
            result = narrow((av << n) | (bv >> (bits - n)), w);
            exact = sx(result, w);
            break;
        case K_SHRD:
            result = narrow((av >> n) | (bv << (bits - n)), w);
            exact = sx(result, w);
            break;
        default: return -1;
        }
        of = (sx(result, w) != exact);
    }

    {
        int zf = (result == 0);
        int sf = (sx(result, w) < 0);
        switch (code) {
        case J_S:  return sf;
        case J_NS: return !sf;
        case J_Z:  return zf;
        case J_NZ: return !zf;
        case J_L:  return sf != of;
        case J_GE: return sf == of;
        case J_LE: return zf || (sf != of);
        case J_G:  return !zf && (sf == of);
        case J_B:  return cf;
        case J_AE: return !cf;
        case J_BE: return cf || zf;
        case J_A:  return !cf && !zf;
        }
    }
    return -1;
}

/*CASES*/

struct probe {
    int (*fn)(uint32_t, uint32_t, uint32_t, int);
    int code;
    int kind;
    int group;
    int from_dest;
    int width;
    const char *what;
};

static const struct probe probes[] = {
/*TABLE*/
};

static const uint32_t values[] = { /*VALUES*/ };
#define NVALUES (sizeof values / sizeof values[0])
#define NPROBES (sizeof probes / sizeof probes[0])

static int failures;
static int probe_failed;

/* One line per disagreeing probe, and never an early exit: a negative control
   has to be able to see that EVERY setter it reverted was caught, not just the
   first one the sweep happened to reach. */
static void check(const struct probe *p, uint32_t a, uint32_t b, uint32_t cnt,
                  int cf_in) {
    int got = p->fn(a, b, cnt, cf_in);
    int want = reference(p->kind, p->from_dest, p->width, a, b, cnt, cf_in,
                         g_dest, p->code);
    if (got != want) {
        failures++;
        if (!probe_failed) {
            probe_failed = 1;
            printf("%s  a=0x%08X b=0x%08X cnt=%u cf=%d  got %d want %d\n",
                   p->what, a, b, cnt, cf_in, got, want);
        }
    }
}

int main(void) {
    size_t p, i, j;
    unsigned n;
    uint32_t v;

    for (p = 0; p < NPROBES; p++) {
        const struct probe *pr = &probes[p];
        probe_failed = 0;
        if (pr->group == G_AB) {
            for (i = 0; i < NVALUES; i++)
                for (j = 0; j < NVALUES; j++) {
                    check(pr, values[i], values[j], 0, 0);
                    check(pr, values[i], values[j], 0, 1);
                }
        } else if (pr->group == G_NEG || pr->group == G_DIRECT) {
            /* Exhaustive: 0..0xFFFF covers every byte and every word, and the
               junk in the top half proves the condition is not reading the
               whole 32-bit register instead of the sub-register. */
            for (v = 0; v <= 0xFFFFu; v++) {
                check(pr, v, 0, 0, 0);
                check(pr, v | 0xDEAD0000u, 0, 0, 0);
            }
            for (i = 0; i < NVALUES; i++)
                check(pr, values[i], 0, 0, 0);
        } else {
            /* Shift counts 1..bits-1. Zero is excluded: x86 leaves the flags
               untouched for a masked count of zero and the lifter does not
               model that -- a pre-existing gap, and a different one. The
               second operand only matters to shld/shrd; the single-operand
               shifts ignore it, so sweeping it for them is only redundancy. */
            for (i = 0; i < NVALUES; i++)
                for (j = 0; j < NVALUES; j++)
                    for (n = 1; n < (unsigned)(pr->width * 8); n++)
                        check(pr, values[i], values[j], n, 0);
        }
    }
    if (failures)
        printf("%d disagreements in total\n", failures);
    return failures != 0;
}
'''


if __name__ == "__main__":
    unittest.main()
