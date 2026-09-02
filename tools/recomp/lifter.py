"""
x86 → C instruction lifter.

Translates individual x86 instructions (and common multi-instruction
patterns like cmp+jcc) into C statements using the recomp_types.h macros.

Register model:
  - eax, ebx, ecx, edx, esi, edi, ebp: uint32_t locals
  - esp: uint32_t local (stack pointer)
  - FPU: shared double g_fp_stack[8] with g_fp_top index

Memory model:
  - MEM8/MEM16/MEM32 macros for memory access at flat addresses
  - Xbox data sections mapped at original VAs
"""

import re
import struct

from .disasm import Instruction, Operand
from .config import is_code_address, is_data_address, va_to_file_offset


# ── Operand formatting ──────────────────────────────────────

def _fmt_reg(name, size=4):
    """Format a register name as a C expression."""
    if not name:
        return "0"

    # Segment registers → constants
    if name in ("fs", "gs", "cs", "ds", "es", "ss"):
        return f"0 /* seg:{name} */"

    # Map sub-registers to expressions on 32-bit locals
    SUB_REGS = {
        "al": "LO8(eax)", "ah": "HI8(eax)", "ax": "LO16(eax)",
        "bl": "LO8(ebx)", "bh": "HI8(ebx)", "bx": "LO16(ebx)",
        "cl": "LO8(ecx)", "ch": "HI8(ecx)", "cx": "LO16(ecx)",
        "dl": "LO8(edx)", "dh": "HI8(edx)", "dx": "LO16(edx)",
        "si": "LO16(esi)", "di": "LO16(edi)",
        "bp": "LO16(ebp)", "sp": "LO16(esp)",
    }
    if name in SUB_REGS:
        return SUB_REGS[name]
    return name


def _fmt_set_reg(name, value_expr):
    """Format assignment to a register, handling sub-register writes."""
    # Segment registers → no-op
    if name in ("fs", "gs", "cs", "ds", "es", "ss"):
        return f"/* mov {name}, {value_expr} - segment register */;"

    SET_MAP = {
        "al": f"SET_LO8(eax, {value_expr})",
        "ah": f"SET_HI8(eax, {value_expr})",
        "ax": f"SET_LO16(eax, {value_expr})",
        "bl": f"SET_LO8(ebx, {value_expr})",
        "bh": f"SET_HI8(ebx, {value_expr})",
        "bx": f"SET_LO16(ebx, {value_expr})",
        "cl": f"SET_LO8(ecx, {value_expr})",
        "ch": f"SET_HI8(ecx, {value_expr})",
        "cx": f"SET_LO16(ecx, {value_expr})",
        "dl": f"SET_LO8(edx, {value_expr})",
        "dh": f"SET_HI8(edx, {value_expr})",
        "dx": f"SET_LO16(edx, {value_expr})",
        "si": f"SET_LO16(esi, {value_expr})",
        "di": f"SET_LO16(edi, {value_expr})",
        "bp": f"SET_LO16(ebp, {value_expr})",
        "sp": f"SET_LO16(esp, {value_expr})",
    }
    if name in SET_MAP:
        return SET_MAP[name] + ";"
    return f"{name} = {value_expr};"


def _fmt_imm(val):
    """Format an immediate value as a C hex literal."""
    if val == 0:
        return "0"
    if val <= 9:
        return str(val)
    if val > 0x7FFFFFFF:
        return f"0x{val:08X}u"
    return f"0x{val:X}"


def _mem_accessor(size, segment=None):
    """Return the MEM macro name for a given operand size."""
    prefix = "FS_" if segment == "fs" else ""
    return prefix + {1: "MEM8", 2: "MEM16", 4: "MEM32"}.get(size, "MEM32")


def _smem_accessor(size, segment=None):
    """Return the signed MEM macro for a given operand size."""
    prefix = "FS_" if segment == "fs" else ""
    return prefix + {
        1: "SMEM8", 2: "SMEM16", 4: "SMEM32", 8: "SMEM64",
    }.get(size, "SMEM32")


def _fmt_fpu_operand(op):
    """Format an x87 memory or stack-register operand for reading."""
    if op.type == "mem":
        accessor = "MEMD" if op.mem_size == 8 else "MEMF"
        return f"{accessor}({_fmt_mem(op)})"
    if op.type == "reg":
        name = op.reg or ""
        if name == "st":
            return "fp_st(0)"
        if name.startswith("st(") and name.endswith(")"):
            return f"fp_st({name[3:-1]})"
    return None


def _fmt_mem(op):
    """Format a memory operand as a C expression (the address computation)."""
    parts = []
    if op.mem_base:
        parts.append(_fmt_reg(op.mem_base))
    if op.mem_index:
        idx = _fmt_reg(op.mem_index)
        if op.mem_scale and op.mem_scale > 1:
            parts.append(f"{idx} * {op.mem_scale}")
        else:
            parts.append(idx)
    if op.mem_disp:
        if op.mem_disp < 0:
            # Negative displacement - but we stored unsigned, check sign
            if op.mem_disp > 0x80000000:
                # Actually negative (two's complement)
                signed_disp = op.mem_disp - 0x100000000
                if parts:
                    parts.append(f"- {_fmt_imm(-signed_disp)}")
                else:
                    parts.append(_fmt_imm(op.mem_disp))
            else:
                parts.append(_fmt_imm(op.mem_disp))
        else:
            parts.append(_fmt_imm(op.mem_disp))
    if not parts:
        return "0"
    return " + ".join(parts)


def _fmt_mem_read(op):
    """Format reading from a memory operand."""
    accessor = _mem_accessor(op.mem_size, getattr(op, "mem_segment", None))
    addr = _fmt_mem(op)
    return f"{accessor}({addr})"


def _fmt_mem_write(op, value_expr):
    """Format writing to a memory operand."""
    accessor = _mem_accessor(op.mem_size, getattr(op, "mem_segment", None))
    addr = _fmt_mem(op)
    return f"{accessor}({addr}) = {value_expr};"


_REG_WIDTH = {
    "al": 1, "ah": 1, "bl": 1, "bh": 1, "cl": 1, "ch": 1, "dl": 1, "dh": 1,
    "ax": 2, "bx": 2, "cx": 2, "dx": 2, "si": 2, "di": 2, "bp": 2, "sp": 2,
}


def _operand_width(op):
    """Byte width of an operand, or None when it carries no width of its own.

    Memory operands know their size; registers imply it by name; immediates do
    not have one and take it from the other operand. Getting this wrong makes a
    narrow compare test a widened value - "cmp word ptr [x], -1" against
    0xFFFFFFFF never matches, because a 16-bit -1 is 0xFFFF.
    """
    if op.type == "mem":
        return getattr(op, "mem_size", None) or 4
    if op.type == "reg":
        return _REG_WIDTH.get(str(op.reg).lower(), 4)
    return None


def _fmt_operand_read(op):
    """Format reading any operand type."""
    if op.type == "reg":
        return _fmt_reg(op.reg)
    elif op.type == "imm":
        return _fmt_imm(op.imm)
    elif op.type == "mem":
        return _fmt_mem_read(op)
    return "/* unknown operand */"


def _is_mmx_operand(op):
    """An MMX register operand (mm0..mm7), not an XMM one."""
    return (op.type == "reg" and op.reg
            and op.reg.startswith("mm") and not op.reg.startswith("xmm"))


def _has_mmx_operand(ops):
    return any(_is_mmx_operand(o) for o in ops)


def _fmt_operand_write(op, value_expr):
    """Format writing to any operand type. Returns a C statement."""
    if op.type == "reg":
        return _fmt_set_reg(op.reg, value_expr)
    elif op.type == "mem":
        return _fmt_mem_write(op, value_expr)
    return f"/* cannot write to {op.type} */;"


# ── Condition code mapping ───────────────────────────────────

# Maps jcc mnemonic → (cmp_macro, test_macro, description)
# cmp_macro takes (lhs, rhs), test_macro takes (lhs, rhs)
COND_MAP = {
    "je":   ("CMP_EQ",  "TEST_Z",  "equal / zero"),
    "jz":   ("CMP_EQ",  "TEST_Z",  "zero"),
    "jne":  ("CMP_NE",  "TEST_NZ", "not equal / not zero"),
    "jnz":  ("CMP_NE",  "TEST_NZ", "not zero"),
    "jb":   ("CMP_B",   None,      "below (unsigned <)"),
    "jnae": ("CMP_B",   None,      "below"),
    "jae":  ("CMP_AE",  None,      "above or equal (unsigned >=)"),
    "jnb":  ("CMP_AE",  None,      "above or equal"),
    "jbe":  ("CMP_BE",  None,      "below or equal (unsigned <=)"),
    "jna":  ("CMP_BE",  None,      "below or equal"),
    "ja":   ("CMP_A",   None,      "above (unsigned >)"),
    "jl":   ("CMP_L",   "TEST_S",  "less (signed <)"),
    "jge":  ("CMP_GE",  None,      "greater or equal (signed >=)"),
    "jle":  ("CMP_LE",  None,      "less or equal (signed <=)"),
    "jg":   ("CMP_G",   None,      "greater (signed >)"),
    "js":   (None,       "TEST_S",  "sign (negative)"),
    "jns":  (None,       None,      "not sign (positive)"),
    "jo":   (None,       None,      "overflow"),
    "jno":  (None,       None,      "not overflow"),
    "jp":   (None,       None,      "parity"),
    "jnp":  (None,       None,      "not parity"),
    "jecxz": (None,      None,      "ecx is zero"),
    "jcxz":  (None,      None,      "cx is zero"),
}

# Instructions that set arithmetic flags (primary set, fully handled)
FLAG_SETTERS = frozenset({
    "cmp", "test", "sub", "add", "and", "or", "xor",
    "inc", "dec", "neg", "shl", "shr", "sar", "imul", "adc", "sbb",
    "comiss", "comisd", "ucomiss", "ucomisd",  # SSE float compare
})

# Additional instructions that modify EFLAGS (tracked but handled as generic)
_EFLAGS_SETTERS = frozenset({
    "shld", "shrd", "rol", "ror", "rcl", "rcr",  # Shifts/rotates set CF
    "bsf", "bsr",       # Bit scan sets ZF
    "bt", "bts", "btr", "btc",  # Bit test sets CF
    "cmpxchg", "lock cmpxchg",   # Compare-and-exchange sets ZF
    "xadd",              # Exchange-and-add sets flags
    "lock xadd",         # LOCK changes atomicity, not arithmetic flags
})

# Instructions with undefined/unpredictable flags (clear tracking)
_FLAGS_UNDEFINED = frozenset({
    "mul", "div", "idiv",  # Flags partially undefined
    "rdtsc", "cpuid",      # Special instructions
})

# Instructions that do NOT modify EFLAGS (preserve flag tracking)
_EFLAGS_PRESERVE = frozenset({
    # General-purpose data movement / stack
    "mov", "lea", "push", "pop", "nop", "leave", "ret",
    "movzx", "movsx", "xchg", "bswap",
    "cdq", "cwde", "cbw", "cwd",
    "lahf",
    "not",  # NOT does not modify flags
    "call",
    "int3", "int", "wait",
    "cld", "std", "cli", "sti",
    "pushfd", "popfd", "pushal",
    "sgdt", "ljmp", "sfence",
    # SSE scalar float
    "movss", "movsd",
    "addss", "subss", "mulss", "divss",
    "minss", "maxss", "sqrtss", "rsqrtss", "rcpss",
    "addsd", "subsd", "mulsd", "divsd",
    "minsd", "maxsd", "sqrtsd",
    "cvtsi2ss", "cvtss2si", "cvttss2si",
    "cvtsi2sd", "cvtsd2si", "cvttsd2si",
    "cvtss2sd", "cvtsd2ss",
    "cmpss", "cmpsd",
    "cmpltss", "cmpeqss", "cmpleps", "cmpneqss",
    # SSE packed float
    "movaps", "movups", "movlps", "movhps", "movlhps", "movhlps",
    "addps", "subps", "mulps", "divps",
    "minps", "maxps", "sqrtps", "rsqrtps", "rcpps",
    "shufps", "unpcklps", "unpckhps",
    "andps", "orps", "xorps", "andnps",
    "cmpps", "cmpneqps",
    "movmskps",
    # SSE2 packed double
    "movapd", "movupd",
    "addpd", "subpd", "mulpd", "divpd",
    # SSE/MMX integer
    "movd", "movq", "movntq",
    "emms",
    "paddb", "paddw", "paddd", "paddq",
    "psubb", "psubw", "psubd",
    "pmullw", "pmulhw", "pmulhuw", "pmaddwd",
    "pand", "pandn", "por", "pxor",
    "pcmpeqb", "pcmpeqw", "pcmpeqd",
    "pcmpgtb", "pcmpgtw", "pcmpgtd",
    "psllw", "pslld", "psllq",
    "psrlw", "psrld", "psrlq",
    "psraw", "psrad",
    "pshufw", "pshufd", "pshufhw", "pshuflw",
    "punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq",
    "punpckhbw", "punpckhwd", "punpckhdq", "punpckhqdq",
    "packsswb", "packssdw", "packuswb",
    "pmovmskb",
    # String operations (without rep prefix)
    "stosb", "stosw", "stosd",
    "movsb", "movsw", "movsd",
    "lodsb", "lodsw", "lodsd",
    # Prefetch hints
    "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2",
})


