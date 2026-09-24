#!/usr/bin/env python3
"""Edit a JSRF (NA) JSRFDATA.SAV: decode a slot, edit sdData, re-encode with
the slot's existing key, re-sign, and write a NEW file.

Encode is a port of CSaveData::Encode (XBE 0x3AB60), run with the slot's
stored key instead of the game's Random() key: per byte i, lane l=i&7,
dst = key[0x40+l] + (i>>3)*8; plaintext byte p[dst] (0 if dst >= 0x3510) has
bit k moved to bit key[0x48+k], is rotated LEFT by |r| if r=key[0x20+(dst&31)]
>= 0 else RIGHT, and is stored at record[0x50+i]; key[0..0x1F] = popcount of
the encoded bytes per lane.  The plaintext is 0x3510 bytes: sdData (0x3508)
plus 8 bytes the game reads from the start of m_SaveBack; they are preserved.

usage:
  savetool.py IN.SAV --prove
  savetool.py IN.SAV -o OUT.SAV [--slot N] [edits...] [--dump]
  savetool.py IN.SAV --make-checkpoints DIR
"""
import argparse, hashlib, hmac, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import progress as P

PLAIN = 0x3510
HDD = os.path.realpath(os.path.expanduser("~/Library/Application Support/JSRF/hdd"))
OFF = dict(chapter=0x14, glob=0x54, special=0x94, events=0x34C8)
LISTS = {"chapter": 0x14, "global": 0x54, "special": 0x94}


def rotl8(b, s): return ((b << s) | (b >> (8 - s))) & 0xFF
def rotr8(b, s): return ((b >> s) | (b << (8 - s))) & 0xFF


def decode_full(rec):
    """CSaveData::Decode (0x3A920), returning all 0x3510 plaintext bytes."""
    key = rec[:0x50]
    rot = struct.unpack_from('<32b', key, 0x20)
    lane, bitp = key[0x40:0x48], key[0x48:0x50]
    cnt = [0] * 8
    out = bytearray(PLAIN)
    for i in range(PLAIN):
        dst = lane[i & 7] + (i >> 3) * 8
        b = rec[0x50 + i]
        cnt[i & 7] += bin(b).count('1')
        r = rot[dst & 0x1F]; s = abs(r) & 7
        v = rotl8(b, s) if r < 0 else rotr8(b, s)
        o = 0
        for k in range(8):
            o |= (v >> bitp[k] & 1) << k
        if dst < PLAIN:
            out[dst] = o
    ok = list(cnt) == list(struct.unpack_from('<8I', key, 0))
    return bytes(out), bytes(key), ok


def encode(plain, key):
    """CSaveData::Encode (0x3AB60) with a given key; popcounts recomputed."""
    assert len(plain) == PLAIN
    rot = struct.unpack_from('<32b', key, 0x20)
    lane, bitp = key[0x40:0x48], key[0x48:0x50]
    cnt = [0] * 8
    enc = bytearray(PLAIN)
    for i in range(PLAIN):
        l = i & 7
        dst = lane[l] + (i >> 3) * 8
        p = plain[dst] if dst < PLAIN else 0
        v = 0
        for k in range(8):
            v |= (p >> k & 1) << bitp[k]
        r = rot[dst & 0x1F]; s = abs(r) & 7
        e = rotr8(v, s) if r < 0 else rotl8(v, s)
        cnt[l] += bin(e).count('1')
        enc[i] = e
    newkey = struct.pack('<8I', *cnt) + key[0x20:0x50]
    return newkey + bytes(enc)


def sign(body):
    return hmac.new(bytes(16), body, hashlib.sha1).digest()


class Save:
    def __init__(self, data):
        assert len(data) == 0x14 + 0x4C + 3 * P.SLOTSIZE, hex(len(data))
        self.orig = data
        self.sig_ok = sign(data[20:]) == data[:20]
        self.hdr = bytearray(data[0x14:0x60])        # 'JSRF'.. + descriptors
        self.slots = []
        for n in range(3):
            rec = data[0x60 + n * P.SLOTSIZE: 0x60 + (n + 1) * P.SLOTSIZE]
            plain, key, ok = decode_full(rec)
            self.slots.append(dict(rec=rec, plain=bytearray(plain), key=key, ok=ok))

    def desc(self, n):
        return list(struct.unpack_from('<4I', self.hdr, 0x1C + 16 * n))

    def set_desc(self, n, vals):
        struct.pack_into('<4I', self.hdr, 0x1C + 16 * n, *vals)

    def build(self):
        recs = b''.join(encode(bytes(s['plain']), s['key']) for s in self.slots)
        body = bytes(self.hdr) + recs
        return sign(body) + body


# ---- edits ---------------------------------------------------------------
def bit(plain, base, idx, val):
    assert 0 <= idx < 512, idx
    w = base + 4 * (idx >> 5)
    v, = struct.unpack_from('<I', plain, w)
    v = (v | (1 << (idx & 31))) if val else (v & ~(1 << (idx & 31)))
    struct.pack_into('<I', plain, w, v)


