"""
Xbox D3D8 function identification via NV2A push-buffer method constants.

Xbox titles link D3D8 statically, and the linker strips it: there are no
symbols, and in a retail build no strings either -- JSRF's D3D section holds
exactly zero. Byte signatures do not travel between titles because each XDK
version compiles a different D3D8, so the CRT approach in crt_identifier.py
does not carry over.

What does survive is what D3D8 is *for*. Every function that talks to the GPU
builds NV2A push-buffer command words inline, and a command word is

    (count << 18) | method

where `method` is a documented NV097_* register offset. Those constants are
immediates in the instruction stream, they are specific to the operation, and
they cannot be optimised away -- the hardware defines them. So a function that
writes NV097_FLIP_STALL is the buffer flip whatever the XDK version, and a
function writing NV097_SET_BEGIN_END alongside NV097_ARRAY_ELEMENT32 is an
INDEXED draw rather than a plain one.

The method table is read from src/nv2a/nv2a_regs.h, which the tree already
carries (extracted from xemu, LGPL-2.1-or-later -- see NOTICE).

Two axes are combined:

  1. WHAT the function does   -- the set of NV097 methods it writes.
  2. WHETHER it is public API -- whether anything outside the D3D section
     calls it. D3D8 entry points are called by game code; the helpers they
     use are not. An implementation reached only from inside the section is
     reported with its public entry point where the call graph gives one.

This does not identify functions that touch no GPU register: BeginScene,
EndScene and the pure state setters only write the device struct, and need a
different axis (device-context field offsets -- see
docs/technical/d3d8ltcg-device-context.md).
"""

import os
import re
import struct
from collections import defaultdict

# Push-buffer command word layout.
PUSHBUF_COUNT_SHIFT = 18
PUSHBUF_METHOD_MASK = (1 << PUSHBUF_COUNT_SHIFT) - 1

# A single command word carries at most a few dozen dwords in practice; a
# larger "count" means the immediate is not a command word at all.
PUSHBUF_MAX_COUNT = 64

# NV097 method offsets live below this. Anything above is a field mask or a
# value constant from the same header, not a method.
NV097_METHOD_LIMIT = 0x2000

# A bare method offset (no count) is only trusted above this. Below it the
# values are small enough -- 0x100, 0x304 -- to collide with ordinary
# arithmetic constants, so the encoded form is required instead.
BARE_METHOD_MIN = 0x100


