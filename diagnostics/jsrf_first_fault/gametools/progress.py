#!/usr/bin/env python3
"""Decode JSRF (NA standalone) story progress: sdData from a CSaveData memory
dump (guest 0x1EFFB0, vtable word first) or from JSRFDATA.SAV on the HDD.

Layout: JSRF-Decompilation decompile/src/JSRF/SaveData.hpp (struct sdData,
0x3508 bytes).  On-disk format: from the NA XBE's CSaveData::Decode (0x3A920)
and CXboxSaveManager::SignFile (0x46C40); see decode_slot()/read_sav().

usage: progress.py FILE [--stagemap mem-N-1efc88.bin] [--raw]
"""
import hashlib, hmac, os, struct, sys

SDSIZE = 0x3508
SLOTSIZE = 0x50 + SDSIZE + 8          # CSaveData::GetSaveDataSize
NSLOTS = 3

CHARS = ["Beat", "Corn", "Gum", "Combo", "Rhyth", "Soda", "Yoyo", "NT-3000",
         "Garam", "Boogie", "Cube", "Love Shockers", "Poison Jam",
         "Noise Tank", "Immortals", "Doom Riders", "Rapid 99", "Potts",
         "Gouji", "Roboy", "Clutch", "Jazz", "A-KU-MU", "Zero Beat"]

# GG-Notebook stages/stage_ids.md
STAGE_NAMES = {
    0: "Garage", 10: "Shibuya Terminal", 11: "Chuo Street",
    12: "Dogenzaka Hill", 13: "Hikage Street", 20: "Rokkaku-dai Heights",
    21: "Tokyo Underground Sewage Facility", 22: "Kibogaoka Hill",
    23: "Bottom Point of Sewage Facility", 24: "Fortified Residential Zone",
    30: "99th Street", 31: "Sky Dinosaurian Square",
    32: "Future Site of Rokkaku Expo Stadium", 33: "Highway Zero",
    34: "Skyscraper District and Pharaoh Park",
}
# g_stageIdToStageIndex @ 0x1EFC88 (identical in the XBE .data image and in
# the live dumps): index = table[major%10*10 + minor%10]; unlisted ids -> 0
DEFAULT_STAGEMAP = [1] + [0]*9 + [2, 3, 4, 5] + [0]*6 + [6, 7, 8, 9, 10] + \
    [0]*5 + [11, 12, 13, 14, 15] + [0]*25

# GG-Notebook missions/mission_ids.md (partial)
MISSION_NAMES = {
    0: "Garage", 94: "post-cutscene character select",
    95: "Roboy character select", 96: "chapter intro cutscene",
    97: "post-load character select",
}


def bits(words, width=32):
    out = []
    for i, w in enumerate(words):
        for b in range(width):
            if w >> b & 1:
                out.append(i * width + b)
    return out


def rotl8(b, s):
    return ((b << s) | (b >> (8 - s))) & 0xFF


def rotr8(b, s):
    return ((b >> s) | (b << (8 - s))) & 0xFF


def decode_slot(rec):
    """Port of CSaveData::Decode (0x3A920).  rec = 0x3560-byte slot record:
    0x50-byte key + 0x3510 scrambled bytes.  Key: +0x00 8 dword popcounts per
    byte lane, +0x20 32 signed rotate amounts, +0x40 8-byte lane permutation,
    +0x48 8-byte bit permutation.  Returns (sdData bytes, popcount_ok)."""
    key = rec[:0x50]
    expect = struct.unpack_from('<8I', key, 0)
    rot = struct.unpack_from('<32b', key, 0x20)
    lane = key[0x40:0x48]
    bitp = key[0x48:0x50]
    cnt = [0]*8
    out = bytearray(0x3510)
    for i in range(0x3510):
        l = i & 7
        dst = lane[l] + (i >> 3) * 8
        b = rec[0x50 + i]
        cnt[l] += bin(b).count('1')
        r = rot[dst & 0x1F]
        s = abs(r) & 7
        v = rotl8(b, s) if r < 0 else rotr8(b, s)
        o = 0
        for k in range(7, -1, -1):        # bit k <- v bit bitp[k]
            o = (o << 1) | (v >> bitp[k] & 1)
        if dst < 0x3510:
            out[dst] = o
    return bytes(out[:SDSIZE]), list(cnt) == list(expect)


