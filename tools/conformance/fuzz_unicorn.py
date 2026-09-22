"""Differential fuzz of our x86 lifter against Unicorn's x86-32 core.

Ours, not upstream's.  Upstream's `tools/conformance/fuzz.py` (PR #62) uses
NATIVE 32-bit x86 execution as its oracle, and this host cannot provide one:
Apple Silicon, no Rosetta -- and Rosetta 2 cannot execute 32-bit x86 even when
installed -- no Docker, and the conformance runner shells out to `cl.exe`.
This runs entirely locally instead.

    Intel-syntax text -> clang(i386 target) -> bytes
    bytes -> Unicorn x86-32 core                  -> eax   (the oracle)
    bytes -> our disasm + lifter -> C -> clang    -> eax   (what we generate)

READ THIS BEFORE BELIEVING A RESULT.  Unicorn is a second MODEL, not silicon.
A disagreement is a lead, not a verdict, and each one must be adjudicated by
hand against the SDM.  The first run made the point for itself: 9 of 24
"mismatches" were `setcc` reading a flag that BT/BTS/BTR/BTC leave
architecturally UNDEFINED, so they compared two models' choices of undefined
rather than either against x86.  The generator no longer emits those.

Cases are reproducible from `--seed` plus the case index, so anything real can
be promoted into a permanent regression test.

Setup (the two packages are not vendored, and keystone is NOT needed -- clang
assembles the snippets):

    python3 -m venv .venv && .venv/bin/pip install unicorn capstone
    .venv/bin/python -m tools.conformance.fuzz_unicorn --count 600 --seed 7

Coverage limits, stated so a zero is not over-read: GPR integer only, no
memory operands, no FPU, no SSE, no faulting instructions, and the comparison
is EAX after the snippet -- not the flags themselves.
"""
import argparse, os, random, re, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                               UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_ESP,
                               UC_X86_REG_EFLAGS)

BASE, STACK = 0x00100000, 0x00200000

EDGES = (0x00000000, 0x00000001, 0x00000002, 0x0000007F, 0x00000080,
         0x000000FF, 0x00000100, 0x00007FFF, 0x00008000, 0x0000FFFF,
         0x00010000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFE, 0xFFFFFFFF,
         0x12345678, 0x87654321)
WIDTHS = {8: ("al", "cl"), 16: ("ax", "cx"), 32: ("eax", "ecx")}
CONDS = ("z","nz","b","ae","be","a","s","ns","l","ge","le","g","o","no","p","np")
NOISE = ("mov esi, esi", "lea edi, [edi]", "mov edx, edx")


def inputs(rng, n=24):
    out = [(0,0),(0,1),(1,0),(0x7FFFFFFF,1),(0x80000000,1),(0xFFFFFFFF,1),
           (0x80,0xFF),(0x8000,0xFFFF),(0xFFFFFFFF,0xFFFFFFFF),(0x80000000,0x80000000)]
    out = out[:n]
    while len(out) < n:
        out.append((rng.choice(EDGES), rng.choice(EDGES)) if rng.random() < .8
                   else (rng.getrandbits(32), rng.getrandbits(32)))
    return out