def u32(plain, off, val): struct.pack_into('<I', plain, off, val & 0xFFFFFFFF)


def unpaint(plain, idx):
    assert 0 <= idx < 16
    base = 0x174 + idx * 80 * 2 * 4
    for i in range(160):
        u32(plain, base + 4 * i, 0xFFFFFFFF)


# Presets: list of (op, arg).  See PRESET_NOTES for provenance.
PRESETS = {
    # mssn0240 imm[8]: e032 if C20=0 -> C20=1,C21=1; nbl[9] (C21=1,M100=0,C28=0)
    # -> text + police transmission, C21=0,C22=1.  Clear C20/C21/C22 so the
    # first-visit sequence replays.  Return mission 0240 spawn 0.
    "rokkaku-intro": [("chapter", 2), ("mission", 240), ("spawn", 0),
                      ("clear-chapter", 20), ("clear-chapter", 21),
                      ("clear-chapter", 22), ("unsee", 32)],
    # mssn0243 imm[0]: e033 if C420=1 (the game reaches it by 0243 blk[0]
    # exiting 0243 -> 0243 after C400 -> C420).  Load straight into 0243.
    "rhyth-e033": [("chapter", 2), ("mission", 243), ("spawn", 0),
                   ("set-chapter", 420), ("clear-chapter", 400),
                   ("clear-chapter", 410), ("clear-chapter", 430),
                   ("unsee", 33)],
    # Poison Jam: e031 (Chuo) writes C16=1,C23=1; C200 from Chuo tag clear.
    # 0240 nbl[26] places talk char 2 if C24=1 -> TE048 -> exit 0 -> 0242,
    # 0242 blk[1] plays e034 if C24=1 and M202 (set after the chase starts).
    "pj-e034": [("chapter", 2), ("mission", 240), ("spawn", 0),
                ("set-chapter", 10), ("set-chapter", 16), ("set-chapter", 200),
                ("clear-chapter", 23), ("set-chapter", 24),
                ("clear-chapter", 25), ("clear-chapter", 26), ("clear-chapter", 27),
                ("unsee", 34)],
    # same, entering 0242 directly on load (guess: 0242 as a load target)
    "pj-e034-direct": [("chapter", 2), ("mission", 242), ("spawn", 0),
                       ("set-chapter", 10), ("set-chapter", 16), ("set-chapter", 200),
                       ("clear-chapter", 23), ("set-chapter", 24),
                       ("clear-chapter", 25), ("clear-chapter", 26), ("clear-chapter", 27),
                       ("unsee", 34)],
    # 0240 nbl[32] places talk char 4 if C26=1 -> TE025 -> exit 0 -> 0242,
    # 0242 blk[4] plays e111 if C26=1 and M202.
    "pj-e111": [("chapter", 2), ("mission", 240), ("spawn", 0),
                ("set-chapter", 10), ("set-chapter", 16), ("set-chapter", 200),
                ("clear-chapter", 23), ("clear-chapter", 24),
                ("clear-chapter", 25), ("set-chapter", 26), ("clear-chapter", 27),
                ("unsee", 111)],
    # 0240 nbl[21] places cop trigger 2 if C103=0,M110 -> blk[7] exit 1 ->
    # 0241 with C103=1: nbl[0] loads e035, blk[3] plays it after the fight
    # (M120 -> listener 0x0D -> M121).  First two fights marked done.
    "hayashi-e035": [("chapter", 2), ("mission", 240), ("spawn", 0),
                     ("set-chapter", 101), ("set-chapter", 102),
                     ("clear-chapter", 103), ("set-chapter", 106),
                     ("set-chapter", 107), ("clear-chapter", 108),
                     ("unsee", 35)],
}


def apply(save, slot, ops):
    s = save.slots[slot]; p = s['plain']
    d = save.desc(slot)
    for op, a in ops:
        if op == "chapter":
            u32(p, 0x0, a); d[1] = a
        elif op == "mission": u32(p, 0x4, a)
        elif op == "spawn": u32(p, 0x8, a)
        elif op.startswith(("set-", "clear-")):
            v, lst = op.split("-")
            bit(p, LISTS[lst], a, v == "set")
        elif op == "see": bit(p, OFF["events"], a, 1)
        elif op == "unsee": bit(p, OFF["events"], a, 0)
        elif op == "unpaint": unpaint(p, a)
        else: raise ValueError(op)
    save.set_desc(slot, d)


def check_out(path, inp):
    rp = os.path.realpath(os.path.abspath(path))
    if rp.startswith(HDD + os.sep) or rp == os.path.realpath(inp):
        sys.exit(f"refusing to write {path}: inside the HDD or same as input")


def verify(out_bytes, save, slot):
    """proof (b)+(c) on a built file."""
    new = Save(out_bytes)
    res = []
    res.append(("signature verifies", new.sig_ok))
    for n in range(3):
        res.append((f"slot {n} popcount check", new.slots[n]['ok']))
        res.append((f"slot {n} decodes to intended plaintext",
                    bytes(new.slots[n]['plain']) == bytes(save.slots[n]['plain'])))
    return res


