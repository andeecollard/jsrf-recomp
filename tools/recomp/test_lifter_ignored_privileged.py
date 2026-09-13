"""A privileged instruction we ignore must say so, not look like ordinary output.

`in`, `out` and `wbinvd` were falling through to the generic unimplemented
path, which emits `/* TODO: in al, dx */` and nothing else. That is the same
shape as every other bug this translator has paid for: an x86 behaviour emitted
as nothing, indistinguishable from translated code that ran. Ignoring these
three IS the right answer on this host -- there is no I/O port space in a
user-mode recompilation and no guest-visible cache to flush -- but "right
answer" and "no answer" have to be told apart by someone reading the output,
and by grep.

They were the whole of the reachable unimplemented set in JSRF on 13 Sep 2026,
one site each, so this is also the difference between a reachable-unimplemented
count of three and of zero.

Behaviour is deliberately unchanged: the emitted statement is a no-op, and for
`in` the destination register keeps whatever it held, exactly as before. The
marker is the deliverable. `RECOMP-IGNORED-PRIV` is greppable over a gen tree.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import Instruction, Operand
from .lifter import Lifter, _IGNORED_PRIVILEGED

MARKER = "RECOMP-IGNORED-PRIV"


def _insn(mnemonic, op_str=""):
    i = Instruction(0x00401000, 2, mnemonic, op_str, "")
    i.operands = []
    return i


def _lift(mnemonic, op_str=""):
    lifter = Lifter()
    out = lifter.lift_instruction(_insn(mnemonic, op_str))
    return out, lifter


class IgnoredPrivilegedTest(unittest.TestCase):

    def test_the_three_reachable_sites_are_marked(self):
        for mnemonic, op_str in (("in", "al, dx"),
                                 ("out", "dx, al"),
                                 ("wbinvd", "")):
            with self.subTest(mnemonic=mnemonic):
                out, _ = _lift(mnemonic, op_str)
                joined = "\n".join(out)
                self.assertIn(MARKER, joined)
                # The instruction names itself, operands included.
                self.assertIn(mnemonic, joined)
                if op_str:
                    self.assertIn(op_str, joined)
                # And says why ignoring it is safe here.
                self.assertIn(_IGNORED_PRIVILEGED[mnemonic], joined)

    def test_it_is_a_statement_and_not_only_a_comment(self):
        # A bare comment is what the TODO path emits; it disappears into the
        # surrounding code. A `(void)0;` statement keeps a line of its own and
        # survives a reader skimming for statements.
        for mnemonic in _IGNORED_PRIVILEGED:
            with self.subTest(mnemonic=mnemonic):
                out, _ = _lift(mnemonic)
                self.assertTrue(out[0].startswith("(void)0;"), out[0])

    def test_in_admits_that_the_destination_is_untouched(self):
        # The one case where ignoring the instruction has a visible cost: the
        # register keeps a stale value rather than reading a port. Say it,
        # because inferring it from an absent assignment is exactly the kind of
        # reading this translator has been wrong about before.
        out, _ = _lift("in", "al, dx")
        self.assertIn("left unchanged", "\n".join(out))

    def test_they_leave_the_unimplemented_report(self):
        # The report exists to list work that is missing. A decision that has
        # been made and written down is not missing work, and leaving it there
        # hides the mnemonics that are.
        for mnemonic in _IGNORED_PRIVILEGED:
            with self.subTest(mnemonic=mnemonic):
                _, lifter = _lift(mnemonic)
                self.assertEqual(lifter.unimplemented, {})

    def test_string_port_io_keeps_its_todo(self):
        # ins/outs move guest memory. Ignoring those silently would be a real
        # behaviour change, not a recorded decision, so they must NOT be swept
        # into the ignored set along with `in`/`out`.
        for mnemonic in ("insb", "insd", "outsb", "outsd"):
            with self.subTest(mnemonic=mnemonic):
                out, lifter = _lift(mnemonic)
                self.assertIn("TODO", out[0])
                self.assertNotIn(MARKER, out[0])
                self.assertIn(mnemonic, lifter.unimplemented)

    def test_hlt_keeps_its_todo(self):
        # `hlt` waits for an interrupt. A no-op turns that wait into a spin,
        # so it is not in the ignored set either.
        out, lifter = _lift("hlt")
        self.assertIn("TODO", out[0])
        self.assertIn("hlt", lifter.unimplemented)

    def test_the_emitted_lines_compile(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler unavailable")
        body = []
        for mnemonic, op_str in (("in", "al, dx"), ("out", "dx, al"),
                                 ("wbinvd", ""), ("invd", ""),
                                 ("invlpg", "[eax]")):
            body += ["    " + line
                     for line in _lift(mnemonic, op_str)[0]]
        source = ("int main(void)\n{\n" + "\n".join(body) + "\n    return 0;\n}\n")
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "t.c", Path(tmp) / "t"
            src.write_text(source)
            subprocess.run([compiler, "-std=c11", "-Wall", "-Werror",
                            str(src), "-o", str(exe)],
                           check=True, capture_output=True)
            subprocess.run([str(exe)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