def gen_case(rng, i):
    """One snippet. Ends leaving the interesting value in eax."""
    fam = rng.choice(("cmpset","arith","adcsbb","shift","incdec","ext","misc","imul","cmov","bit"))
    w = rng.choice((8,16,32)); lo, hi = WIDTHS[w]
    if fam == "cmpset":
        body = [f"{rng.choice(('cmp','test'))} {lo}, {hi}",
                f"set{rng.choice(CONDS)} dl", "movzx eax, dl"]
    elif fam == "arith":
        body = [f"{rng.choice(('add','sub','and','or','xor'))} {lo}, {hi}",
                f"set{rng.choice(CONDS)} dl", "movzx eax, dl"] if rng.random()<.5 else \
               [f"{rng.choice(('add','sub','and','or','xor'))} {lo}, {hi}"]
        if w != 32 and len(body) == 1: body.append("movzx eax, ax" if w==16 else "movzx eax, al")
    elif fam == "adcsbb":
        body = [f"{rng.choice(('add','sub','cmp'))} {lo}, {hi}",
                f"{rng.choice(('adc','sbb'))} {lo}, {hi}"]
        if w != 32: body.append("movzx eax, ax" if w==16 else "movzx eax, al")
    elif fam == "shift":
        op = rng.choice(("shl","shr","sar","rol","ror","rcl","rcr"))
        cnt = rng.choice((1,2,3,7,8,15,16,17,31))
        body = [f"{op} {lo}, {cnt}"]
        if w != 32: body.append("movzx eax, ax" if w==16 else "movzx eax, al")
    elif fam == "incdec":
        op = rng.choice(('inc','dec','neg','not'))
        body = [f"{op} {lo}"]
        # NOT leaves EFLAGS alone (SDM Vol 2B), so a setcc after it reads
        # whatever was there before the case: undefined in both models, and
        # the one thing the 22 Sep 2026 re-score still flagged (24 vectors,
        # one case, `not ax ; setnz dl`). Only the flag-writing three get a
        # consumer.
        if op != "not" and rng.random() < .5:
            body += [f"set{rng.choice(CONDS)} dl", "movzx eax, dl"]
        elif w != 32: body.append("movzx eax, ax" if w==16 else "movzx eax, al")
    elif fam == "ext":
        src = rng.choice(("al","cl","ax","cx"))
        body = [f"{rng.choice(('movzx','movsx'))} eax, {src}"]
    elif fam == "imul":
        body = ["imul eax, ecx"] if rng.random()<.5 else [f"imul eax, ecx, {rng.choice((2,3,7,255,65535))}"]
    elif fam == "cmov":
        body = ["cmp eax, ecx", f"cmov{rng.choice(CONDS)} eax, ecx"]
    elif fam == "bit":
        # BT/BTS/BTR/BTC define CF ONLY; OF, SF, ZF, AF and PF are
        # architecturally UNDEFINED after them (SDM Vol 2A).  Reading those
        # with a setcc compares two models' choices of undefined, not the
        # lifter against x86 -- 9 of the first run's 24 "mismatches" were
        # exactly that.  Restrict to the carry conditions, which are defined.
        body = [f"{rng.choice(('bt','bts','btr','btc'))} eax, ecx",
                f"set{rng.choice(('b','ae','c','nc','nae'))} dl", "movzx eax, dl"]
    else:
        body = [rng.choice(("bswap eax", "xchg eax, ecx", "cdq", "xadd eax, ecx"))]
    if rng.random() < .5 and len(body) > 1:
        body.insert(len(body)-1, rng.choice(NOISE))
    return {"name": f"f{i:04d}_{fam}_w{w}", "body": body, "inputs": inputs(rng)}


def assemble(body, workdir, tag):
    src = workdir / f"{tag}.s"
    src.write_text(".intel_syntax noprefix\n.text\n" + "\n".join(body) + "\n")
    obj = workdir / f"{tag}.o"
    r = subprocess.run(["clang","--target=i386-unknown-linux-gnu","-c",str(src),"-o",str(obj)],
                       capture_output=True, text=True)
    if r.returncode: return None, (r.stderr.strip().splitlines() or [""])[0]
    text = elf32_text(obj.read_bytes())
    if text is None: return None, "no .text in object"
    return text, None