def prove(path):
    data = open(path, 'rb').read()
    s = Save(data)
    print(f"input: {path}\n  input signature OK: {s.sig_ok}; slot popcount OK: {[x['ok'] for x in s.slots]}")
    out = s.build()
    print(f"(a) decode->encode->sign of all 3 slots reproduces file byte for byte: {out == data}"
          f"  (sha1 in {hashlib.sha1(data).hexdigest()} out {hashlib.sha1(out).hexdigest()})")
    for n in range(3):
        print(f"    slot {n} record identical: {encode(bytes(s.slots[n]['plain']), s.slots[n]['key']) == s.slots[n]['rec']}")
    before = bytes(s.slots[0]['plain'])
    apply(s, 0, PRESETS["rokkaku-intro"] + [("unpaint", 6), ("set-special", 7)])
    out2 = s.build()
    new = Save(out2)
    diff = [i for i in range(P.SDSIZE) if before[i] != new.slots[0]['plain'][i]]
    print(f"(b) after edit (rokkaku-intro + unpaint 6 + set S7): decoded slot 0 == edited plaintext: "
          f"{bytes(new.slots[0]['plain']) == bytes(s.slots[0]['plain'])}; popcount OK: {new.slots[0]['ok']}; "
          f"{len(diff)} sdData bytes changed; other slots unchanged: "
          f"{all(new.slots[n]['rec'] == Save(data).slots[n]['rec'] for n in (1, 2))}")
    print(f"(c) output HMAC-SHA1 (zero key) verifies: {new.sig_ok}")
    return out == data and new.sig_ok and new.slots[0]['ok']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("-o", "--output")
    ap.add_argument("--slot", type=int, default=0)
    ap.add_argument("--prove", action="store_true")
    ap.add_argument("--dump", action="store_true")
    ap.add_argument("--preset", action="append", default=[], choices=sorted(PRESETS))
    ap.add_argument("--make-checkpoints", metavar="DIR")
    for lst in ("chapter", "global", "special"):
        ap.add_argument(f"--set-{lst}-flag", type=int, action="append", default=[])
        ap.add_argument(f"--clear-{lst}-flag", type=int, action="append", default=[])
    ap.add_argument("--chapter", type=int); ap.add_argument("--mission", type=int)
    ap.add_argument("--spawn", type=int)
    ap.add_argument("--see-event", type=int, action="append", default=[])
    ap.add_argument("--unsee-event", type=int, action="append", default=[])
    ap.add_argument("--unpaint-stage", type=int, action="append", default=[])
    a = ap.parse_args()

    if a.prove:
        sys.exit(0 if prove(a.input) else 1)
    data = open(a.input, 'rb').read()

    if a.make_checkpoints:
        for name, ops in PRESETS.items():
            s = Save(data); apply(s, 0, ops)
            out = s.build()
            d = os.path.join(a.make_checkpoints, name); os.makedirs(d, exist_ok=True)
            fp = os.path.join(d, "JSRFDATA.SAV"); check_out(fp, a.input)
            open(fp, 'wb').write(out)
            res = verify(out, s, 0)
            bad = [k for k, v in res if not v]
            print(f"{fp}: sig/popcount/plaintext checks {'all pass' if not bad else 'FAILED: ' + ', '.join(bad)}")
            with open(os.path.join(d, "dump.txt"), 'w') as f:
                old = sys.stdout; sys.stdout = f
                print(f"preset {name}: {ops}")
                P.report(f"slot 0 after preset {name}", bytes(s.slots[0]['plain'][:P.SDSIZE]), P.DEFAULT_STAGEMAP)
                sys.stdout = old
        return

    s = Save(data)
    if not s.sig_ok:
        print("warning: input signature does not verify with the zero key", file=sys.stderr)
    ops = []
    for pr in a.preset: ops += PRESETS[pr]
    for lst in ("chapter", "global", "special"):
        ops += [(f"set-{lst}", n) for n in getattr(a, f"set_{lst}_flag")]
        ops += [(f"clear-{lst}", n) for n in getattr(a, f"clear_{lst}_flag")]
    if a.chapter is not None: ops.append(("chapter", a.chapter))
    if a.mission is not None: ops.append(("mission", a.mission))
    if a.spawn is not None: ops.append(("spawn", a.spawn))
    ops += [("see", n) for n in a.see_event] + [("unsee", n) for n in a.unsee_event]
    ops += [("unpaint", n) for n in a.unpaint_stage]
    apply(s, a.slot, ops)
    if a.output:
        check_out(a.output, a.input)
        out = s.build()
        open(a.output, 'wb').write(out)
        res = verify(out, s, a.slot)
        print(f"wrote {a.output}; checks: " + ", ".join(f"{k}={v}" for k, v in res))
    elif ops:
        print("edits given but no -o; nothing written", file=sys.stderr)
    if a.dump:
        P.report(f"slot {a.slot}", bytes(s.slots[a.slot]['plain'][:P.SDSIZE]), P.DEFAULT_STAGEMAP)


if __name__ == "__main__":
    main()