def read_sav(d):
    """JSRFDATA.SAV: 20-byte XCalculateSignature (HMAC-SHA1) over the rest,
    then 'JSRF' + pad to 0x30, 3 x 16-byte slot descriptors
    {used, chapter, playtime seconds, 0}, then 3 slot records of 0x3560."""
    assert len(d) == 0x14 + 0x4C + NSLOTS * SLOTSIZE, hex(len(d))
    sig_ok = hmac.new(bytes(16), d[20:], hashlib.sha1).digest() == d[:20]
    print(f"file: {len(d)} bytes, magic {d[0x14:0x18]!r}, signature "
          f"{'OK (HMAC-SHA1, all-zero XboxSignatureKey as the recomp kernel exports)' if sig_ok else 'does NOT match zero-key HMAC'}")
    slots = []
    for n in range(NSLOTS):
        used, chap, secs, z = struct.unpack_from('<4I', d, 0x30 + 16 * n)
        print(f"  slot {n}: used={used} chapter={chap} playtime={secs//3600}:{secs//60%60:02d}:{secs%60:02d}")
        if used:
            sd, ok = decode_slot(d[0x60 + n * SLOTSIZE: 0x60 + (n + 1) * SLOTSIZE])
            slots.append((f"slot {n}", sd, ok))
    return slots


class SD:
    def __init__(self, b):
        self.b = b
        o = 0
        def take(fmt):
            nonlocal o
            v = struct.unpack_from('<' + fmt, b, o)
            o += struct.calcsize('<' + fmt)
            return v
        (self.chapter, self.mission, self.spawn, self.frames,
         self.chars) = take('5I')
        self.chapter_flags = take('16I')
        self.global_flags = take('16I')
        self.special_flags = take('16I')
        self.spawned = take('8I')
        self.collected = take('8I')
        self.held = take('8I')
        self.unused_stage = take('16I')
        ts = take('2560I')
        self.tags = [[(ts[(s*80+t)*2], ts[(s*80+t)*2+1]) for t in range(80)]
                     for s in range(16)]
        self.vol_music, self.vol_sfx, self.rumble = take('ffI'); take('29I')
        self.garage_music, = take('I')
        self.unused_bits = take('8I')
        self.misc = take('32I')
        self.hiscores = []
        for i in range(16*4*5):
            self.hiscores.append(take('IBBBx'))
        self.clutch_timer, self.unused_timer = take('2I')
        self.sel_tags = take('5I'); self.custom_sel = take('5I')
        self.events = take('16I')
        assert o == SDSIZE, hex(o)


def marks(w):
    return [(w >> (3 * k)) & 7 for k in range(10)]


GANGS = ["GGs/P1", "Golden Rhinos/Zero Beat/P2", "Poison Jam/P3", "Immortals/P4"]


def layer(w):
    """None if unpainted; else (n painted marks, owner string).  SetTagCovered
    (0x3A400) fills all ten 3-bit groups with one gang/player value."""
    m = marks(w)
    painted = [x for x in m if x != 7]
    if not painted:
        return None
    who = sorted(set(painted))
    if len(painted) == 10 and len(who) == 1:
        return (10, "covered:" + (GANGS[who[0]] if who[0] < 4 else str(who[0])))
    return (len(painted), "/".join(f"P{x+1}" for x in who))


def ranges(xs):
    out, i = [], 0
    while i < len(xs):
        j = i
        while j + 1 < len(xs) and xs[j + 1] == xs[j] + 1:
            j += 1
        out.append(f"{xs[i]}" if i == j else f"{xs[i]}-{xs[j]}")
        i = j + 1
    return ",".join(out)