def load_nv097_methods(header_path=None):
    """
    Parse NV097_* method offsets out of nv2a_regs.h.

    Top-level methods are defined at one indent ("#   define NV097_FOO"); the
    field masks and enum values nested under them use deeper indentation. That
    distinction is what keeps NV097_SET_BEGIN_END_OP_TRIANGLES (a value) from
    being matched as if it were a method.

    Returns:
        dict: method_offset (int) -> NV097 name (str)
    """
    if header_path is None:
        header_path = os.path.join(
            os.path.dirname(__file__), "..", "..", "src", "nv2a", "nv2a_regs.h")
    methods = {}
    pattern = re.compile(r"#\s{1,4}define\s+(NV097_\w+)\s+(0x[0-9A-Fa-f]+)\s*$")
    try:
        with open(header_path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = pattern.match(line)
                if not m:
                    continue
                value = int(m.group(2), 16)
                if value < NV097_METHOD_LIMIT:
                    methods.setdefault(value, m.group(1))
    except OSError:
        return {}
    return methods


def extract_immediates(func_bytes):
    """
    Collect imm32 operands from a function body.

    Byte-level like the rest of this package -- func_id carries no
    disassembler. Three encodings matter, and the third is the one that
    counts: D3D8 writes a command word straight into the push buffer with
    "mov dword ptr [edi], imm32", which imm_scanner.py explicitly skips
    because it only ever needed push/mov-reg for string references.

    Over-reading is harmless here: a false immediate has to collide with a
    real NV097 method offset AND appear in the encoded (count << 18) form to
    reach a result, which is why the bare-offset path is bounded separately.

    Returns:
        set: immediate values (int)
    """
    out = set()
    i = 0
    end = len(func_bytes)
    while i + 5 <= end:
        b = func_bytes[i]

        # push imm32
        if b == 0x68:
            out.add(struct.unpack_from("<I", func_bytes, i + 1)[0])
            i += 5
            continue

        # mov r32, imm32
        if 0xB8 <= b <= 0xBF:
            out.add(struct.unpack_from("<I", func_bytes, i + 1)[0])
            i += 5
            continue

        # mov r/m32, imm32  (0xC7 /0) -- the push-buffer store
        if b == 0xC7:
            disp_len = _modrm_imm32(func_bytes, i + 1)
            if disp_len is not None:
                off = i + 2 + disp_len
                if off + 4 <= end:
                    out.add(struct.unpack_from("<I", func_bytes, off)[0])
                    i = off + 4
                    continue

        i += 1
    return out


def _modrm_imm32(buf, pos):
    """
    Displacement length for a ModR/M byte at `pos`, or None if the operand is
    not a 32-bit memory/register form this scanner handles.

    Returns the number of bytes between the ModR/M byte and the immediate
    (SIB + displacement), so the caller can find the imm32.
    """
    if pos >= len(buf):
        return None
    modrm = buf[pos]
    mod = modrm >> 6
    rm = modrm & 0x07
    reg = (modrm >> 3) & 0x07
    if reg != 0:            # 0xC7 /0 only; /1../7 are not "mov"
        return None
    extra = 0
    if rm == 0x04:          # SIB byte follows
        extra += 1
    if mod == 0x00:
        if rm == 0x05:      # disp32, absolute
            extra += 4
        elif rm == 0x04 and pos + 1 < len(buf) and (buf[pos + 1] & 0x07) == 0x05:
            extra += 4      # SIB with no base -> disp32
    elif mod == 0x01:
        extra += 1
    elif mod == 0x02:
        extra += 4
    # mod == 0x03 is a register destination: no displacement.
    return extra


def decode_pushbuffer_methods(immediates, methods):
    """
    Map immediates to NV097 method names.

    Accepts the encoded command word (count << 18 | method), which is the
    strong form, and a bare method offset above BARE_METHOD_MIN for the cases
    where D3D8 computes the count at run time and ORs it in.

    Returns:
        set: NV097 name (str)
    """
    found = set()
    for value in immediates:
        count = value >> PUSHBUF_COUNT_SHIFT
        method = value & PUSHBUF_METHOD_MASK
        if 0 < count <= PUSHBUF_MAX_COUNT and method in methods:
            found.add(methods[method])
        elif value >= BARE_METHOD_MIN and value in methods:
            found.add(methods[value])
    return found


# Classification rules, most specific first.
#
# (name, confidence, required, forbidden, decisive)
#
# `required` are NV097 name prefixes that must ALL be present; `forbidden`
# excludes a more specific sibling, so an indexed draw -- which also writes
# SET_BEGIN_END -- cannot be claimed by the plain-draw rule.
#
# `decisive` marks a rule whose markers name ONE operation and nothing else:
# the flip pair, the begin/end bracket around a draw, a clear rectangle. Those
# are trusted however much else the function does. The rest are weak: a lone
# SET_LIGHT_AMBIENT_COLOR is real evidence in a small function and almost none
# in a large one that also programs surfaces, depth and the flip -- see
# WEAK_RULE_MAX_METHODS.
D3D8_RULES = [
    ("D3DDevice_Swap", 0.90,
     ["NV097_FLIP_STALL", "NV097_FLIP_INCREMENT_WRITE"], [], True),
    ("D3DDevice_DrawIndexedVertices", 0.85,
     ["NV097_SET_BEGIN_END", "NV097_ARRAY_ELEMENT"], [], True),
    ("D3DDevice_DrawVerticesUP", 0.80,
     ["NV097_SET_BEGIN_END", "NV097_INLINE_ARRAY"], [], True),
    ("D3DDevice_DrawVertices", 0.75,
     ["NV097_SET_BEGIN_END"],
     ["NV097_ARRAY_ELEMENT", "NV097_INLINE_ARRAY"], True),
    ("D3DDevice_Clear", 0.75,
     ["NV097_CLEAR_SURFACE"], [], True),
    ("D3DDevice_Clear", 0.60,
     ["NV097_SET_CLEAR_RECT"], [], True),
    ("D3DDevice_SetPixelShaderState", 0.70,
     ["NV097_SET_COMBINER"], [], False),
    ("D3DDevice_SetTexture", 0.65,
     ["NV097_SET_TEXTURE_OFFSET"], [], False),
    ("D3DDevice_SetTextureState", 0.55,
     ["NV097_SET_TEXTURE_CONTROL"], [], False),
    ("D3DDevice_SetLight", 0.60,
     ["NV097_SET_LIGHT"], [], False),
    ("D3DDevice_SetFogState", 0.60,
     ["NV097_SET_FOG"], [], False),
    ("D3DDevice_SetVertexShaderConstant", 0.60,
     ["NV097_SET_TRANSFORM_CONSTANT"], [], False),
    ("D3DDevice_SetStreamSource", 0.55,
     ["NV097_SET_VERTEX_DATA_ARRAY"], [], False),
    ("D3DDevice_SetRenderTarget", 0.55,
     ["NV097_SET_SURFACE_COLOR_OFFSET"], [], False),
    ("D3DDevice_BlockOnFence", 0.50,
     ["NV097_SET_SEMAPHORE_OFFSET"], [], False),
]

# Above this many distinct methods, a function is programming the pipeline
# broadly rather than implementing one entry point, and only a decisive marker
# may name it. JSRF's sub_0018D0F0 is the case that forced this: 1313 bytes
# writing 11 methods across surface setup, depth/stencil, the flip and one
# lighting register, which the weak SET_LIGHT rule was happy to call SetLight.
WEAK_RULE_MAX_METHODS = 6

# What a broad block is called instead. Reported so the caller still sees the
# function and its methods, rather than it being dropped as unidentified.
STATE_BLOCK_NAME = "D3DDevice_StateBlock"
STATE_BLOCK_CONFIDENCE = 0.30


def classify_methods(method_names):
    """
    Name a function from the NV097 methods it writes.

    Returns:
        (name, confidence) or (None, 0.0) when no rule fires.
    """
    broad = len(method_names) > WEAK_RULE_MAX_METHODS
    for name, confidence, required, forbidden, decisive in D3D8_RULES:
        if not all(_any_prefix(method_names, p) for p in required):
            continue
        if any(_any_prefix(method_names, p) for p in forbidden):
            continue
        if broad and not decisive:
            # Keep looking: a decisive rule further down still wins, and if
            # none fires this falls through to the state-block answer.
            continue
        return name, confidence
    if broad:
        return STATE_BLOCK_NAME, STATE_BLOCK_CONFIDENCE
    return None, 0.0


def _any_prefix(names, prefix):
    return any(n.startswith(prefix) for n in names)


# "mov ecx, imm32" / "mov edx, imm32" / "call rel32" -- the fastcall push.
_OP_MOV_ECX = 0xB9
_OP_CALL_REL32 = 0xE8

# How far after the "mov ecx, <command word>" the call may sit. The argument
# setup in between is a second mov and occasionally a load, so this only has
# to span a few instructions; a wider window starts pairing a constant with an
# unrelated later call.
PUSH_CALL_WINDOW = 24


def scan_pushed_methods(xbe_data, functions, methods, section=None):
    """
    Recover NV097 methods that a function pushes THROUGH A HELPER.

    D3D8 does not always inline the command word next to the store. JSRF's
    push primitive is fastcall -- sub_0018E930, "advance the ring cursor and
    write ecx:edx" -- so the method lives in the CALLER as "mov ecx, imm32"
    immediately before the call, and the callee holds no constant at all. That
    is why a body scan alone finds so few functions: 27 of 264 here, while 61
    separate call sites push state through the one primitive.

    Anchoring on the constant rather than on the primitive keeps this general.
    Any "mov ecx, <encoded command word>" shortly followed by a direct call is
    recorded, and the primitive falls out as the most frequent target rather
    than having to be recognised by shape.

    Note the callers are usually NOT in the D3D section. JSRF's render state is
    inlined into game code at 0x0015xxxx, which is a finding in its own right:
    there is no D3DDevice_SetRenderState to override, because the game is the
    state setter.

    Args:
        xbe_data: Raw bytes of the entire XBE file.
        functions: List of function dicts.
        methods: Method table from load_nv097_methods().
        section: Optional (va_lo, va_hi) to restrict the callers considered.

    Returns:
        (pushed, primitives) where `pushed` maps func_addr -> set of NV097
        names, and `primitives` maps call target -> number of method pushes
        routed through it.
    """
    pushed = defaultdict(set)
    primitives = defaultdict(int)

    # A call target is only believed if it is a function start. The forward
    # scan looks for an 0xE8 byte, and 0xE8 occurs inside other instructions'
    # encodings all the time -- without this check a constant pairs with a
    # displacement byte and yields an address like 0x24656186 that is not code
    # at all. Requiring a known entry point costs nothing and removes them.
    func_starts = {int(f["start"], 16) for f in functions}
    sections = _parse_sections(xbe_data)

    for func in functions:
        addr = int(func["start"], 16)
        if section and not (section[0] <= addr < section[1]):
            continue
        size = func.get("size") or 0
        if size <= 0:
            continue
        off = _file_offset(sections, addr, size, len(xbe_data))
        if off is None:
            continue
        body = xbe_data[off:off + size]
        names, targets = scan_body_pushes(body, addr, methods, func_starts)
        if names:
            pushed[addr] |= names
        for target, n in targets.items():
            primitives[target] += n
    return pushed, primitives


def scan_body_pushes(body, base_va, methods, func_starts):
    """
    Find "mov ecx, <command word>" ... "call rel32" pairs in one function body.

    Pure over bytes so it can be tested without an XBE around it.

    Returns:
        (set of NV097 names, dict of call target -> count)
    """
    names = set()
    targets = defaultdict(int)
    i = 0
    while i + 5 <= len(body):
        if body[i] != _OP_MOV_ECX:
            i += 1
            continue
        value = struct.unpack_from("<I", body, i + 1)[0]
        matched = decode_pushbuffer_methods({value}, methods)
        if not matched:
            i += 5
            continue
        j = i + 5
        limit = min(len(body) - 5, j + PUSH_CALL_WINDOW)
        while j <= limit:
            if body[j] == _OP_CALL_REL32:
                rel = struct.unpack_from("<i", body, j + 1)[0]
                target = (base_va + j + 5 + rel) & 0xFFFFFFFF
                if target in func_starts:
                    names |= matched
                    targets[target] += 1
                    break
            j += 1
        i += 5
    return names, dict(targets)


def _file_offset(sections, addr, size, data_len):
    """File offset of a function, or None if it is outside every section."""
    for va, vend, raw in sections:
        if va <= addr < vend:
            off = raw + (addr - va)
            return off if off + size <= data_len else None
    return None


def _parse_sections(xbe_data):
    """All (va_start, va_end, raw_addr) triples from the XBE section table."""
    out = []
    try:
        base = struct.unpack_from("<I", xbe_data, 0x104)[0]
        count = struct.unpack_from("<I", xbe_data, 0x11C)[0]
        table = struct.unpack_from("<I", xbe_data, 0x120)[0] - base
    except struct.error:
        return out
    if table < 0 or count > 64:
        return out
    for i in range(count):
        off = table + i * 0x38
        if off + 0x38 > len(xbe_data):
            break
        va = struct.unpack_from("<I", xbe_data, off + 0x04)[0]
        vsize = struct.unpack_from("<I", xbe_data, off + 0x08)[0]
        raw = struct.unpack_from("<I", xbe_data, off + 0x0C)[0]
        out.append((va, va + vsize, raw))
    return out


def find_d3d_section(xbe_data):
    """
    Locate the statically linked D3D section in an XBE.

    Read from the binary rather than config.SECTIONS, which is pinned to one
    title: Burnout 3 puts D3D at 0x0034C2E0 and JSRF at 0x0018CB40.

    Returns:
        (va_start, va_end, raw_addr) or None
    """
    try:
        base = struct.unpack_from("<I", xbe_data, 0x104)[0]
        count = struct.unpack_from("<I", xbe_data, 0x11C)[0]
        table = struct.unpack_from("<I", xbe_data, 0x120)[0] - base
    except struct.error:
        return None
    if table < 0 or count > 64:
        return None
    for i in range(count):
        off = table + i * 0x38
        if off + 0x38 > len(xbe_data):
            break
        va = struct.unpack_from("<I", xbe_data, off + 0x04)[0]
        vsize = struct.unpack_from("<I", xbe_data, off + 0x08)[0]
        raw = struct.unpack_from("<I", xbe_data, off + 0x0C)[0]
        name_off = struct.unpack_from("<I", xbe_data, off + 0x14)[0] - base
        if not (0 <= name_off < len(xbe_data)):
            continue
        end = xbe_data.find(b"\0", name_off)
        if end < 0:
            continue
        if xbe_data[name_off:end].decode("ascii", "replace") == "D3D":
            return va, va + vsize, raw
    return None


def identify_d3d8_functions(xbe_data, functions, xrefs=None, verbose=False):
    """
    Identify Xbox D3D8 functions by the NV2A methods they emit.

    Args:
        xbe_data: Raw bytes of the entire XBE file.
        functions: List of function dicts (with 'start' hex string keys).
        xrefs: Optional list of xref dicts from xrefs.json. When given, each
            result is marked public (called from outside the D3D section) or
            internal, and internal results carry the public entry point that
            reaches them.
        verbose: Print progress info.

    Returns:
        dict: func_addr (int) -> {
            "name": str,
            "confidence": float,
            "method": "d3d8_nv2a_method",
            "nv2a_methods": [str, ...],
            "public": bool,
            "public_entry": int or None,
        }
    """
    section = find_d3d_section(xbe_data)
    if section is None:
        if verbose:
            print("  No D3D section in this XBE; skipping D3D8 identification")
        return {}
    d3d_lo, d3d_hi, d3d_raw = section

    methods = load_nv097_methods()
    if not methods:
        if verbose:
            print("  nv2a_regs.h unreadable; skipping D3D8 identification")
        return {}
    if verbose:
        print(f"  D3D section 0x{d3d_lo:08X}-0x{d3d_hi:08X}, "
              f"{len(methods)} NV097 methods")

    # Method sets for every function in the section.
    per_func = {}
    for func in functions:
        addr = int(func["start"], 16)
        if not (d3d_lo <= addr < d3d_hi):
            continue
        size = func.get("size") or 0
        if size <= 0:
            continue
        off = d3d_raw + (addr - d3d_lo)
        body = xbe_data[off:off + size]
        if not body:
            continue
        names = decode_pushbuffer_methods(extract_immediates(body), methods)
        if names:
            per_func[addr] = names

    # Second pass: methods this function pushes through a fastcall helper
    # rather than storing itself. Without it the body scan sees only the
    # functions that inline their own command words.
    pushed, primitives = scan_pushed_methods(
        xbe_data, functions, methods, section=(d3d_lo, d3d_hi))
    for addr, names in pushed.items():
        per_func.setdefault(addr, set()).update(names)
    if verbose and primitives:
        top = max(primitives.items(), key=lambda kv: kv[1])
        print(f"  Push helper sub_{top[0]:08X} carries {top[1]} method pushes")

    callers = _call_graph(xrefs) if xrefs else {}
    func_starts = sorted(int(f["start"], 16) for f in functions)

    results = {}
    for addr, names in per_func.items():
        name, confidence = classify_methods(names)
        if name is None:
            continue
        public, entry = _public_entry(addr, callers, func_starts,
                                      d3d_lo, d3d_hi)
        # An entry point the game calls directly is worth more than the same
        # evidence buried in a helper: the caller boundary corroborates it.
        if public:
            confidence = min(0.95, confidence + 0.05)
        results[addr] = {
            "name": name,
            "category": "game_render",
            "subcategory": "d3d8",
            "confidence": round(confidence, 2),
            "method": "d3d8_nv2a_method",
            "nv2a_methods": sorted(names),
            "public": public,
            "public_entry": entry,
        }

    if verbose:
        pub = sum(1 for r in results.values() if r["public"])
        print(f"  Identified {len(results)} D3D8 functions "
              f"({pub} public, {len(results) - pub} internal)")
    return results


def _call_graph(xrefs):
    """Build callee -> set of call-site addresses from call xrefs."""
    callers = defaultdict(set)
    for ref in xrefs:
        if ref.get("type") != "call":
            continue
        try:
            src = int(ref["from"], 16)
            dst = int(ref["to"], 16)
        except (KeyError, ValueError):
            continue
        callers[dst].add(src)
    return callers


def _containing_function(addr, func_starts):
    """Start address of the function containing `addr`, or None."""
    import bisect
    i = bisect.bisect_right(func_starts, addr)
    return func_starts[i - 1] if i else None


def _public_entry(addr, callers, func_starts, d3d_lo, d3d_hi,
                  depth=0, seen=None):
    """
    Is this function called from outside the D3D section, and if not, which
    public function reaches it?

    xrefs record call SITES -- instruction addresses inside the caller -- so
    each site is resolved back to its containing function before walking up.
    Skipping that step makes every lookup past the first level miss, and every
    implementation function then reports "no public entry".
    """
    if not callers:
        return False, None
    seen = seen if seen is not None else set()
    if addr in seen or depth > 4:
        return False, None
    seen.add(addr)

    sites = callers.get(addr, ())
    if any(not (d3d_lo <= s < d3d_hi) for s in sites):
        return True, addr

    for site in sorted(sites):
        parent = _containing_function(site, func_starts)
        if parent is None or parent == addr:
            continue
        found, entry = _public_entry(parent, callers, func_starts,
                                     d3d_lo, d3d_hi, depth + 1, seen)
        if found:
            return False, entry
    return False, None
