"""Recover small shared return blocks that function discovery did not split.

A branch into another function's epilogue does not push a return address.
Replacing it with a bare-ret stub leaves saved registers on the guest stack.
Only accept instruction-aligned, straight-line, bounded return sequences;
this is not a fallback for arbitrary missing functions.
"""
from .config import is_code_address
from .lifter import Lifter


def recover_shared_epilogue(translator, address, name):
    owners = [(start, info) for start, info in translator.func_db.items()
              if start < address < info.get("end", start)]
    if not owners or not is_code_address(address):
        return None
    start, owner = max(owners, key=lambda item: item[0])
    raw = translator._read_func_bytes(start, owner["end"])
    instructions = translator.disasm.disassemble_function(raw, start, owner["end"])
    by_address = {insn.address: insn for insn in instructions}
    sequence = []
    cursor = address
    restored = False
    registers = {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"}
    for _ in range(16):
        insn = by_address.get(cursor)
        if (insn is None or insn.end_address - address > 64 or
                insn.bytes_hex.startswith(("66", "67"))):
            return None
        ops = insn.operands
        m = insn.mnemonic
        if m in ("ret", "retn"):
            if not restored or len(ops) > 1 or (ops and ops[0].type != "imm"):
                return None
            sequence.append(insn)
            break
        if m == "pop" and len(ops) == 1 and ops[0].type == "reg" and ops[0].reg in registers - {"esp"}:
            restored = True
        elif m == "leave" and not ops:
            restored = True
        elif (m == "xor" and len(ops) == 2 and
              all(op.type == "reg" and op.reg == "eax" for op in ops)):
            pass
        elif (m == "add" and len(ops) == 2 and ops[0].type == "reg" and
              ops[0].reg == "esp" and ops[1].type == "imm" and
              0 <= ops[1].imm <= 4096 and ops[1].imm % 4 == 0):
            restored = True
        elif m == "mov" and len(ops) == 2:
            dst, src = ops
            frame_reset = (dst.type == src.type == "reg" and
                           dst.reg == "esp" and src.reg == "ebp")
            return_value = dst.type == "reg" and dst.reg == "eax"
            state_store = (dst.type == "mem" and dst.mem_size in (1, 2, 4) and
                           not dst.mem_seg and dst.mem_base in registers and
                           (not dst.mem_index or dst.mem_index in registers))
            seh_restore = (dst.type == "mem" and dst.mem_size == 4 and
                           dst.mem_seg == "fs" and not dst.mem_base and
                           not dst.mem_index and dst.mem_disp == 0 and
                           src.type == "reg" and src.reg == "ecx")
            stack_load = (dst.type == "reg" and dst.reg in {"eax", "ecx", "edx"} and
                          src.type == "mem" and src.mem_size == 4 and
                          not src.mem_seg and src.mem_base in {"esp", "ebp"} and
                          not src.mem_index and -4096 <= src.mem_disp <= 4096)
            source_ok = (src.type == "imm" or
                         (src.type == "reg" and src.reg in registers))
            if not (stack_load or (source_ok and
                    (frame_reset or return_value or state_store or seh_restore))):
                return None
        else:
            return None
        sequence.append(insn)
        cursor = insn.end_address
    else:
        return None

    # A fresh lifter keeps this recovery from changing the last translated
    # function's flags, frame classification, or referenced-call collection.
    lifter = Lifter()
    lines = [f"/* Recovered shared epilogue at 0x{address:08X}: " +
             " ".join(insn.bytes_hex for insn in sequence) + " */",
             f"void {name}(void)", "{",
             "    uint32_t ebp = g_seh_ebp;",
             "    int _cf = 0; (void)_cf;"]
    for insn in sequence:
        if insn.is_ret:
            lines.append("    g_seh_ebp = ebp;")
        lines.extend("    " + line for line in lifter.lift_instruction(insn))
    lines.append("}")
    return "\n".join(lines) + "\n"