def report(label, sd_bytes, stagemap, raw=False):
    s = SD(sd_bytes)
    idx2stage = {}
    for sid in range(60):
        i = stagemap[sid]
        if i and i not in idx2stage:
            idx2stage[i] = sid
    print(f"\n==== {label} ====")
    ch, mi = s.chapter, s.mission
    tail = mi % 100
    mname = MISSION_NAMES.get(tail, "")
    print(f"return chapter/mission : chapter {ch}, mission {mi} "
          f"(mssn{mi:04d}.bin{'; ' + mname if mname else ''})")
    print(f"spawn position index   : {s.spawn if s.spawn < 0x80000000 else s.spawn - (1<<32)}")
    f = s.frames
    print(f"playtime               : {f} frames = {f//216000}:{f//3600%60:02d}:{f//60%60:02d}")
    uc = bits([s.chars])
    print(f"unlocked characters    : 0x{s.chars:08x} -> " +
          ", ".join(CHARS[i] if i < len(CHARS) else f"#{i}" for i in uc))
    for name, fl in (("chapter", s.chapter_flags), ("global", s.global_flags),
                     ("special", s.special_flags)):
        b = bits(fl)
        print(f"{name:7s} flags ({len(b):3d}) : {b}")
    sp, co, he = bits(s.spawned), bits(s.collected), bits(s.held)
    print(f"souls spawned/collected/held: {len(sp)}/{len(co)}/{len(he)}")
    print(f"  collected ids: {co}")
    if set(co) != set(he):
        print(f"  collected but not held (stolen): {sorted(set(co)-set(he))}")
    print("per-stage tags (stage index from g_stageIdToStageIndex; 'tag#:n' = n G-marks painted by P1;"
          " rival layer listed separately):")
    for si in range(16):
        sid = idx2stage.get(si)
        nm = (f"stg{sid:02d} {STAGE_NAMES.get(sid,'?')}" if sid is not None
              else "(index 0: unmapped stage ids)")
        own, rival, other = [], {}, []
        for t in range(80):
            fr, rv = s.tags[si][t]
            a, b = layer(fr), layer(rv)
            if a:
                (own if a[1] == "P1" else other).append(f"{t}:{a[0]}" + ("" if a[1] == "P1" else f"({a[1]})"))
            if b:
                rival.setdefault(b[1], []).append(t)
        tot = sum(int(x.split(':')[1].split('(')[0]) for x in own)
        print(f"  [{si:2d}] {nm}: {len(own)} tags painted by player ({tot} marks)")
        if own:
            print(f"        player: {' '.join(own)}")
        if other:
            print(f"        front layer, other owner: {' '.join(other)}")
        for k, v in rival.items():
            print(f"        rival layer {k}: tags {ranges(v)} ({len(v)})")
        if raw:
            for t in range(80):
                if s.tags[si][t] != (0xFFFFFFFF, 0xFFFFFFFF):
                    print(f"        raw tag {t}: {s.tags[si][t][0]:08x} {s.tags[si][t][1]:08x}")
    mo = bits(s.misc)
    print(f"misc objectives ({len(mo)}): {mo}  (0-2 sewer switches, 3+ noise tanks per SaveData.hpp)")
    ev = bits(s.events)
    print(f"events seen ({len(ev)}): " + " ".join(f"e{e:03d}" for e in ev if e < 300)
          + ("  | ids >=300 (not event files; written by mission opcode 0xF9, e.g. 308 Combo, 310/311 Rhyth joins): "
             + " ".join(str(e) for e in ev if e >= 300) if any(e >= 300 for e in ev) else ""))
    print(f"settings: music {s.vol_music:.2f} sfx {s.vol_sfx:.2f} rumble {s.rumble}; "
          f"garage music {s.garage_music}; clutch timer {s.clutch_timer}")
    return s


def main():
    a = sys.argv[1:]
    raw = '--raw' in a
    if raw:
        a.remove('--raw')
    stagemap = DEFAULT_STAGEMAP
    if '--stagemap' in a:
        i = a.index('--stagemap')
        stagemap = list(struct.unpack_from('<60I', open(a[i+1], 'rb').read()))
        del a[i:i+2]
    path = a[0]
    d = open(path, 'rb').read()
    print(f"# {path}")
    if os.path.basename(path).upper().endswith('.SAV'):
        for label, sd, ok in read_sav(d):
            print(f"  {label}: lane popcount check {'OK' if ok else 'FAILED'}")
            report(label, sd, stagemap, raw)
    else:
        vt, = struct.unpack_from('<I', d, 0)
        print(f"memory dump of CSaveData, vtable word 0x{vt:08x}; sdData at +4")
        report("m_SaveData (live)", d[4:4 + SDSIZE], stagemap, raw)
        mf = bits(struct.unpack_from('<16I', d, 4 + 2 * SDSIZE))
        print(f"runtime mission flags (CSaveData.m_dwMissionFlags, not saved) ({len(mf)}): {mf}")


if __name__ == '__main__':
    main()