def _make_condition(jcc, flag_setter, flag_ops):
    """
    Generate a C condition expression for a jcc based on what set the flags.
    Returns (cond_expr, description) or None.
    """
    cond_info = COND_MAP.get(jcc)
    if not cond_info:
        return None
    cmp_macro, test_macro, desc = cond_info

    # A cmp/test that is not fused with its jcc snapshots its operands into
    # _fa/_fb (zero-extended) and _fas/_fbs (sign-extended) at the point the
    # comparison happens. Use those rather than re-reading registers that may
    # since have changed.
    SIGNED = {"CMP_L", "CMP_LE", "CMP_G", "CMP_GE", "TEST_S"}
    if flag_setter in ("cmp", "test") and len(flag_ops) >= 2:
        signed = (cmp_macro in SIGNED) or (test_macro in SIGNED)
        lhs, rhs = ("_fas", "_fbs") if signed else ("_fa", "_fb")
    elif len(flag_ops) >= 2:
        lhs = _fmt_operand_read(flag_ops[0])
        rhs = _fmt_operand_read(flag_ops[1])
    elif len(flag_ops) == 1:
        lhs = _fmt_operand_read(flag_ops[0])
        rhs = None
    else:
        lhs = None
        rhs = None

    # ── FPU compare-to-EFLAGS and sahf: no standard operands ──
    if flag_setter in ("fcompi", "fcomip", "fucomi", "fucompi",
                        "fucomip", "fcomi", "sahf"):
        fpu_cmp_map = {
            "ja": ">", "jnbe": ">",
            "jae": ">=", "jnb": ">=", "jnc": ">=",
            "jb": "<", "jnae": "<", "jc": "<",
            "jbe": "<=", "jna": "<=",
            "je": "==", "jz": "==",
            "jne": "!=", "jnz": "!=",
        }
        op = fpu_cmp_map.get(jcc)
        if op:
            return f"(g_fp_cmp {op} 0) /* {flag_setter} */", desc
        if jcc == "jp":
            return "0 /* fpu: unordered/NaN */", desc
        if jcc == "jnp":
            return "1 /* fpu: ordered */", desc
        return None

    # If no operands available for other flag-setters, can't generate condition
    if lhs is None:
        return None

    # ── comiss/ucomiss: float comparison, sets CF/ZF/PF ──
    if flag_setter in ("comiss", "comisd", "ucomiss", "ucomisd"):
        def _sse_op(op):
            if op.type == "reg" and op.reg and op.reg.startswith("xmm"):
                return f"{op.reg}.f[0]"
            elif op.type == "reg":
                return op.reg
            elif op.type == "mem":
                if op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)})"
                return f"MEMF({_fmt_mem(op)})"
            return _fmt_operand_read(op)
        a = _sse_op(flag_ops[0]) if len(flag_ops) >= 1 else "0.0f"
        b = _sse_op(flag_ops[1]) if len(flag_ops) >= 2 else "0.0f"
        # comiss uses unsigned condition codes (CF, ZF)
        if jcc in ("ja", "jnbe"):
            return f"({a} > {b})", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"({a} >= {b})", desc
        if jcc in ("jb", "jnae", "jc"):
            return f"({a} < {b})", desc
        if jcc in ("jbe", "jna"):
            return f"({a} <= {b})", desc
        if jcc in ("je", "jz"):
            return f"({a} == {b})", desc
        if jcc in ("jne", "jnz"):
            return f"({a} != {b})", desc
        if jcc == "jp":
            return f"0 /* {jcc}: unordered/NaN */", desc
        if jcc == "jnp":
            return f"1 /* {jcc}: ordered */", desc
        return None

    # SF is the sign bit of the result at the OPERAND's width, not at 32 bits.
    # `test dl, dl; jns` asks about bit 7; evaluating the zero-extended byte as
    # an int32 makes 0x80..0xFF look positive and the branch always goes the
    # same way. Same defect the signed compares had before the width-aware
    # CMP_L/CMP_G landed -- js/jns were simply missed at the time.
    _sf_width = _operand_width(flag_ops[0]) if flag_ops else None
    if _sf_width is None and len(flag_ops) > 1:
        _sf_width = _operand_width(flag_ops[1])
    _sf_cast = {1: "(int8_t)", 2: "(int16_t)"}.get(_sf_width, "(int32_t)")

    # ── cmp: flags from (a - b), operands unchanged ──
    if flag_setter == "cmp":
        if cmp_macro:
            return f"{cmp_macro}({lhs}, {rhs})", desc
        if jcc == "js":
            return f"({_sf_cast}(({lhs}) - ({rhs})) < 0)", desc
        if jcc == "jns":
            return f"({_sf_cast}(({lhs}) - ({rhs})) >= 0)", desc
        if jcc == "jp":
            return f"RECOMP_PARITY8(({lhs}) - ({rhs}))", desc
        if jcc == "jnp":
            return f"(!RECOMP_PARITY8(({lhs}) - ({rhs})))", desc
        return None

    # ── test: flags from (a & b), operands unchanged ──
    if flag_setter == "test":
        if test_macro:
            return f"{test_macro}({lhs}, {rhs})", desc
        if cmp_macro:
            return f"{cmp_macro}({lhs} & {rhs}, 0)", desc
        if jcc == "js":
            return f"({_sf_cast}(({lhs}) & ({rhs})) < 0)", desc
        if jcc == "jns":
            return f"({_sf_cast}(({lhs}) & ({rhs})) >= 0)", desc
        if jcc == "jo":
            return "0", desc  # OF=0 after test
        if jcc == "jno":
            return "1", desc
        if jcc == "jp":
            # PF from (a & b) low byte. This is the x87 float branch idiom
            # `fnstsw ax; test ah, mask; jp/jnp` (fnstsw put the compare bits
            # into ah). jp jumps on even parity (PF=1).
            return f"RECOMP_PARITY8(({lhs}) & ({rhs}))", desc
        if jcc == "jnp":
            return f"(!RECOMP_PARITY8(({lhs}) & ({rhs})))", desc
        return None

    # ── sub: a = a - b, flags from (a_orig - b) ──
    if flag_setter == "sub":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        # Ordered: reconstruct original a = result + b
        if cmp_macro and rhs:
            return f"{cmp_macro}((uint32_t){lhs} + (uint32_t){rhs}, (uint32_t){rhs})", desc
        if jcc in ("jb", "jnae"):
            return f"((uint32_t){lhs} + (uint32_t){rhs} < (uint32_t){rhs})", desc
        if jcc in ("jae", "jnb"):
            return f"((uint32_t){lhs} + (uint32_t){rhs} >= (uint32_t){rhs})", desc
        if jcc in ("jl", "jnge"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jge", "jnl"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jle", "jng"):
            return f"((int32_t){lhs} <= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"((int32_t){lhs} > 0)", desc
        return None

    # ── add: a = a + b, flags from result ──
    if flag_setter == "add":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jb", "jnae", "jc"):
            return f"({lhs} < (uint32_t){rhs})", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"({lhs} >= (uint32_t){rhs})", desc
        if jcc in ("jl", "jnge"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jge", "jnl"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jle", "jng"):
            return f"((int32_t){lhs} <= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"((int32_t){lhs} > 0)", desc
        return None

    # ── adc/sbb: result-based (like add/sub but with carry) ──
    if flag_setter in ("adc", "sbb"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        return None

    # ── and/or/xor: result-based, CF=0, OF=0 ──
    if flag_setter in ("and", "or", "xor"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc in ("js", "jl"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jns", "jge"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc == "jle":
            return f"((int32_t){lhs} <= 0)", desc
        if jcc == "jg":
            return f"((int32_t){lhs} > 0)", desc
        if jcc in ("jb", "jnae", "jbe", "jna"):
            return "0", desc  # CF=0 after and/or/xor
        if jcc in ("jae", "jnb", "ja", "jnbe"):
            return "1", desc
        return None

    # ── dec/inc: result-based, CF unchanged ──
    if flag_setter in ("dec", "inc"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jl", "jle", "jg", "jge"):
            cast = "(int32_t)" + lhs
            op = {"jl": "<", "jle": "<=", "jg": ">", "jge": ">="}[jcc]
            return f"({cast} {op} 0)", desc
        return None

    # ── neg: flags from (0 - a_orig), result is -a ──
    if flag_setter == "neg":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc in ("jb", "jnae", "jc"):
            # CF=1 unless original was 0
            return f"({lhs} != 0)", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"({lhs} == 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"((int32_t){lhs} > 0)", desc
        if jcc in ("jge", "jnl"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jl", "jnge"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jle", "jng"):
            return f"((int32_t){lhs} <= 0)", desc
        return None

    # ── shift: result-based ──
    if flag_setter in ("shl", "shr", "sar"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        return None

    # ── shld/shrd: double-precision shift, result-based ──
    if flag_setter in ("shld", "shrd"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        return None

    # ── rol/ror/rcl/rcr: rotation, only CF/OF affected ──
    if flag_setter in ("rol", "ror", "rcl", "rcr"):
        # ZF/SF not modified by rotations - can't resolve most conditions
        return None

    # ── bsf/bsr: bit scan, ZF set if source is zero ──
    if flag_setter in ("bsf", "bsr"):
        if jcc in ("je", "jz"):
            return "_flags", desc
        if jcc in ("jne", "jnz"):
            return "!_flags", desc
        return None

    # ── bt/bts/btr/btc: bit test, sets CF ──
    if flag_setter in ("bt", "bts", "btr", "btc"):
        if rhs is None:
            return None
        if jcc in ("jb", "jnae", "jc"):
            return f"(({lhs} >> ({rhs} & 31)) & 1)", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"!(({lhs} >> ({rhs} & 31)) & 1)", desc
        return None

    # ── cmpxchg: ZF is "the exchange happened" ──
    #
    # This used to answer `({lhs} == eax)`, which reads the operands AFTER the
    # instruction has already written them: on success the destination now
    # holds the source, so the comparison is against the wrong value. The lift
    # records the outcome in _flags instead, the way the string compares do.
    #
    # Only je/jne are answered. cmpxchg also sets CF/SF/OF/AF/PF from the
    # comparison and those are not modelled, so anything else falls through to
    # the generic path rather than being answered wrongly.
    if flag_setter in ("cmpxchg", "lock cmpxchg"):
        if jcc in ("je", "jz"):
            return "(_flags != 0)", desc
        if jcc in ("jne", "jnz"):
            return "(_flags == 0)", desc
        return None

    # ── xadd: exchange and add, flags from addition ──
    if flag_setter in ("xadd", "lock xadd"):
        flag = {
            "je": "RECOMP_EFLAGS_ZF(_flags)",
            "jz": "RECOMP_EFLAGS_ZF(_flags)",
            "jne": "!RECOMP_EFLAGS_ZF(_flags)",
            "jnz": "!RECOMP_EFLAGS_ZF(_flags)",
            "jb": "RECOMP_EFLAGS_CF(_flags)",
            "jnae": "RECOMP_EFLAGS_CF(_flags)",
            "jae": "!RECOMP_EFLAGS_CF(_flags)",
            "jnb": "!RECOMP_EFLAGS_CF(_flags)",
            "jbe": "(RECOMP_EFLAGS_CF(_flags) || RECOMP_EFLAGS_ZF(_flags))",
            "jna": "(RECOMP_EFLAGS_CF(_flags) || RECOMP_EFLAGS_ZF(_flags))",
            "ja": "(!RECOMP_EFLAGS_CF(_flags) && !RECOMP_EFLAGS_ZF(_flags))",
            "js": "RECOMP_EFLAGS_SF(_flags)",
            "jns": "!RECOMP_EFLAGS_SF(_flags)",
            "jo": "RECOMP_EFLAGS_OF(_flags)",
            "jno": "!RECOMP_EFLAGS_OF(_flags)",
            "jp": "RECOMP_EFLAGS_PF(_flags)",
            "jnp": "!RECOMP_EFLAGS_PF(_flags)",
            "jl": "(RECOMP_EFLAGS_SF(_flags) != RECOMP_EFLAGS_OF(_flags))",
            "jge": "(RECOMP_EFLAGS_SF(_flags) == RECOMP_EFLAGS_OF(_flags))",
            "jle": "(RECOMP_EFLAGS_ZF(_flags) || (RECOMP_EFLAGS_SF(_flags) != RECOMP_EFLAGS_OF(_flags)))",
            "jg": "(!RECOMP_EFLAGS_ZF(_flags) && (RECOMP_EFLAGS_SF(_flags) == RECOMP_EFLAGS_OF(_flags)))",
        }.get(jcc)
        if flag:
            return flag, desc
        return None

    # ── repe cmpsb / repne scasb: string comparison ──
    if "cmps" in flag_setter or "scas" in flag_setter:
        if jcc in ("je", "jz"):
            return "(_flags != 0)", desc
        if jcc in ("jne", "jnz"):
            return "(_flags == 0)", desc
        return None

    return None


def _make_setcc_value(setcc_mnemonic, flag_setter, flag_ops):
    """Generate the condition expression for a SETcc instruction."""
    cc = setcc_mnemonic[3:]
    jcc = "j" + cc
    result = _make_condition(jcc, flag_setter, flag_ops)
    if result:
        return result[0]
    return None


def _make_cmovcc_cond(cmov_mnemonic, flag_setter, flag_ops):
    """Generate the condition expression for a CMOVcc instruction."""
    cc = cmov_mnemonic[4:]
    jcc = "j" + cc
    result = _make_condition(jcc, flag_setter, flag_ops)
    if result:
        return result[0]
    return None


# ── Pattern matching for flag-setter + jcc ────────────────────

def _emit_cond_goto(cond_expr, jcc, desc, target, lifter):
    """Emit a conditional goto or call for a jump target."""
    if target is None:
        return f"if ({cond_expr}) {{ /* {jcc}: {desc} - indirect */ }}"
    if lifter and lifter._is_external_target(target):
        # Conditional tail call: same frame bridge as the unconditional tail
        # jmp in _lift_jmp, applied only on the taken path.
        if target in lifter.manual_functions:
            return (f"if ({cond_expr}) {{ g_seh_ebp = ebp; "
                    f"RECOMP_ITAIL(0x{target:08X}u); return; }}"
                    f" /* {jcc}: {desc}, manual tail */")
        name = lifter._call_target_name(target)
        return (f"if ({cond_expr}) {{ g_seh_ebp = ebp; {name}(); return; }}"
                f" /* {jcc}: {desc} */")
    return f"if ({cond_expr}) goto loc_{target:08X}; /* {jcc}: {desc} */"


def try_match_cmp_jcc(insns, idx, lifter=None):
    """
    Try to match a cmp/test + jcc pattern starting at insns[idx].
    Returns (c_statement, num_consumed) or None.
    """
    if idx + 1 >= len(insns):
        return None

    first = insns[idx]
    second = insns[idx + 1]

    if first.mnemonic not in ("cmp", "test") or not second.is_cond_jump:
        return None

    if len(first.operands) < 2:
        return None

    result = _make_condition(second.mnemonic, first.mnemonic, first.operands)
    if not result:
        return None

    cond_expr, desc = result
    target = second.jump_target
    stmt = _emit_cond_goto(cond_expr, second.mnemonic, desc, target, lifter)
    return (stmt, 2)


# ── Single instruction lifting ───────────────────────────────

# MSVC's __SEH_prolog establishes the caller's frame pointer, so the lifter has
# to know which function it is. The address is per-title, and hardcoding it
# meant every other game silently got no frame set up after the call: ebp kept
# whatever stale value it had, and the first ebp-relative local access read
# through it. In Halo that surfaced as a read of 0xFFFFFFFC (ebp=0, [ebp-4]).
#
# Both helpers are compiler boilerplate with distinctive bodies, so detect them
# rather than asking every project to look them up by hand.
#
#   __SEH_prolog   mov eax, fs:[0]        64 A1 00 00 00 00
#                  lea ebp, [esp+0x10]    8D 6C 24 10
#   __SEH_epilog   mov fs:[0], ecx        64 89 0D 00 00 00 00
#                  leave; push ecx; ret   C9 51 C3
_SEH_PROLOG_MARKERS = (b"\x64\xa1\x00\x00\x00\x00", b"\x8d\x6c\x24\x10")
_SEH_EPILOG_MARKERS = (b"\x64\x89\x0d\x00\x00\x00\x00", b"\xc9\x51\xc3")

# Both are tiny; a large match is something else that happens to touch fs:[0].
_SEH_PROLOG_MAX_SIZE = 128
_SEH_EPILOG_MAX_SIZE = 64


def detect_seh_helpers(func_db, xbe_data, verbose=False):
    """Locate __SEH_prolog / __SEH_epilog in the target binary.

    Returns (prolog_addr, epilog_addr); either may be None if not found, which
    is normal for a title whose CRT does not use them.
    """
    from .config import va_to_file_offset

    prolog = epilog = None

    def _size_of(info):
        # "end" is a hex string in functions.json but BatchTranslator rewrites
        # it to an int in place, so accept either.
        try:
            size = int(info.get("size") or 0)
        except (TypeError, ValueError):
            size = 0
        if size:
            return size
        end = info.get("end")
        if isinstance(end, str):
            try:
                end = int(end, 16)
            except ValueError:
                return 0
        return (end - addr) if isinstance(end, int) else 0

    for addr in sorted(func_db):
        info = func_db[addr]
        size = _size_of(info)
        if size <= 0 or size > _SEH_PROLOG_MAX_SIZE:
            continue

        offset = va_to_file_offset(addr)
        if offset is None or xbe_data is None or offset + size > len(xbe_data):
            continue
        body = xbe_data[offset:offset + size]

        if (prolog is None and size <= _SEH_PROLOG_MAX_SIZE
                and all(m in body for m in _SEH_PROLOG_MARKERS)):
            prolog = addr
        elif (epilog is None and size <= _SEH_EPILOG_MAX_SIZE
                and all(m in body for m in _SEH_EPILOG_MARKERS)):
            epilog = addr

        if prolog is not None and epilog is not None:
            break

    if verbose:
        import sys
        fmt = lambda a: f"0x{a:08X}" if a else "not found"
        print(f"  SEH helpers: __SEH_prolog {fmt(prolog)}, "
              f"__SEH_epilog {fmt(epilog)}", file=sys.stderr)

    return prolog, epilog


class Lifter:
    """Translates x86 instructions to C statements."""

    def __init__(self, func_db=None, label_db=None, abi_db=None, xbe_data=None,
                 seh_prolog=None, seh_epilog=None, manual_functions=None):
        """
        func_db: dict of func_addr → func_info (for naming call targets)
        label_db: dict of addr → name (for kernel imports, etc.)
        abi_db: dict of addr → ABI info (for calling conventions)
        xbe_data: raw XBE file bytes (for reading jump tables)
        seh_prolog/seh_epilog: override the detected __SEH_prolog/__SEH_epilog
        manual_functions: addresses replaced through recomp_lookup_manual
        """
        self.func_db = func_db or {}
        self.label_db = label_db or {}
        self.abi_db = abi_db or {}
        self.xbe_data = xbe_data
        self.manual_functions = set(manual_functions or ())
        self._fp_top = 0  # FPU stack top index
        self.func_start = 0  # Set per-function by translator
        self.func_end = 0
        self.needs_flags = True  # Translator disables snapshots with no consumer
        self.needs_cf = False  # Set per-function by translator (has adc/sbb)
        self.publishes_ebp = False  # Set per-function: has a real frame
        self.trace_exit_name = None  # Set per-function when traced
        # Every direct call target we emit a name for, as {addr: name}. The
        # batch translator diffs this against the functions it actually defined
        # so it can stub out the remainder (see translate_batch_split).
        self.referenced_calls = {}

        # Detect if either is missing, so overriding one does not silently
        # leave the other unset -- that is the bug this whole path fixes.
        if (seh_prolog is None or seh_epilog is None) and self.func_db:
            found_prolog, found_epilog = detect_seh_helpers(self.func_db, xbe_data)
            seh_prolog = seh_prolog if seh_prolog is not None else found_prolog
            seh_epilog = seh_epilog if seh_epilog is not None else found_epilog
        self.SEH_PROLOG = seh_prolog
        self.SEH_EPILOG = seh_epilog
        self.jump_table_targets = {}

    def _call_target_name(self, addr):
        """Get the name for a call target address.

        func_db wins over label_db. The function definition is emitted from
        func_db, so consulting labels first meant a renamed function was
        *defined* as cseries__sub_0008DB80 but *called* as sub_0008DB80 -- the
        disassembler's generic auto-label -- and the link failed on every
        function any naming pass had touched. Labels still cover call targets
        that are not known function starts.
        """
        if addr in self.func_db:
            name = self.func_db[addr].get("name", f"sub_{addr:08X}")
        elif addr in self.label_db:
            name = self.label_db[addr]
        else:
            name = f"sub_{addr:08X}"
        self.referenced_calls[addr] = name
        return name

    def lift_instruction(self, insn):
        """
        Translate a single x86 instruction to one or more C statements.
        Returns a list of C statement strings.
        """
        m = insn.mnemonic
        ops = insn.operands
        nops = len(ops)

        # ── NOP ──
        if m == "nop" or (m == "lea" and nops == 2 and
                          ops[0].type == "reg" and ops[1].type == "mem" and
                          ops[1].mem_base == ops[0].reg and
                          not ops[1].mem_index and ops[1].mem_disp == 0):
            return [f"/* nop */"]

        # ── Data movement ──
        if m == "mov":
            return self._lift_mov(insn, ops)
        if m == "movzx":
            return self._lift_movzx(insn, ops)
        if m == "movsx":
            return self._lift_movsx(insn, ops)
        if m == "lea":
            return self._lift_lea(insn, ops)
        if m == "xchg":
            return self._lift_xchg(insn, ops)
        if m in ("emms", "femms"):
            # Clears MMX/x87 tag state. We do not model the aliasing, so there
            # is nothing to clear -- but say so rather than emit a bare TODO.
            return [f"/* {m}: MMX state not aliased with x87 here */"]
        if m in ("movq", "movntq") and nops >= 2 and _has_mmx_operand(ops):
            return self._lift_mmx_move(insn, ops, nontemporal=(m == "movntq"))

        if m in ("xadd", "lock xadd"):
            return self._lift_xadd(insn, ops, locked=(m == "lock xadd"))
        if m in ("cmpxchg", "lock cmpxchg"):
            return self._lift_cmpxchg(insn, ops,
                                      locked=(m == "lock cmpxchg"))

        # ── Stack ──
        if m == "push":
            return self._lift_push(insn, ops)
        if m == "pop":
            return self._lift_pop(insn, ops)

        # ── Arithmetic ──
        if m in ("add", "sub", "and", "or", "xor"):
            return self._lift_alu_binop(insn, ops, m)
        if m in ("inc", "dec"):
            return self._lift_inc_dec(insn, ops, m)
        if m == "neg":
            return self._lift_neg(insn, ops)
        if m == "not":
            return self._lift_not(insn, ops)
        if m == "imul":
            return self._lift_imul(insn, ops)
        if m in ("mul", "div", "idiv"):
            return self._lift_muldiv(insn, ops, m)
        if m == "sbb":
            return self._lift_sbb(insn, ops)
        if m == "adc":
            return self._lift_adc(insn, ops)
        if m in ("shl", "sal"):
            return self._lift_shift(insn, ops, "<<")
        if m == "shr":
            return self._lift_shift(insn, ops, ">>")
        if m == "sar":
            return self._lift_sar(insn, ops)
        if m in ("rol", "ror"):
            return self._lift_rotate(insn, ops, m)
        if m in ("bsf", "bsr"):
            return self._lift_bit_scan(ops, m)

        # ── Comparison / test (standalone, not part of cmp+jcc pattern) ──
        if m == "cmp":
            return self._lift_cmp(insn, ops)
        if m == "test":
            return self._lift_test(insn, ops)

        # ── Control flow ──
        if m == "call":
            return self._lift_call(insn, ops)
        if m in ("ret", "retn", "retf"):
            return self._lift_ret(insn, ops)
        if m == "jmp":
            return self._lift_jmp(insn, ops)
        if insn.is_cond_jump:
            return self._lift_jcc(insn)

        # ── String operations ──
        if m.startswith("rep ") or m.startswith("repe ") or m.startswith("repne "):
            return self._lift_rep_string(insn, m)
        if m in ("movsb", "movsd", "movsw", "stosb", "stosd", "stosw",
                 "lodsb", "lodsd", "lodsw"):
            return self._lift_string_op(insn, m)
        if m == "wait":
            return ["/* wait - FPU sync */"]

        # ── Misc ──
        if m == "cdq":
            return ["edx = ((int32_t)eax < 0) ? 0xFFFFFFFF : 0; /* cdq */"]
        if m == "cwde":
            return ["eax = SX16(eax); /* cwde */"]
        if m == "cbw":
            return ["SET_LO16(eax, SX8(eax)); /* cbw */"]
        if m == "bswap" and nops >= 1 and ops[0].type == "reg":
            r = _fmt_reg(ops[0].reg)
            return [f"{r} = BSWAP32({r}); /* bswap */"]
        # ── Bit test and modify ──
        # 386 instructions, so real Xbox code has them. The CRT's float-to-int
        # helper uses `btr` to clear a rounding-control bit of the x87 control
        # word, which is exactly the _control87 shape. Unhandled, they lifted to
        # a comment and the bit was silently left alone.
        if m in ("bt", "btr", "bts", "btc") and nops >= 2:
            dst, bit = ops[0], ops[1]
            index = (f"({_fmt_imm(bit.imm)})" if bit.type == "imm"
                     else f"({_fmt_operand_read(bit)} & 31)")
            value = _fmt_operand_read(dst)
            out = []
            if self.needs_cf:
                out.append(f"_cf = (int)(({value} >> {index}) & 1u);"
                           f" /* {m}: CF = bit */")
            update = {"btr": f"{value} & ~(1u << {index})",
                      "bts": f"{value} | (1u << {index})",
                      "btc": f"{value} ^ (1u << {index})"}.get(m)
            if update:
                out.append(_fmt_operand_write(dst, f"({update})")
                           + f" /* {m} */")
            elif not out:
                out.append(f"/* bt {insn.op_str}: no CF consumer */")
            return out

        if m == "int3":
            return ["__debugbreak(); /* int3 */"]
        if m in ("leave",):
            return ["esp = ebp;", "POP32(esp, ebp); /* leave */"]
        if m in ("cld", "std"):
            return [f"/* {m} - direction flag */"]
        if m == "lahf":
            return ["/* lahf - load AH from flags (used in FPU compare idiom) */"]
        if m == "sahf":
            return ["/* sahf - store AH to flags */"]
        if m == "shld":
            return self._lift_shld(insn, ops)
        if m == "shrd":
            return self._lift_shrd(insn, ops)
        if m == "bt":
            if len(ops) >= 2:
                return [f"/* bt {_fmt_operand_read(ops[0])}, {_fmt_operand_read(ops[1])} - bit test */"]
            return [f"/* bt {insn.op_str} */"]
        if m == "emms":
            return ["/* emms - empty MMX state */"]
        if m in ("xlat", "xlatb"):
            # AL indexes the byte table at EBX. With an address-size override,
            # the effective offset is calculated and wrapped at 16 bits.
            raw_bytes = bytes.fromhex(insn.bytes_hex)
            if 0x67 in raw_bytes[:-1]:
                address = "(uint16_t)(LO16(ebx) + LO8(eax))"
            else:
                address = "ebx + LO8(eax)"
            return [f"SET_LO8(eax, MEM8({address})); /* xlatb */"]
        if m in ("sete", "setne", "setb", "setae", "setbe", "seta",
                 "setl", "setge", "setle", "setg", "sets", "setns"):
            return self._lift_setcc(insn, ops, m)
        if m in ("cmove", "cmovne", "cmovb", "cmovae", "cmovbe", "cmova",
                 "cmovl", "cmovge", "cmovle", "cmovg", "cmovs", "cmovns"):
            return self._lift_cmovcc(insn, ops, m)

        # ── SSE (scalar float) ──
        if m in ("movss", "movsd", "movaps", "movups", "movlps", "movhps",
                 "movlhps", "movhlps", "movapd", "movupd",
                 "addss", "subss", "mulss", "divss", "sqrtss",
                 "addsd", "subsd", "mulsd", "divsd", "sqrtsd",
                 "minss", "maxss", "minsd", "maxsd",
                 "comiss", "comisd", "ucomiss", "ucomisd",
                 "cvtsi2ss", "cvtss2si", "cvttss2si",
                 "cvtsi2sd", "cvtsd2si", "cvttsd2si",
                 "cvtss2sd", "cvtsd2ss",
                 "xorps", "xorpd", "andps", "orps", "andnps",
                 "movd", "movq",
                 "shufps", "unpcklps", "unpckhps",
                 "addps", "subps", "mulps", "divps",
                 "minps", "maxps", "rsqrtss", "rcpss",
                 "sqrtps", "rsqrtps", "rcpps",
                 "cmpneqps", "cmpeqps", "cmpltps", "cmpleps",
                 "movmskps",
                 "pand", "pandn", "por", "pxor", "pcmpgtd"):
            return self._lift_sse(insn, m, ops)

        # ── FPU ──
        if m.startswith("f"):
            return self._lift_fpu(insn, m, ops)

        # ── Explicit carry-flag manipulation ──
        # MSVC emits these around the multi-word arithmetic helpers, and around
        # the "return a bool in CF" idiom. They were unhandled, so the flag the
        # following adc/sbb reads kept whatever the last arithmetic left in it.
        # Only worth emitting when something downstream consumes CF -- that is
        # also the only time the enclosing function declares _cf.
        if m in ("stc", "clc", "cmc"):
            if not self.needs_cf:
                return [f"/* {m}: no adc/sbb in this function consumes CF */"]
            expr = {"stc": "1", "clc": "0", "cmc": "!_cf"}[m]
            return [f"_cf = {expr}; /* {m} */"]

        # ── Unhandled ──
        return [f"/* TODO: {m} {insn.op_str} */"]

    # ── MOV family ──

    def _lift_mov(self, insn, ops):
        if nops := len(ops) < 2:
            return [f"/* mov: bad operands */"]
        src = _fmt_operand_read(ops[1])
        out = [_fmt_operand_write(ops[0], src)]
        # `mov ebp, esp` establishes this function's frame. Publish it, because
        # ebp is a per-function local and a callee with no prologue of its own
        # reads its caller's frame through ebp -- MSVC emits those routinely for
        # shared tails (Halo's CRT float formatting is one). Without the
        # publish, such a callee starts from an uninitialised ebp and its
        # [ebp-N] stores land wherever that garbage points.
        if (ops[0].type == "reg" and ops[0].reg == "ebp" and
                ops[1].type == "reg" and ops[1].reg == "esp"):
            out.append("g_ebp = ebp; /* publish frame for frameless callees */")
        return out

    def _lift_movzx(self, insn, ops):
        if len(ops) < 2:
            return [f"/* movzx: bad operands */"]
        src = _fmt_operand_read(ops[1])
        if ops[1].type == "mem":
            if ops[1].mem_size == 1:
                src = f"ZX8({src})"
            elif ops[1].mem_size == 2:
                src = f"ZX16({src})"
        elif ops[1].type == "reg":
            r = ops[1].reg
            if r in ("al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"):
                src = f"ZX8({src})"
            elif r in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"):
                src = f"ZX16({src})"
        return [_fmt_operand_write(ops[0], src)]

    def _lift_movsx(self, insn, ops):
        if len(ops) < 2:
            return [f"/* movsx: bad operands */"]
        src = _fmt_operand_read(ops[1])
        if ops[1].type == "mem":
            accessor = _smem_accessor(
                ops[1].mem_size, getattr(ops[1], "mem_segment", None))
            addr = _fmt_mem(ops[1])
            src = f"(uint32_t)(int32_t){accessor}({addr})"
        elif ops[1].type == "reg":
            r = ops[1].reg
            if r in ("al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"):
                src = f"SX8({src})"
            elif r in ("ax", "bx", "cx", "dx", "si", "di"):
                src = f"SX16({src})"
        return [_fmt_operand_write(ops[0], src)]

    def _lift_lea(self, insn, ops):
        if len(ops) < 2 or ops[1].type != "mem":
            return [f"/* lea: unexpected operands */"]
        addr_expr = _fmt_mem(ops[1])
        return [_fmt_operand_write(ops[0], addr_expr)]

    def _lift_xchg(self, insn, ops):
        if len(ops) < 2:
            return [f"/* xchg: bad operands */"]
        a = _fmt_operand_read(ops[0])
        b = _fmt_operand_read(ops[1])
        return [
            f"{{ uint32_t _tmp = {a};",
            _fmt_operand_write(ops[0], b),
            _fmt_operand_write(ops[1], "_tmp") + " }",
        ]

    def _lift_mmx_move(self, insn, ops, nontemporal=False):
        """movq / movntq where at least one operand is an MMX register.

        `movq` was already in the SSE mnemonic list, so an MMX form fell
        through that handler and came out as "/* SSE: movq mm0, ... */"; the
        `movntq` store had no rule at all and came out as a TODO. The D3D block
        copy at sub_00198FD0 is eight of each per iteration, so its 64-byte
        inner loop copied NOTHING -- only the rep movsd/movsb prologue and
        epilogue moved any bytes.

        Non-temporal only describes cache behaviour, so movntq is the same
        store; the distinction is recorded in the comment and nowhere else.
        """
        dst, src = ops[0], ops[1]
        note = "movntq" if nontemporal else "movq"

        def rd(op):
            if _is_mmx_operand(op):
                return op.reg
            if op.type == "mem":
                return f"MEM64({_fmt_mem(op)})"
            return _fmt_operand_read(op)

        if _is_mmx_operand(dst):
            return [f"{dst.reg} = {rd(src)}; /* {note} */"]
        if dst.type == "mem":
            return [f"MEM64({_fmt_mem(dst)}) = {rd(src)}; /* {note} */"]
        return [f"/* {note} {insn.op_str}: unsupported operand form */"]

    def _lift_cmpxchg(self, insn, ops, locked=False):
        """Compare-and-exchange.

            TEMP = DEST
            if (ACC == TEMP) { ZF = 1; DEST = SRC }
            else             { ZF = 0; ACC = TEMP }

        Left as a comment this was worse than a no-op. DSOUND's sub_001A1F31
        uses the standard `mov eax,[m]; L: cmpxchg [m],edx; jne L` idiom to
        atomically swap a pending-event mask to zero:

            eax = [m]                  ; snapshot
          L: cmpxchg [m], edx          ; swap in 0 if [m] is still eax
            jne L                      ; retry with the reloaded eax
            or  [acc], eax             ; drain what we took

        With the exchange missing the mask is never cleared, and because
        nothing reloads eax the retry loop spins forever the moment another
        thread touches the word -- which is what JSRF does once real worker
        threads run.

        ACC is al/ax/eax by operand width. LOCK is only meaningful with a
        memory destination and selects the runtime's sequentially-consistent
        helper; unlocked memory cmpxchg stays a plain read/modify/write.
        """
        if len(ops) < 2:
            return [f"/* {'lock ' if locked else ''}cmpxchg: bad operands */"]
        dst, src = ops[0], ops[1]
        width = _operand_width(dst) or _operand_width(src) or 4
        if width not in (1, 2, 4):
            return [f"/* {'lock ' if locked else ''}cmpxchg: "
                    f"unsupported width {width} */"]
        if locked and dst.type != "mem":
            return ["/* lock cmpxchg: LOCK requires a memory destination */"]

        bits = width * 8
        ctype = f"uint{bits}_t"
        acc_read = {1: "LO8(eax)", 2: "LO16(eax)", 4: "eax"}[width]
        acc_write = {1: "SET_LO8(eax, _cx_old);",
                     2: "SET_LO16(eax, _cx_old);",
                     4: "eax = _cx_old;"}[width]
        src_expr = _fmt_operand_read(src)

        line = (f"{{ {ctype} _cx_acc = ({ctype})({acc_read}); "
                f"{ctype} _cx_src = ({ctype})({src_expr}); "
                f"int _cx_ok = 0; {ctype} _cx_old; ")

        if dst.type == "mem":
            accessor = _mem_accessor(width)
            line += f"uint32_t _cx_addr = (uint32_t)({_fmt_mem(dst)}); "
            if locked:
                line += (f"_cx_old = RECOMP_ATOMIC_CMPXCHG{bits}"
                         f"(_cx_addr, _cx_acc, _cx_src, &_cx_ok); ")
            else:
                line += (f"_cx_old = ({ctype}){accessor}(_cx_addr); "
                         f"_cx_ok = (_cx_old == _cx_acc); "
                         f"if (_cx_ok) {accessor}(_cx_addr) = _cx_src; ")
        else:
            line += (f"_cx_old = ({ctype})({_fmt_operand_read(dst)}); "
                     f"_cx_ok = (_cx_old == _cx_acc); "
                     f"if (_cx_ok) " + _fmt_operand_write(dst, "_cx_src") + " ")

        line += f"if (!_cx_ok) {acc_write} _flags = _cx_ok; }}"
        return [line + f" /* {'lock ' if locked else ''}cmpxchg */"]

    def _lift_xadd(self, insn, ops, locked=False):
        """Exchange-and-add for register or memory destinations.

        Capture the effective address and source before either architectural
        operand is written.  That ordering matters for forms such as
        ``xadd [eax], eax``.  LOCK is only valid with a memory destination and
        selects the runtime's sequentially-consistent atomic fetch-add helper;
        ordinary memory XADD remains a single-threaded read/modify/write.
        """
        if len(ops) < 2:
            return [f"/* {'lock ' if locked else ''}xadd: bad operands */"]
        dst, src = ops[0], ops[1]
        width = _operand_width(dst) or _operand_width(src) or 4
        if width not in (1, 2, 4):
            return [f"/* {'lock ' if locked else ''}xadd: unsupported width {width} */"]
        if locked and dst.type != "mem":
            return ["/* lock xadd: LOCK requires a memory destination */"]

        bits = width * 8
        ctype = f"uint{bits}_t"
        src_expr = _fmt_operand_read(src)
        result = []

        if dst.type == "mem":
            addr = _fmt_mem(dst)
            result.append(f"{{ uint32_t _xadd_addr = (uint32_t)({addr}); ")
            result[-1] += f"{ctype} _xadd_src = ({ctype})({src_expr}); "
            if locked:
                result[-1] += (
                    f"{ctype} _xadd_old = RECOMP_ATOMIC_XADD{bits}"
                    f"(_xadd_addr, _xadd_src); ")
            else:
                accessor = _mem_accessor(width)
                result[-1] += f"{ctype} _xadd_old = {accessor}(_xadd_addr); "
            result[-1] += (
                f"{ctype} _xadd_result = ({ctype})(_xadd_old + _xadd_src); "
                f"_flags = RECOMP_ADD_FLAGS{bits}"
                f"(_xadd_old, _xadd_src, _xadd_result); ")
            if not locked:
                result[-1] += (
                    f"{_mem_accessor(width)}(_xadd_addr) = _xadd_result; ")
            result[-1] += _fmt_operand_write(src, "_xadd_old") + " }"
            return result

        if dst.type == "reg":
            dst_expr = _fmt_operand_read(dst)
            line = (
                f"{{ {ctype} _xadd_old = ({ctype})({dst_expr}); "
                f"{ctype} _xadd_src = ({ctype})({src_expr}); "
                f"{ctype} _xadd_result = ({ctype})(_xadd_old + _xadd_src); "
                f"_flags = RECOMP_ADD_FLAGS{bits}"
                f"(_xadd_old, _xadd_src, _xadd_result); "
                + _fmt_operand_write(src, "_xadd_old") + " "
                + _fmt_operand_write(dst, "_xadd_result") + " }"
            )
            return [line]

        return [f"/* {'lock ' if locked else ''}xadd: bad destination */"]

    # ── Stack ──

    def _lift_push(self, insn, ops):
        if len(ops) < 1:
            return ["/* push: no operand */"]
        val = _fmt_operand_read(ops[0])
        return [f"PUSH32(esp, {val});"]

    def _lift_pop(self, insn, ops):
        if len(ops) < 1:
            return ["/* pop: no operand */"]
        if ops[0].type == "reg":
            r = ops[0].reg
            # Segment register pop → discard from stack
            if r in ("fs", "gs", "cs", "ds", "es", "ss"):
                return [f"{{ uint32_t _tmp; POP32(esp, _tmp); }} /* pop {r} - segment register */"]
            # Sample esp at each pop in a traced function. An epilogue that ends
            # `mov esp, ebp` restores esp unconditionally, so any drift inside
            # the function is erased before a return-time trace can see it --
            # yet the pops run *before* that restore, so drift is exactly what
            # breaks them. This is the only point the drift is observable.
            if self.trace_exit_name:
                return [f'RECOMP_TRACE_ESP("{self.trace_exit_name}", "pop {r}");',
                        f"POP32(esp, {r});"]
            return [f"POP32(esp, {r});"]
        else:
            return [f"{{ uint32_t _tmp; POP32(esp, _tmp); {_fmt_operand_write(ops[0], '_tmp')} }}"]

    # ── ALU binary operations ──

    def _lift_alu_binop(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        c_op = {"add": "+", "sub": "-", "and": "&", "or": "|", "xor": "^"}[m]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        # XOR reg, reg → zero
        if m == "xor" and ops[0].type == "reg" and ops[1].type == "reg" and ops[0].reg == ops[1].reg:
            return [_fmt_operand_write(ops[0], "0") + " /* xor self */"]
        expr = f"{dst} {c_op} {src}"
        out = []
        if self.needs_cf:
            # CF must be computed from the pre-write operands.
            if m == "add":
                w = _operand_width(ops[0]) or 4
                out.append(f"_cf = (int)((((uint64_t)({dst}) + (uint64_t)({src})) >> {w * 8}) & 1);")
            elif m == "sub":
                out.append(f"_cf = (int)((uint32_t)({dst}) < (uint32_t)({src}));")
            else:
                out.append("_cf = 0; /* logical op clears CF */")
        out.append(_fmt_operand_write(ops[0], expr))
        return out

    def _lift_inc_dec(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        val = _fmt_operand_read(ops[0])
        delta = "1"
        op_char = "+" if m == "inc" else "-"
        # For sub-registers (al, cl, etc.), use the SET macro instead of ++
        if ops[0].type == "reg" and ops[0].reg in (
                "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"):
            return [f"{val}{'++' if m == 'inc' else '--'};"]
        else:
            return [_fmt_operand_write(ops[0], f"{val} {op_char} {delta}")]

    def _lift_neg(self, insn, ops, preserve_carry=False):
        if len(ops) < 1:
            return ["/* neg: no operand */"]
        val = _fmt_operand_read(ops[0])
        out = []
        if preserve_carry or self.needs_cf:
            # neg sets CF iff the operand was non-zero (neg/sbb sign-extract).
            out.append(f"_cf = (int)(({val}) != 0);")
        out.append(_fmt_operand_write(ops[0], f"(uint32_t)(-(int32_t){val})"))
        return out

    def _lift_not(self, insn, ops):
        if len(ops) < 1:
            return ["/* not: no operand */"]
        val = _fmt_operand_read(ops[0])
        return [_fmt_operand_write(ops[0], f"~{val}")]

    def _lift_sbb(self, insn, ops):
        """SBB: subtract with borrow. Common idiom: sbb reg, reg → -CF (0 or -1)."""
        if len(ops) < 2:
            return ["/* sbb: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        # sbb reg, reg is a common idiom: result is 0 or 0xFFFFFFFF depending on CF
        if ops[0].type == "reg" and ops[1].type == "reg" and ops[0].reg == ops[1].reg:
            return [_fmt_operand_write(ops[0], "_cf ? 0xFFFFFFFF : 0") + " /* sbb self (CF extend) */"]
        w = (_operand_width(ops[0]) or 4) * 8
        return ["{ uint64_t _t = (uint64_t)(%s) - (uint64_t)(%s) - (uint64_t)_cf;"
                " _cf = (int)((_t >> %d) & 1); %s }  /* sbb */"
                % (dst, src, w, _fmt_operand_write(ops[0], "(uint32_t)_t"))]

    def _lift_adc(self, insn, ops):
        """ADC: add with carry."""
        if len(ops) < 2:
            return ["/* adc: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        w = (_operand_width(ops[0]) or 4) * 8
        return ["{ uint64_t _t = (uint64_t)(%s) + (uint64_t)(%s) + (uint64_t)_cf;"
                " _cf = (int)((_t >> %d) & 1); %s }  /* adc */"
                % (dst, src, w, _fmt_operand_write(ops[0], "(uint32_t)_t"))]

    def _lift_shld(self, insn, ops):
        """SHLD: double-precision shift left."""
        if len(ops) < 3:
            return [f"/* shld: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        cnt = _fmt_operand_read(ops[2])
        return [_fmt_operand_write(ops[0],
            f"({dst} << {cnt}) | ({src} >> (32 - {cnt}))") + " /* shld */"]

    def _lift_shrd(self, insn, ops):
        """SHRD: double-precision shift right."""
        if len(ops) < 3:
            return [f"/* shrd: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        cnt = _fmt_operand_read(ops[2])
        return [_fmt_operand_write(ops[0],
            f"({dst} >> {cnt}) | ({src} << (32 - {cnt}))") + " /* shrd */"]

    def _lift_imul(self, insn, ops):
        nops = len(ops)
        if nops == 1:
            # One operand: edx:eax = eax * ops[0]
            src = _fmt_operand_read(ops[0])
            return [
                f"{{ int64_t _r = (int64_t)(int32_t)eax * (int64_t)(int32_t){src};",
                f"  eax = (uint32_t)_r; edx = (uint32_t)(_r >> 32); }}"
            ]
        elif nops == 2:
            # Two operand: dst = dst * src
            dst = _fmt_operand_read(ops[0])
            src = _fmt_operand_read(ops[1])
            return [_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){dst} * (int32_t){src})")]
        elif nops == 3:
            # Three operand: dst = src1 * imm
            src = _fmt_operand_read(ops[1])
            imm = _fmt_operand_read(ops[2])
            return [_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){src} * (int32_t){imm})")]
        return ["/* imul: unexpected form */"]

    def _lift_muldiv(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        src = _fmt_operand_read(ops[0])
        if m == "mul":
            return [
                f"{{ uint64_t _r = (uint64_t)eax * (uint64_t){src};",
                f"  eax = (uint32_t)_r; edx = (uint32_t)(_r >> 32); }}"
            ]
        elif m == "div":
            return [
                f"{{ uint64_t _dividend = ((uint64_t)edx << 32) | eax;",
                f"  eax = (uint32_t)(_dividend / (uint32_t){src});",
                f"  edx = (uint32_t)(_dividend % (uint32_t){src}); }}"
            ]
        elif m == "idiv":
            return [
                f"{{ int64_t _dividend = ((int64_t)(int32_t)edx << 32) | eax;",
                f"  eax = (uint32_t)((int32_t)(_dividend / (int32_t){src}));",
                f"  edx = (uint32_t)((int32_t)(_dividend % (int32_t){src})); }}"
            ]
        return [f"/* {m}: unhandled */"]

    def _lift_shift(self, insn, ops, c_op):
        if len(ops) < 2:
            return [f"/* shift: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        out = []
        if self.needs_cf:
            w = (_operand_width(ops[0]) or 4) * 8
            # CF is the last bit shifted out; a zero count leaves CF alone.
            bit = f"({cnt}) - 1" if c_op == ">>" else f"{w} - ({cnt})"
            out.append(f"if ({cnt}) _cf = (int)((({dst}) >> ({bit})) & 1);")
        out.append(_fmt_operand_write(ops[0], f"{dst} {c_op} {cnt}"))
        return out

    def _lift_sar(self, insn, ops):
        if len(ops) < 2:
            return ["/* sar: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        out = []
        if self.needs_cf:
            out.append(f"if ({cnt}) _cf = (int)((({dst}) >> (({cnt}) - 1)) & 1);")
        out.append(_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){dst} >> {cnt})"))
        return out

    def _lift_rotate(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        func = "ROL32" if m == "rol" else "ROR32"
        return [_fmt_operand_write(ops[0], f"{func}({dst}, {cnt})")]

    def _lift_bit_scan(self, ops, mnemonic):
        """Lift bsf/bsr.

        Why this matters, kept from the independent fix this replaced: the
        Xbox D3D library's Log2 helper is a bare `bsf eax, ecx`. Unhandled it
        lifted to a comment, so the helper returned whatever eax happened to
        hold, and every surface size computed from log2(w)+log2(h)+log2(d) was
        wrong -- JSRF asked MmAllocateContiguousMemoryEx for 0x08000000 bytes,
        twice the console's RAM, and D3D failed the create with E_OUTOFMEMORY.

        With a zero source x86 sets ZF and leaves the destination ALONE, which
        is what the guard below reproduces; writing a sentinel instead would
        invent a value the hardware never produces.
        """
        if len(ops) < 2:
            return [f"/* {mnemonic}: bad operands */"]

        src = _fmt_operand_read(ops[1])
        width = _operand_width(ops[1]) or _operand_width(ops[0]) or 4
        bits = width * 8
        value = (f"(uint32_t)(uint16_t)({src})" if width == 2
                 else f"(uint32_t)({src})")
        if mnemonic == "bsr":
            initial_index = bits - 1
            step = "--_bs_index"
        else:
            initial_index = 0
            step = "++_bs_index"
        write = _fmt_operand_write(ops[0], "_bs_index")
        statements = [
            "{",
            f"    uint32_t _bs_value = {value};",
        ]
        if self.needs_flags:
            statements.append("    _flags = (_bs_value == 0);")
        statements.extend([
            "    if (_bs_value != 0) {",
            f"        uint32_t _bs_index = {initial_index};",
            "        while (((_bs_value >> _bs_index) & 1u) == 0u) "
            f"{step};",
            f"        {write}",
            "    }",
            f"}} /* {mnemonic} */",
        ])
        return statements

    # ── Compare / Test (standalone) ──

    # Widths for the flag snapshot below.
    _SNAP_MASK = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}
    _SNAP_SX = {1: "(int8_t)", 2: "(int16_t)", 4: "(int32_t)"}

    def _snapshot_flags(self, insn, ops, kind):
        """Capture a cmp/test's operands where the comparison happens.

        The flags are consumed later by a jcc, which used to re-read the
        operands at that point. Two things go wrong with that. Anything
        between the two can change them - "cmp [esi+ecx*2], -1 / lea esi,
        [esi+ecx*2] / jne" re-read through the already-advanced esi and
        tested the wrong slot. And the comparison lost its width, so a
        16-bit "cmp word ptr [..], -1" became a compare against
        0xFFFFFFFF, which a 16-bit -1 (0xFFFF) never equals - the branch
        then went the same way every time.

        Snapshotting fixes both: operands are read once, at the right
        width, in both a zero- and a sign-extended form so the jcc can
        pick whichever its condition needs.
        """
        size = _operand_width(ops[0])
        if size is None:
            size = _operand_width(ops[1])          # e.g. cmp imm, reg
        if size not in self._SNAP_MASK:
            size = 4
        mask = self._SNAP_MASK[size]
        sx = self._SNAP_SX[size]
        lhs = _fmt_operand_read(ops[0])
        rhs = _fmt_operand_read(ops[1])
        out = [
            f"_fa = (uint32_t)({lhs}) & {mask}; _fb = (uint32_t)({rhs}) & {mask};",
            f"_fas = (int32_t){sx}(_fa); _fbs = (int32_t){sx}(_fb);"
            f" /* {kind} {lhs}, {rhs} ({size*8}-bit) */",
        ]
        # A cmp sets the carry flag too, and sbb/adc/setc/rcl read it directly
        # rather than through _fa/_fb. Leaving CF alone here let those pick up
        # whatever an earlier instruction had left in it.
        #
        # MSVC's branchless tolower, in the _stricmp every Source string
        # compare goes through, is exactly that shape:
        #
        #     sub al, 0x41      ; CF = al < 'A'
        #     cmp al, 0x1A      ; CF = "is a letter"  <- the one sbb wants
        #     sbb cl, cl
        #     and cl, 0x20
        #     add al, cl        ; fold to lowercase
        #
        # With CF still coming from the sub, no uppercase letter was ever
        # folded and _stricmp quietly became strcmp.
        #
        # _fa and _fb are already masked to the operand width, so their
        # unsigned comparison is CF at that width.
        if self.needs_cf:
            if kind == "cmp":
                out.append("_cf = (int)(_fa < _fb);")
            else:
                out.append("_cf = 0; /* test/cmp-logical clears CF */")
        return out

    def _lift_cmp(self, insn, ops):
        if len(ops) < 2:
            return ["/* cmp: bad operands */"]
        return self._snapshot_flags(insn, ops, "cmp")

    def _lift_test(self, insn, ops):
        if len(ops) < 2:
            return ["/* test: bad operands */"]
        return self._snapshot_flags(insn, ops, "test")

    # ── Control flow ──

    def _build_call_args(self, target_addr):
        """Build argument list for a function call based on ABI data."""
        abi_info = self.abi_db.get(target_addr, {})
        cc = abi_info.get("calling_convention", "cdecl")
        num_params = abi_info.get("estimated_params", 0)

        args = []
        if cc in ("thiscall", "thiscall_cdecl"):
            args.append("(void*)(uintptr_t)ecx")
        for i in range(num_params):
            args.append(f"0 /* a{i+1} */")
        return ", ".join(args)

    # SEH prolog/epilog addresses - these functions modify ebp for their
    # caller.  After calling __SEH_prolog, the caller must read back ebp
    # from g_seh_ebp.  Before returning, __SEH_prolog writes g_seh_ebp.
    #
    # Per-title addresses, detected from the binary by detect_seh_helpers()
    # and assigned to the instance. The class values are only a fallback for
    # callers that construct a Lifter without a function database.
    SEH_PROLOG = None
    SEH_EPILOG = None

    def _lift_call(self, insn, ops):
        # x86 'call' pushes the address of the following instruction, then jumps.
        # Push that real guest address, not a placeholder: the value is visible
        # to the callee, and plenty of x86 code reads it. __SEH_prolog locates
        # its scope table through [esp], _alloca probes walk back to it, and the
        # `mov eax, [esp]` / `pop eax` idiom for "where was I called from" shows
        # up in any CRT. A zero there is silently wrong until something reads it.
        #
        # 'ret' still only does esp += 4 and returns -- it never consumes the
        # value -- so writing the true address costs nothing at the return side.
        ret_va = insn.end_address
        if insn.call_target:
            lines = []
            # Re-publish this function's frame before every call, not just once
            # at `mov ebp, esp`. g_ebp is "the last frame established anywhere",
            # so a callee that sets up its own frame overwrites it and leaves it
            # stale on return. A frameless helper called afterwards then
            # inherits the wrong frame -- in Halo, sub_001E1BA0 called one
            # function, returned, then called the frameless sub_001DEC07, which
            # inherited a long-dead frame of ~0xA6 and wrote [ebp-0xa2] and
            # [ebp-0xa0] onto Xbox VA 4 and 6: exactly the fs:[4] corruption.
            if self.publishes_ebp:
                lines.append("g_ebp = ebp; /* frame stays current across calls */")
            if insn.call_target in self.manual_functions:
                lines.append(
                    f"PUSH32(esp, 0x{ret_va:08X}u); "
                    f"RECOMP_ICALL_SAFE(0x{insn.call_target:08X}u, "
                    "_icall_esp); "
                    f"/* manual call 0x{insn.call_target:08X} */")
            else:
                name = self._call_target_name(insn.call_target)
                lines.append(
                    f"PUSH32(esp, 0x{ret_va:08X}u); {name}(); "
                    f"/* call 0x{insn.call_target:08X} */")
            # esp immediately after the callee returns. A per-call delta is the
            # only way to attribute a leak to one callee rather than to the
            # function containing them all.
            if self.trace_exit_name:
                lines.append(
                    f'RECOMP_TRACE_ESP("{self.trace_exit_name}", '
                    f'"after call 0x{insn.call_target:08X}");')
            # The SEH helpers exchange the frame pointer with their caller
            # through g_seh_ebp, because ebp is a C local rather than a global.
            #
            # Publishing before the call is as necessary as reading back after.
            # __SEH_prolog stores the caller's ebp into the new frame, and
            # __SEH_epilog restores esp from it before popping ebx/esi/edi. With
            # only the read-back, g_seh_ebp still held whatever the last *nested*
            # SEH function left there, so the epilog unwound to the wrong frame
            # and restored the callee-saved registers from the wrong stack slots.
            #
            # That corrupts ebx/esi/edi across any SEH function that calls
            # another - which is most of them. In Halo it left esi holding a
            # stack address where the caller had just zeroed it, so an
            # "if (status < 0)" test against esi failed and XapiInitProcess
            # bailed to the dashboard.
            if insn.call_target in (self.SEH_PROLOG, self.SEH_EPILOG):
                lines.insert(0, "g_seh_ebp = ebp; /* publish frame to SEH helper */")
                lines.append("ebp = g_seh_ebp; /* read back frame from SEH helper */")
            return lines
        elif len(ops) >= 1:
            target = _fmt_operand_read(ops[0])
            # Mark indirect calls for post-processing by _fixup_icall_esp_save
            #
            # The target is read into a local BEFORE the return-address push.
            # On real x86 the memory operand is computed before the push, so
            # emitting the push first shifts esp by four and every esp-relative
            # target -- `call dword ptr [esp+8]`, the shape MSVC gives a
            # callback invoked through a stack argument -- is read four bytes
            # low and dispatches through the wrong slot.
            return [f"{{ uint32_t _icall_target = {target}; "
                    f"PUSH32(esp, 0x{ret_va:08X}u); "
                    "RECOMP_ICALL_SAFE(_icall_target, _icall_esp); }"
                    " /* indirect call */"]
        return ["/* call: no target */"]

    def _lift_ret(self, insn, ops):
        # x86 'ret' pops return address from stack.
        # 'ret N' also pops N extra bytes (stdcall cleanup).
        # If this function IS __SEH_prolog or __SEH_epilog, bridge ebp
        # so the caller can read back the frame pointer.
        prefix = ""
        if self.func_start in (self.SEH_PROLOG, self.SEH_EPILOG):
            prefix = "g_seh_ebp = ebp; "
        # Exit trace, for functions that return with a register the caller
        # relied on holding something else. Entry tracing alone cannot show
        # that: it tells you what went in, never what came back. Emitted at the
        # ret, after the epilogue's pops, so the values are what the caller
        # actually receives.
        if self.trace_exit_name:
            prefix = (f'RECOMP_TRACE_EXIT("{self.trace_exit_name}", '
                      f'0x{self.func_start:08X}); ') + prefix
        if len(ops) >= 1 and ops[0].type == "imm":
            n = ops[0].imm
            return [f"{prefix}esp += {4 + n}; return; /* ret {n} */"]
        return [f"{prefix}esp += 4; return; /* ret */"]

    def _is_external_target(self, addr):
        """Check if a jump target is outside the current function."""
        return not (self.func_start <= addr < self.func_end)

    def _read_jump_table(self, table_va, max_entries=256):
        """Read 32-bit jump table entries from the XBE at a given VA.
        Returns list of target addresses. Stops when an entry is not a
        valid code address or max_entries is reached."""
        if not self.xbe_data:
            return []
        offset = va_to_file_offset(table_va)
        if offset is None:
            return []
        targets = []
        for i in range(max_entries):
            o = offset + i * 4
            if o + 4 > len(self.xbe_data):
                break
            val = struct.unpack_from('<I', self.xbe_data, o)[0]
            if not is_code_address(val):
                break
            targets.append(val)
        return targets

    def _analyze_switch_table(self, ops):
        """Detect if an indirect jmp operand is an intra-function switch table.
        Pattern: jmp [reg*scale + table_base] or jmp [reg + table_base]
        Returns (targets: list[int]) if ALL table entries are within the current
        function, else empty list."""
        if not ops or ops[0].type != "mem":
            return []
        op = ops[0]
        # Need a table base (displacement) and an index register
        if not op.mem_disp or not (op.mem_index or op.mem_base):
            return []
        table_va = op.mem_disp
        targets = self.jump_table_targets.get(table_va)
        if targets is None:
            targets = self._read_jump_table(table_va)
        if not targets:
            return []
        # Check that ALL targets are within the current function
        if all(self.func_start <= t < self.func_end for t in targets):
            return targets
        return []

    def _lift_jmp(self, insn, ops):
        if insn.jump_target:
            if self._is_external_target(insn.jump_target):
                # Tail call - no return address push (reuses current frame's)
                # Bridge ebp so the target function can inherit our frame pointer.
                if insn.jump_target in self.manual_functions:
                    tail = (
                        f"g_seh_ebp = ebp; "
                        f"RECOMP_ITAIL(0x{insn.jump_target:08X}u); return; "
                        f"/* manual tail jmp 0x{insn.jump_target:08X} */"
                    )
                    if self.trace_exit_name:
                        return [
                            f'RECOMP_TRACE_ESP("{self.trace_exit_name}", '
                            f'"tail 0x{insn.jump_target:08X}");',
                            tail,
                        ]
                    return [tail]
                name = self._call_target_name(insn.jump_target)
                tail = (f"g_seh_ebp = ebp; {name}(); return; "
                        f"/* tail jmp 0x{insn.jump_target:08X} */")
                if self.trace_exit_name:
                    # Tag the exit so a trace says which one was taken. A
                    # function whose paths all look individually balanced can
                    # still leak, and knowing the path is the difference
                    # between measuring and guessing.
                    return [f'RECOMP_TRACE_ESP("{self.trace_exit_name}", '
                            f'"tail 0x{insn.jump_target:08X}");', tail]
                return [tail]
            return [f"goto loc_{insn.jump_target:08X};"]
        elif len(ops) >= 1:
            # Detect intra-function switch tables (computed gotos)
            switch_targets = self._analyze_switch_table(ops)
            if switch_targets:
                target_expr = _fmt_operand_read(ops[0])
                unique_targets = sorted(set(switch_targets))
                lines = [f"{{ uint32_t _jt = {target_expr}; /* switch: {len(switch_targets)} entries, {len(unique_targets)} targets */"]
                for t in unique_targets:
                    lines.append(f"if (_jt == 0x{t:08X}u) goto loc_{t:08X};")
                lines.append(f"g_seh_ebp = ebp; RECOMP_ITAIL(_jt); return; }}")
                return lines
            target = _fmt_operand_read(ops[0])
            return [f"g_seh_ebp = ebp; RECOMP_ITAIL({target}); return; /* indirect tail jmp */"]
        return ["/* jmp: no target */"]

    def _lift_jcc(self, insn):
        """Standalone conditional jump (no flag-setter tracked)."""
        target = insn.jump_target
        jcc = insn.mnemonic

        # jecxz/jcxz: jump if ecx/cx is zero (not flag-based)
        if jcc in ("jecxz", "jcxz"):
            cond = "ecx == 0" if jcc == "jecxz" else "LO16(ecx) == 0"
            if target:
                if self._is_external_target(target):
                    name = self._call_target_name(target)
                    return [f"if ({cond}) {{ g_seh_ebp = ebp; {name}(); return; }} /* {jcc} */"]
                return [f"if ({cond}) goto loc_{target:08X}; /* {jcc} */"]
            return [f"/* {jcc} - no target */"]

        cond_info = COND_MAP.get(jcc)
        desc = cond_info[2] if cond_info else jcc
        if target:
            if self._is_external_target(target):
                name = self._call_target_name(target)
                return [f"if (_flags /* {jcc}: {desc} */) {{ g_seh_ebp = ebp; {name}(); return; }}"]
            return [f"if (_flags /* {jcc}: {desc} */) goto loc_{target:08X};"]
        return [f"/* {jcc}: {desc} - no target */"]

    # ── SETcc / CMOVcc ──

    def _lift_setcc(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        return [_fmt_operand_write(ops[0], f"_flags /* {m} */")]

    def _lift_cmovcc(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        src = _fmt_operand_read(ops[1])
        return [f"if (_flags /* {m} */) {_fmt_operand_write(ops[0], src)}"]

    # ── String operations ──

    def _lift_rep_string(self, insn, m):
        if "movsb" in m:
            return ["memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx);",
                    "esi += ecx; edi += ecx; ecx = 0; /* rep movsb */"]
        if "movsd" in m:
            return ["memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx * 4);",
                    "esi += ecx * 4; edi += ecx * 4; ecx = 0; /* rep movsd */"]
        if "movsw" in m:
            return ["memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx * 2);",
                    "esi += ecx * 2; edi += ecx * 2; ecx = 0; /* rep movsw */"]
        if "stosb" in m:
            return ["memset((void*)XBOX_PTR(edi), (uint8_t)eax, ecx);",
                    "edi += ecx; ecx = 0; /* rep stosb */"]
        if "stosd" in m:
            return [
                "{ uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi + _i*4) = eax; }",
                "edi += ecx * 4; ecx = 0; /* rep stosd */"
            ]
        if "stosw" in m:
            return [
                "{ uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM16(edi + _i*2) = LO16(eax); }",
                "edi += ecx * 2; ecx = 0; /* rep stosw */"
            ]
        if "cmpsb" in m:
            continue_on_equal = "repne" not in m and "repnz" not in m
            stop_condition = "!_flags" if continue_on_equal else "_flags"
            return [
                "while (ecx != 0) {",
                "    _flags = (MEM8(esi) == MEM8(edi));",
                "    esi++; edi++; ecx--;",
                f"    if ({stop_condition}) break;",
                f"}} /* {m} */",
            ]
        if "scasb" in m:
            continue_on_equal = "repne" not in m and "repnz" not in m
            stop_condition = "!_flags" if continue_on_equal else "_flags"
            return [
                "while (ecx != 0) {",
                "    _flags = (LO8(eax) == MEM8(edi));",
                "    edi++; ecx--;",
                f"    if ({stop_condition}) break;",
                f"}} /* {m} */",
            ]
        # The word and dword widths were left as comments while cmpsb/scasb
        # above were implemented. A comment is worse than an unhandled
        # instruction here: the compare never runs, so the following setcc or
        # jcc resolves its ZF against whatever instruction came before -- and
        # what comes before a GUID compare is the `xor edx, edx` that zeroes
        # the result byte, which reads as "equal" every time.
        #
        # JSRF's CDirectSound QueryInterface (sub_00168310) is four of these in
        # a row against candidate IIDs. The first always matched, so QI handed
        # back the primary interface for every IID asked of it. The caller had
        # asked for the second base, called ITS vtable slot 3, and reached a
        # two-argument method with three arguments pushed: 4 bytes leaked, the
        # enclosing `pop esi` read one slot low, and the game object's `this`
        # came back as a TLS address. The title then exited to the dashboard.
        for width, mem, stride in (("cmpsw", "MEM16", 2), ("cmpsd", "MEM32", 4)):
            if width in m:
                stop = "!_flags" if ("repne" not in m and "repnz" not in m) else "_flags"
                return [
                    "while (ecx != 0) {",
                    f"    _flags = ({mem}(esi) == {mem}(edi));",
                    f"    esi += {stride}; edi += {stride}; ecx--;",
                    f"    if ({stop}) break;",
                    f"}} /* {m} */",
                ]
        for width, mem, reg, stride in (("scasw", "MEM16", "LO16(eax)", 2),
                                        ("scasd", "MEM32", "eax", 4)):
            if width in m:
                stop = "!_flags" if ("repne" not in m and "repnz" not in m) else "_flags"
                return [
                    "while (ecx != 0) {",
                    f"    _flags = ({reg} == {mem}(edi));",
                    f"    edi += {stride}; ecx--;",
                    f"    if ({stop}) break;",
                    f"}} /* {m} */",
                ]
        return [f"/* {m} */"]

    def _lift_string_op(self, insn, m):
        if m == "movsb":
            return ["MEM8(edi) = MEM8(esi); esi++; edi++; /* movsb */"]
        if m == "movsd":
            return ["MEM32(edi) = MEM32(esi); esi += 4; edi += 4; /* movsd */"]
        if m == "stosb":
            return ["MEM8(edi) = LO8(eax); edi++; /* stosb */"]
        if m == "stosd":
            return ["MEM32(edi) = eax; edi += 4; /* stosd */"]
        if m == "lodsb":
            return ["SET_LO8(eax, MEM8(esi)); esi++; /* lodsb */"]
        if m == "lodsd":
            return ["eax = MEM32(esi); esi += 4; /* lodsd */"]
        if m == "movsw":
            return ["MEM16(edi) = MEM16(esi); esi += 2; edi += 2; /* movsw */"]
        if m == "stosw":
            return ["MEM16(edi) = LO16(eax); edi += 2; /* stosw */"]
        if m == "lodsw":
            return ["SET_LO16(eax, MEM16(esi)); esi += 2; /* lodsw */"]
        return [f"/* {m} */"]

    # ── FPU (x87) ──

    # ── SSE (scalar/packed float) ──

    def _lift_sse(self, insn, m, ops):
        """Translate SSE instructions to C float operations."""
        nops = len(ops)
        if nops < 1:
            return [f"/* {m}: no operands */"]

        def _is_xmm(op):
            return op.type == "reg" and op.reg and op.reg.startswith("xmm")

        def _is_mmx(op):
            return (op.type == "reg" and op.reg and op.reg.startswith("mm")
                    and not op.reg.startswith("xmm"))

        # ── Scalar (lane 0) access ──
        # movss/addss/... genuinely operate on one 32-bit value, so they read
        # and write lane 0 explicitly rather than the whole register.
        def _sse_read(op):
            if _is_xmm(op):
                return f"{op.reg}.f[0]"
            elif op.type == "reg":
                return op.reg
            elif op.type == "mem":
                if op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)})"
                return f"MEMF({_fmt_mem(op)})"
            elif op.type == "imm":
                return _fmt_imm(op.imm)
            return f"/* sse_read? */"

        def _sse_write(op, val):
            if _is_xmm(op):
                return f"{op.reg}.f[0] = {val};"
            elif op.type == "reg":
                return f"{op.reg} = {val};"
            elif op.type == "mem":
                if op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)}) = {val};"
                return f"MEMF({_fmt_mem(op)}) = {val};"
            return f"/* sse_write? */;"

        # ── Packed (128-bit) access ──
        # A whole-register read yields a RecompXmm value; a whole-register
        # write is a statement. Memory goes through the guest translation.
        def _packed_read(op):
            if _is_xmm(op):
                return op.reg
            if op.type == "mem":
                return f"XMM_MEM({_fmt_mem(op)})"
            return None

        def _packed_write(op, val):
            if _is_xmm(op):
                return f"{op.reg} = {val};"
            if op.type == "mem":
                return f"XMM_STORE({_fmt_mem(op)}, {val});"
            return None

        def _packed_binary(helper):
            """dst = helper(dst, src) for a two-operand packed op."""
            if nops < 2:
                return None
            a = _packed_read(ops[0])
            b = _packed_read(ops[1])
            if a is None or b is None:
                return None
            written = _packed_write(ops[0], f"{helper}({a}, {b})")
            if written is None:
                return None
            return [written + f" /* {m} */"]

        # ── Moves ──
        # movaps/movups move all 16 bytes. Treating them like movss was the
        # defect that left every packed value 4 bytes wide.
        if m in ("movaps", "movups", "movapd", "movupd"):
            if nops >= 2:
                src = _packed_read(ops[1])
                if src is not None:
                    written = _packed_write(ops[0], src)
                    if written is not None:
                        return [written + f" /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        # movlps/movhps transfer 8 bytes into or out of one half.
        if m in ("movlps", "movhps"):
            half = "LOW" if m == "movlps" else "HIGH"
            if nops >= 2:
                if _is_xmm(ops[0]) and ops[1].type == "mem":
                    return [f"XMM_LOAD_{half}({ops[0].reg}, "
                            f"{_fmt_mem(ops[1])}); /* {m} */"]
                if ops[0].type == "mem" and _is_xmm(ops[1]):
                    return [f"XMM_STORE_{half}({_fmt_mem(ops[0])}, "
                            f"{ops[1].reg}); /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        if m in ("movlhps", "movhlps"):
            helper = ("XMM_MOVE_LOW_TO_HIGH" if m == "movlhps"
                      else "XMM_MOVE_HIGH_TO_LOW")
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]

        # movss/movsd are scalar. Loading from memory zeroes the upper lanes;
        # a register-to-register move leaves them untouched.
        if m in ("movss", "movsd"):
            if nops >= 2:
                scalar = ("XMM_SCALAR" if m == "movss"
                          else "XMM_SCALAR_DOUBLE")
                if _is_xmm(ops[0]) and ops[1].type == "mem":
                    return [f"{ops[0].reg} = "
                            f"{scalar}({_sse_read(ops[1])}); /* {m} */"]
                src = _sse_read(ops[1])
                return [_sse_write(ops[0], src) + f" /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        # movd moves 32 bits without converting. Into an XMM register it also
        # zeroes the upper lanes. The generated code redefines memcpy as a
        # guest-to-guest copy, so the bits are moved through the union instead
        # of taking the address of a host local.
        if m == "movd":
            if nops >= 2:
                if _is_xmm(ops[0]):
                    if _is_xmm(ops[1]):
                        src = f"{ops[1].reg}.u[0]"
                    elif _is_mmx(ops[1]):
                        src = f"(uint32_t){ops[1].reg}"
                    else:
                        src = _fmt_operand_read(ops[1])
                    return [f"{ops[0].reg} = XMM_SCALAR_BITS({src});"
                            " /* movd to xmm */"]
                if _is_xmm(ops[1]):
                    return [f"{_fmt_operand_write(ops[0], ops[1].reg + '.u[0]')}"
                            " /* movd */"]
                src = _fmt_operand_read(ops[1])
                return [f"{_fmt_operand_write(ops[0], src)} /* movd */"]
            return [f"/* movd {insn.op_str} */"]

        # ── Arithmetic ──
        if m in ("addss", "addsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} + {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("subss", "subsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} - {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("mulss", "mulsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} * {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("divss", "divsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} / {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("sqrtss", "sqrtsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"sqrtf({_sse_read(ops[1])})") + f" /* {m} */"]
        if m in ("minss", "minsd"):
            if nops >= 2:
                a, b = _sse_read(ops[0]), _sse_read(ops[1])
                return [_sse_write(ops[0], f"({a} < {b} ? {a} : {b})") + f" /* {m} */"]
        if m in ("maxss", "maxsd"):
            if nops >= 2:
                a, b = _sse_read(ops[0]), _sse_read(ops[1])
                return [_sse_write(ops[0], f"({a} > {b} ? {a} : {b})") + f" /* {m} */"]

        # ── Packed arithmetic ──
        # These used to emit a bare comment, so a matrix concatenation built
        # from shufps + mulps + addps executed as nothing at all.
        if m in ("addps", "subps", "mulps", "divps"):
            helper = {"addps": "XMM_ADD", "subps": "XMM_SUB",
                      "mulps": "XMM_MUL", "divps": "XMM_DIV"}[m]
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]

        # ── Conversions ──
        if m == "cvtsi2ss":
            if nops >= 2:
                src = _fmt_operand_read(ops[1])
                return [_sse_write(ops[0], f"(float)(int32_t){src}") + " /* cvtsi2ss */"]
        if m in ("cvtss2si", "cvttss2si"):
            if nops >= 2:
                return [_fmt_operand_write(ops[0], f"(int32_t){_sse_read(ops[1])}") + f" /* {m} */"]
        if m == "cvtsi2sd":
            if nops >= 2:
                src = _fmt_operand_read(ops[1])
                return [_sse_write(ops[0], f"(double)(int32_t){src}") + " /* cvtsi2sd */"]
        if m in ("cvtsd2si", "cvttsd2si"):
            if nops >= 2:
                return [_fmt_operand_write(ops[0], f"(int32_t){_sse_read(ops[1])}") + f" /* {m} */"]
        if m == "cvtss2sd":
            if nops >= 2:
                return [_sse_write(ops[0], f"(double){_sse_read(ops[1])}") + " /* cvtss2sd */"]
        if m == "cvtsd2ss":
            if nops >= 2:
                return [_sse_write(ops[0], f"(float){_sse_read(ops[1])}") + " /* cvtsd2ss */"]

        # ── Comparison ──
        if m in ("comiss", "comisd", "ucomiss", "ucomisd"):
            if nops >= 2:
                return [f"/* {m} {_sse_read(ops[0])}, {_sse_read(ops[1])} - sets EFLAGS */"]

        # ── Bitwise ──
        # Done on the integer lanes: these carry sign-mask and select idioms
        # (fabs, negate, blend) that are meaningless as float arithmetic.
        if m in ("xorps", "xorpd"):
            if (nops >= 2 and _is_xmm(ops[0]) and _is_xmm(ops[1])
                    and ops[0].reg == ops[1].reg):
                return [f"{ops[0].reg} = XMM_ZERO(); /* {m} self = zero */"]
            lifted = _packed_binary("XMM_XOR")
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]
        if m in ("andps", "orps", "andnps"):
            helper = {"andps": "XMM_AND", "orps": "XMM_OR",
                      "andnps": "XMM_ANDN"}[m]
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]

        # ── Packed min/max ──
        if m in ("minps", "maxps"):
            helper = "XMM_MIN" if m == "minps" else "XMM_MAX"
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]

        # ── Reciprocal / rsqrt ──
        if m == "rsqrtss":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / sqrtf({_sse_read(ops[1])})") + " /* rsqrtss */"]
        if m == "rcpss":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / {_sse_read(ops[1])}") + " /* rcpss */"]

        # ── Packed sqrt / reciprocal / rsqrt ──
        # The SSE model here tracks only the low lane as a single float, so
        # these compute the low lane like their scalar ...ss forms rather than
        # all four. That is the same low-lane approximation the packed
        # arithmetic ops (addps/mulps) already use -- but computing the low lane
        # is strictly better than the TODO no-op these used to hit, which left
        # the destination stale and fed garbage into vector normalisation.
        # rsqrtps/sqrtps are the workhorse of 3D vector normalize; Wreckless
        # uses them heavily, Burnout 3 did not, which is why this surfaced now.
        if m == "sqrtps":
            if nops >= 2:
                return [_sse_write(ops[0], f"sqrtf({_sse_read(ops[1])})")
                        + " /* sqrtps (low lane; 4-lane model TODO) */"]
        if m == "rsqrtps":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / sqrtf({_sse_read(ops[1])})")
                        + " /* rsqrtps (low lane; 4-lane model TODO) */"]
        if m == "rcpps":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / {_sse_read(ops[1])}")
                        + " /* rcpps (low lane; 4-lane model TODO) */"]

        # ── Packed comparison ──
        if m in ("cmpneqps", "cmpeqps", "cmpltps", "cmpleps"):
            helper = {"cmpeqps": "XMM_CMP_EQ", "cmpltps": "XMM_CMP_LT",
                      "cmpleps": "XMM_CMP_LE", "cmpneqps": "XMM_CMP_NEQ"}[m]
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]

        # ── Move mask ──
        # This feeds branches, so a hardcoded 0 silently picked one side.
        if m == "movmskps":
            if nops >= 2:
                src = _packed_read(ops[1])
                if src is not None:
                    return [_fmt_operand_write(ops[0], f"XMM_MOVEMASK({src})")
                            + f" /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        # ── MMX / integer SIMD ──
        if m in ("pand", "pandn", "por", "pxor", "pcmpgtd"):
            if nops >= 2:
                return [f"/* {m} {insn.op_str} (MMX/SIMD integer) */"]

        # ── Shuffle/unpack ──
        # shufps is the broadcast in every matrix concatenation.
        if m == "shufps":
            if nops >= 3 and ops[2].type == "imm":
                a = _packed_read(ops[0])
                b = _packed_read(ops[1])
                if a is not None and b is not None:
                    written = _packed_write(
                        ops[0],
                        f"XMM_SHUFFLE({a}, {b}, {_fmt_imm(ops[2].imm)})")
                    if written is not None:
                        return [written + f" /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]
        if m in ("unpcklps", "unpckhps"):
            helper = ("XMM_UNPACK_LOW" if m == "unpcklps"
                      else "XMM_UNPACK_HIGH")
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]

        return [f"/* SSE: {m} {insn.op_str} */"]

    # ── FPU (x87) ──

    @staticmethod
    def _st_index(reg):
        """FPU register index from a capstone name like 'st(2)' or 'st2'."""
        mm = re.search(r"st\(?(\d+)\)?", reg or "")
        return int(mm.group(1)) if mm else 0

    @staticmethod
    def _st_expr(i):
        """C expression for FPU register st(i) relative to the current top."""
        if i == 0:
            return "fp_top()"
        if i == 1:
            return "fp_st1()"
        return f"g_fp_stack[(g_fp_top + {i}) & 7]"

    def _fcom_rhs(self, ops):
        """The value an fcom-family instruction compares st0 against.

        `fcom`/`fcomp`/`fcompp` with no operand compare st0 with st1. With a
        memory operand they compare st0 with that float/double. With an st(i)
        operand, that register. Returning fp_st1() unconditionally (the old
        behavior) is only right for the no-operand form.
        """
        if not ops:
            return "fp_st1()"
        op = ops[0]
        if getattr(op, "type", None) == "mem":
            if getattr(op, "mem_size", 4) == 8:
                return f"MEMD({_fmt_mem(op)})"
            return f"MEMF({_fmt_mem(op)})"
        if getattr(op, "type", None) == "reg" and op.reg:
            return self._st_expr(self._st_index(op.reg))
        return "fp_st1()"

    def _lift_fpu(self, insn, m, ops):
        """Basic FPU instruction translation using double locals."""
        # FPU is complex. We translate common patterns to double operations.
        # Full accuracy would require an x87 stack emulator.

        if m == "fld":
            if len(ops) >= 1:
                if ops[0].type == "mem":
                    if ops[0].mem_size == 4:
                        return [f"fp_push(MEMF({_fmt_mem(ops[0])})); /* fld float */"]
                    elif ops[0].mem_size == 8:
                        return [f"fp_push(MEMD({_fmt_mem(ops[0])})); /* fld double */"]
                    return [f"fp_push(MEMF({_fmt_mem(ops[0])})); /* fld */"]
                if ops[0].type == "reg":
                    # fld st(i) pushes a COPY of st(i). Was a no-op comment,
                    # which silently dropped a stack slot -- sub_00109150 (the
                    # world_to_view builder) duplicates values with fld st(0)
                    # three times. Capture the value first: fp_push mutates
                    # g_fp_top, so passing fp_top() directly would read the
                    # slot after the decrement.
                    src = self._st_expr(self._st_index(ops[0].reg))
                    return [f"{{ double _t = {src}; fp_push(_t); }}"
                            f" /* fld {insn.op_str} */"]
            return [f"/* fld {insn.op_str} */"]

        if m in ("fst", "fstp"):
            # Only fstp pops. fst stores st0 and leaves the stack alone. The old
            # code emitted fp_pop() for BOTH -- fp_pop() is g_fp_top++, a real
            # pop, not a no-op -- so every `fst [mem]` (store WITHOUT pop) shrank
            # the FP stack by one. valid_real_matrix4x3 does `fst [tmp]` before
            # its fabs/fcompare, so the compare ran on an emptied stack slot and
            # rejected orthonormal camera matrices at render_cameras.c:458.
            do_pop = " fp_pop();" if m == "fstp" else ""
            if len(ops) >= 1 and ops[0].type == "mem":
                pop = " fp_pop();" if m == "fstp" else ""
                if ops[0].mem_size == 4:
                    return [f"MEMF({_fmt_mem(ops[0])}) = (float)fp_top();{do_pop} /* {m} */"]
                elif ops[0].mem_size == 8:
                    return [f"MEMD({_fmt_mem(ops[0])}) = fp_top();{do_pop} /* {m} */"]
            # fst/fstp st(i): copy st0 to st(i); fstp then pops. This used to be
            # a bare comment -- a no-op -- which LEAKS the FPU stack. `fstp st(0)`
            # is the common idiom for "pop the value fptan/fsincos just pushed";
            # dropping it left every later float op one slot off. That is why
            # Halo's field-of-view came out 0.
            if len(ops) >= 1 and ops[0].type == "reg" and ops[0].reg:
                mm = re.search(r"st\(?(\d+)\)?", ops[0].reg)
                idx = int(mm.group(1)) if mm else 0
                parts = []
                if idx != 0:   # i==0 is a self-copy; skip, just (maybe) pop
                    dst = "fp_st1()" if idx == 1 else \
                          f"g_fp_stack[(g_fp_top + {idx}) & 7]"
                    parts.append(f"{dst} = fp_top();")
                if m == "fstp":
                    parts.append("fp_pop();")
                body = " ".join(parts) if parts else "(void)0;"
                return [f"{body} /* {m} st({idx}) */"]
            return [f"/* {m} {insn.op_str} */"]

        if m == "fild":
            if len(ops) >= 1 and ops[0].type == "mem":
                smem = _smem_accessor(
                    ops[0].mem_size, getattr(ops[0], "mem_segment", None))
                return [f"fp_push((double){smem}({_fmt_mem(ops[0])})); /* fild */"]
            return [f"/* fild {insn.op_str} */"]

        if m in ("fist", "fistp"):
            if len(ops) >= 1 and ops[0].type == "mem":
                size = ops[0].mem_size
                mem_acc = _smem_accessor(
                    size, getattr(ops[0], "mem_segment", None))
                int_type = {2: "int16_t", 4: "int32_t", 8: "int64_t"}.get(
                    size, "int32_t")
                pop = " fp_pop();" if m == "fistp" else ""
                return [f"{mem_acc}({_fmt_mem(ops[0])}) = "
                        f"({int_type})llrint(fp_top());{pop} /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        if m in ("fadd", "faddp", "fsub", "fsubp", "fsubr", "fsubrp",
                 "fmul", "fmulp", "fdiv", "fdivp", "fdivr", "fdivrp",
                 "fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr"):
            # x87 binary arithmetic, operand-aware. The old handlers hardcoded
            # the no-operand pop form (fp_st1() op= fp_top(); pop) for every
            # variant, so `fmul [mem]`, `fadd st(0),st(0)`, and the reversed
            # fsubr/fdivr were all wrong -- and fsubr/fdivr fell through to a
            # no-op. That corrupted the FPU stack on any float that used a
            # memory or register operand; Halo's field-of-view chain multiplied
            # by a constant with `fmul [k]` and came out 0.
            # The fi* forms are the same operations against a signed integer
            # in memory (fiadd -> fadd, fisubr -> fsubr). They are memory-only
            # and never pop.
            integer_operand = m.startswith("fi")
            base = m[:-1] if m.endswith("p") else m       # strip pop suffix
            if integer_operand:
                base = "f" + base[2:]
            reverse = base.endswith("r")                  # fsubr / fdivr
            core = base[:-1] if reverse else base         # fsub / fdiv / fadd / fmul
            cop = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/"}[core]
            pops = m.endswith("p")

            def _combine(dst, src):
                if cop in ("+", "*") or not reverse:
                    return f"{dst} = {dst} {cop} {src};"
                return f"{dst} = {src} {cop} {dst};"   # reversed sub/div

            # Memory operand: dst is st0, no pop (memory forms never pop).
            if ops and ops[0].type == "mem":
                if integer_operand:
                    accessor = "SMEM16" if ops[0].mem_size == 2 else "SMEM32"
                    rhs = f"(double){accessor}({_fmt_mem(ops[0])})"
                else:
                    rhs = (f"MEMD({_fmt_mem(ops[0])})" if ops[0].mem_size == 8
                           else f"MEMF({_fmt_mem(ops[0])})")
                return [f"{_combine('fp_top()', rhs)} /* {m} {insn.op_str} */"]

            # Two register operands: fXXX st(i), st(j).
            if len(ops) >= 2 and ops[0].type == "reg" and ops[1].type == "reg":
                di, si = self._st_index(ops[0].reg), self._st_index(ops[1].reg)
                code = _combine(self._st_expr(di), self._st_expr(si))
                if pops:
                    code += " fp_pop();"
                return [f"{code} /* {m} {insn.op_str} */"]

            # One register operand. Capstone reports the pop forms this way too
            # -- `faddp st(1)` comes through as a single operand st(1), NOT as
            # two operands and NOT as the no-operand form. The pop variants put
            # the result in st(i) and pop; only the non-pop variants target st0:
            #   fadd  st(i)  ->  st0  = st0  op st(i)          (no pop)
            #   faddp st(i)  ->  st(i) = st(i) op st0 ; pop
            # Treating faddp st(i) as the no-pop st0 form left the FP stack one
            # slot deep AND wrote the wrong slot, so a normalize's sum of squares
            # double-counted -- a unit vector got length sqrt(2) and Halo's
            # world_to_view basis came out scaled by 1/sqrt(2) (0.707), failing
            # valid_real_matrix4x3.
            if len(ops) >= 1 and ops[0].type == "reg":
                si = self._st_index(ops[0].reg)
                if pops:
                    code = _combine(self._st_expr(si), "fp_top()") + " fp_pop();"
                else:
                    code = _combine("fp_top()", self._st_expr(si))
                return [f"{code} /* {m} {insn.op_str} */"]

            # No operand: the stack pop form, st1 op= st0, pop.
            code = _combine("fp_st1()", "fp_top()") + " fp_pop();"
            return [f"{code} /* {m} */"]
        if m == "fchs":
            return [f"fp_top() = -fp_top(); /* fchs */"]
        if m == "fabs":
            return [f"fp_top() = fabs(fp_top()); /* fabs */"]
        if m == "fsqrt":
            return [f"fp_top() = sqrt(fp_top()); /* fsqrt */"]
        # x87 transcendentals. None of these were implemented, so every one fell
        # through to the unknown-op path and left the FP stack untouched --
        # silently, because an unimplemented FPU op looks exactly like an
        # instruction that only had a side effect on the status word.
        #
        # Halo 2276 alone has 123 fcos, 108 fsin, 31 fpatan, 8 fptan, 5 fyl2x,
        # 3 f2xm1 and 1 fprem. Every camera matrix and rotation in the game goes
        # through them, which is why render_camera_build_frustum asserted on
        # valid_real_matrix4x3: the matrix was built out of tangents that were
        # never computed, so it held whatever had been in those globals before.
        #
        # These are exact-enough mappings onto libm. The 80-bit intermediates of
        # real x87 are not reproduced -- fp_stack is double -- which is the same
        # approximation every other op here already makes.
        if m == "fsin":
            return [f"fp_top() = sin(fp_top()); /* fsin */"]
        if m == "fcos":
            return [f"fp_top() = cos(fp_top()); /* fcos */"]
        if m == "fsincos":
            # Replaces st0 with sin, then pushes cos. Order matters: the push
            # must see the sine already stored.
            return [f"{{ double _a = fp_top(); fp_top() = sin(_a);"
                    f" fp_push(cos(_a)); }} /* fsincos */"]
        if m == "fptan":
            # st0 = tan(st0), then push 1.0. The constant push is not decoration:
            # callers use it as the denominator of a subsequent fdiv.
            return [f"{{ fp_top() = tan(fp_top()); fp_push(1.0); }} /* fptan */"]
        if m == "fpatan":
            # st1 = atan2(st1, st0), pop. Argument order is st1 over st0.
            return [f"{{ fp_st1() = atan2(fp_st1(), fp_top()); fp_pop(); }}"
                    f" /* fpatan */"]
        if m == "fyl2x":
            # st1 = st1 * log2(st0), pop.
            return [f"{{ fp_st1() = fp_st1() * log2(fp_top()); fp_pop(); }}"
                    f" /* fyl2x */"]
        if m == "fyl2xp1":
            return [f"{{ fp_st1() = fp_st1() * log2(fp_top() + 1.0); fp_pop(); }}"
                    f" /* fyl2xp1 */"]
        if m == "f2xm1":
            return [f"fp_top() = exp2(fp_top()) - 1.0; /* f2xm1 */"]
        if m in ("fprem", "fprem1"):
            # Both leave the remainder in st0 and clear C2 to say "complete".
            # fprem truncates toward zero, fprem1 rounds to nearest (IEEE), which
            # is the difference between fmod and remainder.
            fn = "fmod" if m == "fprem" else "remainder"
            return [f"fp_top() = {fn}(fp_top(), fp_st1()); /* {m} */"]
        if m == "fscale":
            return [f"fp_top() = ldexp(fp_top(), (int)fp_st1()); /* fscale */"]
        if m == "frndint":
            return [f"fp_top() = rint(fp_top()); /* frndint */"]
        if m == "fldpi":
            return [f"fp_push(3.14159265358979323846); /* fldpi */"]
        if m == "fldl2e":
            return [f"fp_push(1.44269504088896340736); /* fldl2e */"]
        if m == "fldl2t":
            return [f"fp_push(3.32192809488736234787); /* fldl2t */"]
        if m == "fldlg2":
            return [f"fp_push(0.30102999566398119521); /* fldlg2 */"]
        if m == "fldln2":
            return [f"fp_push(0.69314718055994530942); /* fldln2 */"]
        if m == "ftst":
            return [f"g_fp_cmp = (fp_top() < 0.0) ? -1 : "
                    f"(fp_top() > 0.0) ? 1 : 0; /* ftst */"]
        if m == "fxch":
            # fxch st(i) swaps st0 with st(i); the bare form is st(1). Was
            # hardcoded to st1, so fxch st(2)/st(3)/st(4) (86/7/1 sites in Halo)
            # swapped the wrong slot -- corrupting the FP stack in the camera
            # basis builder that feeds world_to_view.
            # Capstone reports fxch with BOTH operands -- (st(0), st(i)) -- and
            # it is the only x87 form that does; fadd/fcom/fld st(i) all come
            # through with the explicit register alone. Reading ops[0] therefore
            # picked up the implicit st(0) and emitted a swap of st0 with
            # itself, so every `fxch st(i)` was a silent no-op.
            i = (self._st_index(ops[-1].reg)
                 if (ops and ops[-1].type == "reg" and ops[-1].reg) else 1)
            dst = self._st_expr(i)
            return [f"{{ double _t = fp_top(); fp_top() = {dst}; {dst} = _t; }}"
                    f" /* fxch {insn.op_str} */"]
        if m in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp"):
            # Compare st0 against the operand, not always st1. `fcomp [mem]`
            # compares st0 with the memory value; only the no-operand form
            # compares st0 with st1. Getting this wrong made every float compare
            # against a constant read a garbage st1 -- Halo's camera FOV and
            # world_to_view checks both fed on it.
            rhs = self._fcom_rhs(ops)
            # Pop count is in the mnemonic and was being ignored: fcom/fucom pop
            # nothing, fcomp/fucomp pop once, fcompp/fucompp pop twice. Emitting
            # zero pops for every form leaked a stack slot on each fcomp -- and
            # float compares are everywhere -- so g_fp_top drifted and later fld
            # st(i)/faddp read the wrong slots. valid_real_matrix4x3 calls the
            # leaking per-vector check three times, then its own dot products ran
            # on a drifted stack: an orthonormal matrix failed non-deterministically
            # at render_cameras.c:458. Compare first (rhs may be fp_st1()), then pop.
            npop = 2 if m.endswith("pp") else (1 if m.endswith("p") else 0)
            pops = " fp_pop();" * npop
            return [f"g_fp_cmp = RECOMP_FCMP(fp_top(), {rhs});"
                    f"{pops} /* {m} {insn.op_str} */"]
        if m in ("fcompi", "fcomip", "fucomi", "fucompi", "fucomip", "fcomi"):
            # These set EFLAGS directly (CF, ZF, PF) from FPU comparison
            # fcompi/fucompi pop st(0) after comparing; fcomi/fucomi do not
            pops = m.endswith("pi") or m.endswith("ip")
            pop_code = " fp_pop();" if pops else ""
            rhs = self._fcom_rhs(ops)
            return [f"g_fp_cmp = RECOMP_FCMP(fp_top(), {rhs});"
                    f"{pop_code} /* {m} */"]
        if m == "fnstsw":
            # `fnstsw ax` after an FPU compare is half of the pre-SSE float
            # branch idiom `fcomp; fnstsw ax; test ah, mask; j(p/np/z/nz)`.
            # Put the compare's C3/C2/C0 condition bits into ah so the following
            # test reads a real value instead of stale eax. g_fp_cmp is -1/0/1
            # for st0 </=/> src; the FPU sets C3 on equal (ah bit 6 = 0x40) and
            # C0 on less-than (ah bit 0 = 0x01), C2 only on unordered (NaN),
            # which non-NaN game math does not hit.
            # The status word also carries TOP in bits 11-13, which lands in
            # AH bits 3-5. We model TOP (it is g_fp_top), so emit it: leaving it
            # out made `fnstsw ax` disagree with the hardware on every AH read
            # taken while the stack was non-empty. The exception-flag byte (AL)
            # we do not model and it is zero after masked, clean operations.
            # C3/C2/C0 as the hardware sets them: unordered is C3|C2|C0, and
            # `test ah, 0x44; jp` -- the standard isnan idiom -- reads exactly
            # those two bits. Reporting equal for a NaN compare sends every
            # float classification in a title down the wrong branch.
            status = ("(uint16_t)(((g_fp_top & 7u) << 11) |"
                      " (g_fp_cmp == 2 ? 0x4500u :"
                      " g_fp_cmp < 0 ? 0x0100u :"
                      " g_fp_cmp > 0 ? 0x0000u : 0x4000u))")
            if insn.op_str.strip() in ("ax", "eax"):
                # `fnstsw ax` writes the whole of AX, not just AH.
                return [f"eax = (eax & 0xFFFF0000u) | (uint32_t){status};"
                        " /* fnstsw ax <- fpu status */"]
            if ops and ops[0].type == "mem":
                return [f"MEM16({_fmt_mem(ops[0])}) = {status};"
                        f" /* fnstsw {insn.op_str} */"]
            if ops and ops[0].type == "reg":
                return [_fmt_set_reg(ops[0].reg, status)
                        + f" /* fnstsw {insn.op_str} */"]
            return [f"/* fnstsw {insn.op_str} - no destination operand */"]
        if m == "fnstcw":
            # The CRT reads the control word back to decide whether an
            # exception is masked, so it must be stored, not dropped.
            if ops and ops[0].type == "mem":
                return [f"MEM16({_fmt_mem(ops[0])}) = g_fp_control_word;"
                        f" /* fnstcw {insn.op_str} */"]
            if ops and ops[0].type == "reg":
                return [_fmt_set_reg(ops[0].reg, "g_fp_control_word")
                        + f" /* fnstcw {insn.op_str} */"]
            return [f"/* fnstcw {insn.op_str} - no destination operand */"]
        if m == "fldcw":
            if ops and ops[0].type == "mem":
                return [f"g_fp_control_word = MEM16({_fmt_mem(ops[0])});"
                        f" /* fldcw {insn.op_str} */"]
            if ops and ops[0].type == "reg":
                return [f"g_fp_control_word = (uint16_t)"
                        f"{_fmt_operand_read(ops[0])};"
                        f" /* fldcw {insn.op_str} */"]
            return [f"/* fldcw {insn.op_str} - no source operand */"]
        if m == "fldz":
            return [f"fp_push(0.0); /* fldz */"]
        if m == "fld1":
            return [f"fp_push(1.0); /* fld1 */"]

        return [f"/* FPU: {m} {insn.op_str} */"]


def lift_basic_block(lifter, bb, flag_state=None):
    """
    Lift a basic block to C statements.
    Tracks flags to generate proper conditions for jcc/setcc/cmovcc.

    Args:
        lifter: Lifter instance
        bb: BasicBlock with instructions
        flag_state: tuple of (flag_setter_mnemonic, flag_operands) from
                    a preceding block, or None

    Returns:
        (stmts, flag_state) where stmts is a list of C statement strings
        and flag_state is a tuple for passing to the next block.
    """
    stmts = []
    insns = bb.instructions
    i = 0

    # Track the last instruction that set flags
    if flag_state:
        last_flag_setter, last_flag_ops = flag_state
    else:
        last_flag_setter = None
        last_flag_ops = []

    while i < len(insns):
        curr = insns[i]

        # Try cmp/test + jcc pattern first (2-instruction match)
        match = try_match_cmp_jcc(insns, i, lifter=lifter)
        if match:
            stmt, consumed = match
            flag_insn = insns[i]
            # The fused form tests the operands inline, but the flags stay live
            # for any later jcc, and that one reads the snapshot. Emit the
            # snapshot here too or those temps are stale - which silently sends
            # every reusing branch the wrong way.
            if flag_insn.mnemonic in ("cmp", "test") and len(flag_insn.operands) >= 2:
                stmts.extend(lifter._snapshot_flags(
                    flag_insn, flag_insn.operands, flag_insn.mnemonic))
            stmts.append(stmt)
            last_flag_setter = flag_insn.mnemonic
            last_flag_ops = list(flag_insn.operands)
            i += consumed
            continue

        # Handle jecxz/jcxz specially (not flag-based)
        if curr.mnemonic in ("jecxz", "jcxz"):
            results = lifter._lift_jcc(curr)
            stmts.extend(results)
            i += 1
            continue

        # Check if this instruction uses flags (jcc, setcc, cmovcc)
        if curr.is_cond_jump and last_flag_setter:
            result = _make_condition(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if result:
                cond_expr, desc = result
                target = curr.jump_target
                stmt = _emit_cond_goto(
                    cond_expr, curr.mnemonic, desc, target, lifter)
                stmts.append(stmt)
                i += 1
                continue

        if (curr.mnemonic in ("sete", "setne", "setb", "setae", "setbe",
                              "seta", "setl", "setge", "setle", "setg",
                              "sets", "setns")
                and last_flag_setter and len(curr.operands) >= 1):
            cond = _make_setcc_value(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if cond:
                stmts.append(
                    _fmt_operand_write(curr.operands[0],
                                       f"({cond}) ? 1 : 0")
                    + f" /* {curr.mnemonic} */")
                i += 1
                continue

        if (curr.mnemonic in ("cmove", "cmovne", "cmovb", "cmovae",
                              "cmovbe", "cmova", "cmovl", "cmovge",
                              "cmovle", "cmovg", "cmovs", "cmovns")
                and last_flag_setter and len(curr.operands) >= 2):
            cond = _make_cmovcc_cond(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if cond:
                src = _fmt_operand_read(curr.operands[1])
                stmts.append(
                    f"if ({cond}) "
                    + _fmt_operand_write(curr.operands[0], src)
                    + f" /* {curr.mnemonic} */")
                i += 1
                continue

        # NEG sets CF when its operand is nonzero. Preserve that value when
        # a later SBB/ADC consumes it, skipping over EFLAGS-preserving
        # instructions (e.g. neg eax; push edi; sbb eax, eax).
        if curr.mnemonic == "neg":
            j = i + 1
            while (j < len(insns)
                    and insns[j].mnemonic in _EFLAGS_PRESERVE
                    and insns[j].mnemonic != "popfd"
                    and not insns[j].is_branch
                    and not insns[j].is_call
                    and not insns[j].is_ret):
                j += 1
            preserve = (j < len(insns)
                        and insns[j].mnemonic in ("sbb", "adc"))
            results = lifter._lift_neg(
                curr, curr.operands, preserve_carry=preserve)
        else:
            results = lifter.lift_instruction(insns[i])
        stmts.extend(results)

        # Track flag-setting instructions
        if curr.mnemonic in FLAG_SETTERS:
            last_flag_setter = curr.mnemonic
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic in _FLAGS_UNDEFINED:
            # Flags are undefined after these - clear tracking
            last_flag_setter = None
            last_flag_ops = []
        elif curr.mnemonic in _EFLAGS_SETTERS:
            # Additional flag-setting instructions
            last_flag_setter = curr.mnemonic
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic in _EFLAGS_PRESERVE:
            pass  # These don't affect EFLAGS
        elif curr.mnemonic in ("fcompi", "fcomip", "fucomi", "fucompi",
                                "fucomip", "fcomi"):
            # FPU compare-to-EFLAGS: sets CF, ZF, PF directly
            last_flag_setter = curr.mnemonic
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic == "sahf":
            # sahf loads AH into flags - typically after fnstsw ax
            # in the fcomp/fnstsw/sahf pattern for FPU comparisons
            last_flag_setter = "sahf"
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic.startswith("f") or curr.mnemonic.startswith("cmov"):
            pass  # FPU and already-handled CMOVcc
        elif curr.mnemonic.startswith("j"):
            pass  # Jumps don't set flags
        elif curr.mnemonic.startswith("set"):
            pass  # SETcc doesn't set flags
        elif curr.mnemonic.startswith("rep"):
            # rep movsb/movsd = data copy, preserves flags
            # repe cmpsb/repne scasb = comparison, sets flags
            rest = curr.op_str.strip() if hasattr(curr, 'op_str') else ""
            raw_m = curr.mnemonic
            _cmp_forms = ("cmpsb", "cmpsw", "cmpsd",
                          "scasb", "scasw", "scasd")
            if any(f in raw_m for f in _cmp_forms):
                last_flag_setter = raw_m
                last_flag_ops = list(curr.operands)
            elif any(f in rest for f in _cmp_forms):
                last_flag_setter = raw_m
                last_flag_ops = list(curr.operands)
            else:
                pass  # rep movs/stos = data movement, flags preserved
        else:
            # Unknown instruction - conservatively clear flag state
            last_flag_setter = None
            last_flag_ops = []

        i += 1

    out_flag_state = (last_flag_setter, last_flag_ops) if last_flag_setter else None
    return stmts, out_flag_state