def elf32_text(blob):
    """Pull .text out of a little-endian ELF32 object -- no objcopy on this host."""
    import struct
    if blob[:4] != b"\x7fELF" or blob[4] != 1: return None
    shoff, = struct.unpack_from("<I", blob, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", blob, 0x2E)
    def sh(i):
        o = shoff + i * shentsize
        name, typ, flags, addr, off, size = struct.unpack_from("<IIIIII", blob, o)
        return name, off, size
    _, stroff, _ = sh(shstrndx)
    for i in range(shnum):
        name, off, size = sh(i)
        end = blob.index(b"\0", stroff + name)
        if blob[stroff + name:end] == b".text":
            return blob[off:off + size]
    return None


def oracle(code, a, b):
    """Run the bytes on Unicorn's x86-32 core; return eax."""
    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    mu.mem_map(BASE, 0x1000, UC_PROT_ALL)
    mu.mem_map(STACK, 0x2000, UC_PROT_ALL)
    mu.mem_write(BASE, code)
    mu.reg_write(UC_X86_REG_EAX, a); mu.reg_write(UC_X86_REG_ECX, b)
    mu.reg_write(UC_X86_REG_EDX, 0)
    mu.reg_write(UC_X86_REG_ESI, 0); mu.reg_write(UC_X86_REG_EDI, 0)
    mu.reg_write(UC_X86_REG_ESP, STACK + 0x1000)
    mu.reg_write(UC_X86_REG_EFLAGS, 0x202)
    mu.emu_start(BASE, BASE + len(code))
    return mu.reg_read(UC_X86_REG_EAX) & 0xFFFFFFFF


def lift(code):
    from tools.recomp.disasm import BasicBlock, Disassembler
    from tools.recomp.lifter import Lifter, lift_basic_block
    d = Disassembler(); insns, mnem = [], []
    for insn in d._cs.disasm(code, BASE):
        mnem.append(insn.mnemonic); insns.append(d._decode_instruction(insn))
    from tools.recomp.translator import FunctionTranslator as FT
    lf = Lifter()
    # Use the TRANSLATOR's own predicates, not a guess: lifting with a
    # different needs_cf/needs_zf from production would test a path recomp
    # never takes, which is the same trap the conformance _lift docstring names.
    # getattr, because this runs against two trees: _function_needs_zf is a
    # later addition and upstream's translator does not have it. Falling back
    # to the mnemonic test rather than to False keeps adc/sbb lifting honest.
    needs_cf = getattr(FT, "_function_needs_cf", None)
    lf.needs_cf = (needs_cf(insns) if needs_cf
                   else any(i.mnemonic in ("sbb", "adc", "rcl", "rcr")
                            for i in insns))
    needs_zf = getattr(FT, "_function_needs_zf", None)
    if needs_zf:
        lf.needs_zf = needs_zf(insns)
    lines, _ = lift_basic_block(lf, BasicBlock(start=BASE, instructions=insns))
    return list(lines), mnem


PREAMBLE = """#define RECOMP_GENERATED_CODE 1
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "recomp_types.h"
RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi;
RECOMP_TLS uint32_t g_seh_ebp,g_ebp;
RECOMP_TLS int g_df;
RECOMP_TLS double g_fp_stack[8]; RECOMP_TLS int g_fp_top;
RECOMP_TLS uint16_t g_fp_control_word=0x027F; RECOMP_TLS int g_fp_cmp;
RECOMP_TLS RecompXmm g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7;
RECOMP_TLS RecompMmx g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7;
volatile uint32_t g_icall_trace[16]; volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;
ptrdiff_t g_xbox_mem_offset;
void recomp_icall_fail_log(uint32_t va){(void)va;}
void recomp_unimpl(const char *text, uint32_t va){(void)text;(void)va;}
static unsigned char g_guest_stack[64*1024];
"""


def build_c(cases):
    out = [PREAMBLE]
    for c in cases:
        out.append(f"static uint32_t {c['name']}(uint32_t a, uint32_t b) {{")
        out.append("    int _flags = 0; uint32_t _fa=0,_fb=0; int32_t _fas=0,_fbs=0;")
        out.append("    int _cf = 0, _zf = 0; double _fca=0.0,_fcb=0.0; uint32_t ebp = 0;")
        out.append("    (void)_flags;(void)_fa;(void)_fb;(void)_fas;(void)_fbs;")
        out.append("    (void)_cf;(void)_zf;(void)_fca;(void)_fcb;(void)ebp;")
        out.append("    g_eax=a; g_ecx=b; g_edx=0; g_esi=0; g_edi=0;")
        out.append("    g_esp=(uint32_t)(uintptr_t)(g_guest_stack+sizeof(g_guest_stack)-64);")
        for l in c["lines"]:
            out.append("    " + l)
        out.append("    return g_eax;\n}")
    out.append("int main(void){")
    for c in cases:
        for (a, b) in c["inputs"]:
            out.append(f'    printf("{c["name"]} %08X\\n", {c["name"]}(0x{a:08X}u,0x{b:08X}u));')
    out.append("    return 0;}")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=400)
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=7)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    rng = random.Random(a.seed)
    wd = Path(tempfile.mkdtemp(prefix="lifterfuzz-"))
    cases, skipped = [], []
    for i in range(a.count):
        c = gen_case(rng, i)
        code, err = assemble(c["body"], wd, c["name"])
        if code is None:
            skipped.append((c["name"], "asm: " + err)); continue
        c["code"] = code
        try:
            c["oracle"] = [oracle(code, x, y) for (x, y) in c["inputs"]]
        except Exception as e:
            skipped.append((c["name"], f"unicorn: {e}")); continue
        try:
            lines, mnem = lift(code)
        except Exception as e:
            skipped.append((c["name"], f"lift: {type(e).__name__}: {e}")); continue
        # An instruction the lifter cannot translate used to leave a bare
        # comment; since G22b it leaves a RECOMP_UNIMPL(...) call beside the
        # comment. Both are coverage loss, not a mismatch, so both skip.
        dropped = [l for l in lines if (l.strip().startswith("/*")
                                        and l.strip() != "/* nop */")
                   or "RECOMP_UNIMPL(" in l]
        if dropped:
            skipped.append((c["name"], "unlifted: " + "; ".join(d.strip() for d in dropped[:2])))
            continue
        c["lines"], c["mnem"] = lines, mnem
        cases.append(c)

    if not cases:
        print("no runnable cases"); print(skipped[:10]); return 1

    csrc = wd / "lifted.c"
    csrc.write_text(build_c(cases))
    exe = wd / "lifted"
    r = subprocess.run(["clang","-O1","-w","-std=c11",
                        f"-I{ROOT}/templates/runtime", str(csrc), "-o", str(exe),
                        "-lm"], capture_output=True, text=True)
    if r.returncode:
        print("COMPILE FAILED (first 40 lines):")
        print("\n".join((r.stderr or r.stdout).splitlines()[:40]))
        print("source kept at", csrc); return 2
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    got = {}
    for line in run.stdout.splitlines():
        n, v = line.split()
        got.setdefault(n, []).append(int(v, 16))

    mismatches = []
    for c in cases:
        ours = got.get(c["name"], [])
        for k, (inp, exp) in enumerate(zip(c["inputs"], c["oracle"])):
            if k >= len(ours): break
            if ours[k] != exp:
                mismatches.append((c, k, inp, exp, ours[k]))

    bad_cases = sorted({m[0]["name"] for m in mismatches})
    from collections import Counter
    fam_of = lambda n: n.split("_")[1]
    fams = Counter(fam_of(n) for n in bad_cases)
    print(f"seed=0x{a.seed:X} generated={a.count} ran={len(cases)} "
          f"skipped={len(skipped)} vectors={sum(len(c['inputs']) for c in cases)}")
    print(f"MISMATCHES: {len(mismatches)} across {len(bad_cases)} cases")
    print("  by family: " + ", ".join(f"{k}={v}" for k, v in fams.most_common()))
    seen = set()
    for c, k, inp, exp, gotv in mismatches:
        if c["name"] in seen: continue
        seen.add(c["name"])
        print(f"\n  {c['name']}  [{' ; '.join(c['body'])}]")
        print(f"    eax=0x{inp[0]:08X} ecx=0x{inp[1]:08X}  unicorn=0x{exp:08X}  lifted=0x{gotv:08X}")
        if a.verbose:
            for l in c["lines"]: print("      | " + l)
    if a.verbose and skipped:
        print("\n--- skipped ---")
        from collections import Counter
        for reason, n in Counter(s[1].split(":")[0] for s in skipped).most_common():
            print(f"  {n:4d}  {reason}")
    if a.keep: print("\nworkdir:", wd)
    return 0

sys.exit(main())
