#!/usr/bin/env python3
"""evdump.py -- dump a JSRF event .dat (MULT, 12 sections) as a readable timeline.

Layout follows KeybadeBlox GG-Notebook events/event_dat.hexpat.
Usage:  /usr/bin/python3 evdump.py 034 [111 ...]      (or a path to a .dat)
        --raw   also print unknown fields of actors
"""
import struct, sys, os, re

EVDIR = os.path.expanduser("~/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/"
                           "Jet Set Radio Future (US)/Media/Event/Event")
PNGDIR = None
HEXPAT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "../kbsurvey/GG-Notebook/events/event_dat.hexpat")
SECT = ["sceneIntl", "sceneJP", "talkEvent", "models", "heads", "u6", "u7", "u8",
        "shadow?", "textures", "effects", "sounds"]
CHARID = ["Beat","Corn","Gum","Combo","Rhyth","Soda","Yoyo","NT3000","Garam","Boogie","Cube",
          "LoveShockers","PoisonJam","NoiseTank","Immortals","DoomRiders","Rapid99","Pots","Gouji",
          "Roboy","Clutch","Jazz","AKUMU","ZeroBeat"]
TALKCHAR = ["Beat","Gum","Corn","Combo","Boogie","Garam","Rhyth","Cube","Yoyo","Soda","Clutch","Jazz",
            "PoisonJam","Rapid99","Immortals","NoiseTank","DoomRiders","LoveShockers","Pots",
            "DJProfessorK","Keikan","Heli","Tank","PatrolCar","Hayashi","KeisatsuRobo1",
            "MassProducedNoiseTank","RhinoFighter","Harrier","Pyro","ClawGuy","KeisatsuRobo2",
            "HeavyArmorTrainGun","TowerFinalBoss","ZeroBeat","Roboy","ElectricBarricade","PoliceVan",
            "SearchLight","NT3000","Gouji","AKUMU","GoldenRhinoCommCenter","TapeRecorder"]
DISPLAY = ["Transmission","Bubble","Centre","Top","BottomLeft","BottomCentre"]
# ModelResource.location: 1 = event .dat section 4, 2 = player/character file (sel = CharId),
# >=3 = a game-side enemy/NPC model slot (not in the event file).  Mapping observed over all 300 events:
LOC = {0: "light?", 1: "dat", 2: "player", 3: "enemy:hayashi", 4: "enemy:police", 5: "enemy:arm",
       6: "enemy:fire", 9: "enemy:killer_gun", 10: "enemy:ntank_low", 11: "enemy:ntank_low_fly",
       12: "npc:people", 13: "enemy:tank", 14: "enemy:probot", 15: "enemy:traingun", 16: "enemy:tower",
       17: "enemy:police_tank", 18: "enemy:probot_break", 19: "enemy:traingun_grf_break",
       20: "enemy:akumu_d2", 21: "enemy:traingun_grf", 22: "enemy:killer_hand", 23: "enemy:patcar",
       24: "enemy:akumu"}



def load_enum(name):
    try:
        src = open(HEXPAT).read()
    except OSError:
        return {}
    m = re.search(r"enum %s\s*:\s*u32\s*\{(.*?)\};" % name, src, re.S)
    out = {}
    if m:
        for nm, val in re.findall(r"(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)", m.group(1)):
            out[int(val, 0)] = nm
    return out

ADXN = load_enum("ADXId")
SEN = load_enum("SEFile")


class R:
    def __init__(s, d, o, end):
        s.d, s.o, s.end = d, o, end
    def u32(s):
        v = struct.unpack_from("<I", s.d, s.o)[0]; s.o += 4
        if s.o > s.end: raise ValueError("overrun @%x" % s.o)
        return v
    def i32(s):
        v = struct.unpack_from("<i", s.d, s.o)[0]; s.o += 4; return v
    def f32(s):
        v = struct.unpack_from("<f", s.d, s.o)[0]; s.o += 4; return v
    def words(s, n):
        v = list(struct.unpack_from("<%dI" % n, s.d, s.o)); s.o += 4 * n
        if s.o > s.end: raise ValueError("overrun @%x" % s.o)
        return v
    def cstr(s):
        e = s.d.index(b"\0", s.o); v = s.d[s.o:e].decode("latin1"); s.o = e + 1; return v
    def align4(s):
        s.o += (-s.o) % 4
    def lenarr(s, fn):
        n = s.u32()
        if n > 100000: raise ValueError("absurd count %d @%x" % (n, s.o - 4))
        return [fn() for _ in range(n)]


def f2(w):  # interpret a u32 as float for display
    return struct.unpack("<f", struct.pack("<I", w))[0]


def parse_actor(r):
    a = {"type": r.u32(), "model": r.u32(), "defAnim": r.u32(), "u1": r.u32(), "u2": r.u32()}
    a["paths"] = r.lenarr(lambda: dict(zip(("path", "start", "end"), r.words(3))))
    a["unk3"] = r.lenarr(lambda: r.words(3))
    def anim():
        w = r.words(7); sp = r.f32()
        return {"anim": w[0], "start": w[1], "end": w[2], "u2": w[3], "off1": w[4], "off2": w[5],
                "u3": w[6], "speed": sp}
    a["anims"] = r.lenarr(anim)
    a["unk5"] = r.lenarr(lambda: r.words(13))
    def follow():
        w = r.words(3); v = (r.f32(), r.f32(), r.f32())
        return {"start": w[0], "end": w[1], "actor": w[2], "off": v}
    a["follows"] = r.lenarr(follow)
    a["yaws"] = r.lenarr(lambda: dict(zip(("deg", "start", "u"), struct.unpack("<iII", struct.pack("<3I", *r.words(3))))))
    a["rolls"] = r.lenarr(lambda: dict(zip(("deg", "start", "end"), struct.unpack("<iII", struct.pack("<3I", *r.words(3))))))
    a["zooms"] = r.lenarr(lambda: dict(zip(("zoom", "frame", "smooth"), r.words(3))))
    def snd():
        w = r.words(10)
        return {"start": w[0], "end": w[1], "file": w[2], "idx": w[3], "rest": w[4:]}
    a["sounds"] = r.lenarr(snd)
    a["unk11"] = r.lenarr(lambda: r.words(3))
    a["unk12"] = r.lenarr(lambda: r.words(11))
    a["unk13"] = r.lenarr(lambda: r.words(5))
    return a


def parse_scene(d, o, end):
    r = R(d, o, end)
    sc = {"duration": r.u32(), "render": r.u32(), "u1": r.u32(), "u2": r.u32()}
    n = r.u32(); sc["actorCnt"] = n
    sc["actors"] = [parse_actor(r) for _ in range(n)]
    sc["fades"] = r.lenarr(lambda: dict(zip(("start", "dur", "type", "colour"), r.words(4))))
    sc["adx"] = r.lenarr(lambda: dict(zip(("start", "id"), r.words(2))))
    def path():
        u1, u2, k = r.words(3)
        pos = [(r.f32(), r.f32(), r.f32()) for _ in range(k)]
        u4 = r.words(k)
        return {"u1": u1, "u2": u2, "keys": k, "pos": pos, "u4": u4}
    sc["paths"] = r.lenarr(path)
    nu13 = sum(1 for p in sc["paths"] if p["u1"] != 0)
    sc["unk13"] = [[(r.f32(), r.f32(), r.f32()) for _ in range(100)] for _ in range(nu13)]
    sc["unk7"] = r.lenarr(r.u32)
    na = r.u32(); sc["u9"] = r.u32()
    def animres():
        w = r.words(9); p = r.cstr(); r.align4(); return {"w": w, "path": p}
    sc["animres"] = [animres() for _ in range(na)]
    def modres():
        w = r.words(4); u2 = r.i32(); u3 = r.u32(); u4 = r.f32(); loc = r.u32(); ch = r.u32()
        u5 = r.u32(); p = r.cstr(); r.align4()
        return {"w": w, "u2": u2, "u3": u3, "u4": u4, "loc": loc, "sel": ch, "u5": u5, "path": p}
    sc["models"] = r.lenarr(modres)
    def u19():
        s = r.cstr(); r.align4(); return s
    try:
        sc["unk12"] = r.lenarr(u19)
    except Exception as e:
        sc["unk12"] = ["<parse fail %s>" % e]
    sc["_consumed"] = r.o - o
    sc["_size"] = end - o
    return sc


def norm_items(d, off):
    if d[off:off + 4] != b"NORM":
        return []
    c = struct.unpack_from("<I", d, off + 4)[0]; p = off + 16; out = []
    for _ in range(c):
        s, nx, sz, _pd = struct.unpack_from("<IIII", d, p)
        out.append((off + s, sz)); p = off + nx
    return out


def mult_sections(d):
    assert d[:4] == b"MULT", "not a MULT file"
    n = struct.unpack_from("<I", d, 4)[0]; o = 16; out = []
    for _ in range(n):
        off, nxt, _sz, _u = struct.unpack_from("<IIII", d, o)
        out.append(norm_items(d, off)); o = nxt
    return out


def parse_talk(d, a, sz):
    if sz == 0:
        return None
    r = R(d, a, a + sz)
    r.u32(); r.u32(); r.u32(); flags = r.u32()
    pal = (flags >> 2) & 1
    langs = ["ja", "en", "fr", "de", "es"] + (["it"] if pal else [])
    sc = {l: r.u32() for l in langs}
    lc = {l: r.words(sc[l]) for l in langs}
    ch = {l: r.words(sc[l]) for l in langs}
    dt = {l: r.words(sc[l]) for l in langs}
    u3 = {l: r.words(sc[l]) for l in langs}
    st = {l: r.words(sc[l]) for l in langs}
    du = {l: r.words(sum(lc[l])) for l in langs}
    lines = {}
    for l in langs:
        ls = []
        for _ in range(sum(lc[l])):
            e = d.index(b"$r", r.o, a + sz); raw = d[r.o:e]; r.o = e + 2
            try:
                ls.append(raw.decode("shift_jis" if l == "ja" else "latin1"))
            except UnicodeDecodeError:
                ls.append(raw.decode("latin1"))
        lines[l] = ls
    out = []
    li = 0; l = "en"
    for i in range(sc[l]):
        seg = []
        for k in range(lc[l][i]):
            seg.append((du[l][li], lines[l][li])); li += 1
        out.append({"char": ch["ja"][i] if i < len(ch["ja"]) else None, "disp": dt[l][i],
                    "start": st[l][i], "lines": seg})
    return {"flags": flags, "sections": out}


# Texture header (32 bytes): u32 id; u32 pad[4]; u16 width @0x14; u16 height @0x16 (0=square);
# u32 flags @0x18; u32 mipLevels @0x1c.  CONFIRMED from the XBE: parseEventDatTextures (0x2a940)
# -> UnknownStatic25 0x6e1c0 (bit 0x400 = mipmapped) -> CMGameGL vtbl+0x38 (0x14f720)
# -> format resolver 0x14e730 -> D3DDevice_CreateTexture(0x18cbf0).
BASEFMT = {1: ("X1R5G5B5", 0x03), 2: ("R5G6B5", 0x05), 3: ("A1R5G5B5", 0x02), 4: ("A4R4G4B4", 0x04),
           5: ("X8R8G8B8", 0x07), 6: ("A8R8G8B8", 0x06), 15: ("L8", 0x00)}
LINEAR = {0x03: 0x1C, 0x05: 0x11, 0x02: 0x10, 0x04: 0x1D, 0x07: 0x1E, 0x06: 0x12, 0x00: 0x13}
SUPPORTED = {0x0C, 0x0E, 0x06, 0x07, 0x11, 0x03, 0x04}   # per the caller's renderer list
NV = {0x0C: "DXT1", 0x0E: "DXT3", 0x0F: "DXT5", 0x03: "X1R5G5B5", 0x05: "R5G6B5(swz)", 0x02: "A1R5G5B5",
      0x04: "A4R4G4B4", 0x07: "X8R8G8B8", 0x06: "A8R8G8B8", 0x00: "L8", 0x1C: "LIN_X1R5G5B5",
      0x11: "LIN_R5G6B5", 0x10: "LIN_A1R5G5B5", 0x1D: "LIN_A4R4G4B4", 0x1E: "LIN_X8R8G8B8",
      0x12: "LIN_A8R8G8B8", 0x13: "LIN_L8", 0x27: "L6V5U5?", 0x28: "V8U8?"}


def resolve_fmt(flags):
    base = flags & 0xF
    if base not in BASEFMT:
        return None, "invalid base %d (CreateTexture skipped)" % base
    f = BASEFMT[base][1]; notes = []
    alpha = base in (4, 6)
    if flags & 0x100:
        f = 0x0E if alpha else 0x0C
    elif alpha and flags & 0x200:
        f = 0x0E
    elif alpha and flags & 0x40000:
        f = 0x0E
    elif alpha and flags & (0x80000 | 0x100000):
        f = 0x0F
    if flags & 0x4000: notes.append("bit0x4000 -> 0x27/0x28 bump fmt")
    if flags & 0x20000: f = LINEAR.get(f, f); notes.append("linear")
    if flags & 0x9800: notes.append("cube/volume/special path bits 0x%x" % (flags & 0x9800))
    return f, ",".join(notes)


def dxt_decode(d, a, w, h, fmt):
    """Decode top mip of DXT1/DXT3 into RGBA bytes (for eyeballing)."""
    out = bytearray(w * h * 4); bs = 8 if fmt == 0x0C else 16; p = a
    def c565(c):
        return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)
    for by in range(h // 4):
        for bx in range(w // 4):
            if fmt == 0x0E:
                al = d[p:p + 8]; cb = p + 8
            else:
                al = None; cb = p
            c0, c1, idx = struct.unpack_from("<HHI", d, cb)
            a0, a1 = c565(c0), c565(c1)
            if c0 > c1 or fmt == 0x0E:
                pal = [a0 + (255,), a1 + (255,), tuple((2 * x + y) // 3 for x, y in zip(a0, a1)) + (255,),
                       tuple((x + 2 * y) // 3 for x, y in zip(a0, a1)) + (255,)]
            else:
                pal = [a0 + (255,), a1 + (255,), tuple((x + y) // 2 for x, y in zip(a0, a1)) + (255,), (0, 0, 0, 0)]
            for i in range(16):
                px = pal[(idx >> (2 * i)) & 3]
                if al is not None:
                    nib = (al[i // 2] >> (4 * (i & 1))) & 15
                    px = px[:3] + (nib * 17,)
                o = ((by * 4 + i // 4) * w + bx * 4 + i % 4) * 4
                out[o:o + 4] = bytes(px)
            p += bs
    return bytes(out)


def tex_info(d, a, sz):
    h = d[a:a + 32]
    tid = struct.unpack_from("<I", h, 0)[0]
    w, hh = struct.unpack_from("<HH", h, 0x14)
    flags, mips = struct.unpack_from("<II", h, 0x18)
    f, notes = resolve_fmt(flags)
    levels = mips if flags & 0x400 else 1
    payload = sz - 32
    hh2 = hh or w
    exp = 0; ww, hv = w, hh2
    for _ in range(max(levels, 1)):
        if f in (0x0C, 0x0E, 0x0F):
            exp += max(ww // 4, 1) * max(hv // 4, 1) * (8 if f == 0x0C else 16)
        elif f is not None:
            exp += ww * hv * (4 if f in (0x06, 0x07, 0x12, 0x1E) else 1 if f in (0x00, 0x13) else 2)
        ww //= 2; hv //= 2
    alpha = None
    if f == 0x0E:
        blk = d[a + 32:a + 32 + max(w // 4, 1) * max(hh2 // 4, 1) * 16]
        s = set()
        for i in range(0, len(blk), 16):
            for x in blk[i:i + 8]:
                s.add(x >> 4); s.add(x & 15)
        alpha = sorted(s)
    return {"id": tid, "w": w, "h": hh2, "flags": flags, "levels": levels, "payload": payload,
            "nv": f, "fmt": ("%s (NV2A 0x%02X)" % (NV.get(f, "?"), f)) if f is not None else notes,
            "notes": notes, "sizeok": exp == payload, "supported": f in SUPPORTED, "alpha_nibbles": alpha}


def model_info(d, a, sz):
    magic = d[a:a + 4]
    return "%s size=0x%x" % (magic.decode("latin1", "replace"), sz)


def fmt_adx(i):
    return ADXN.get(i, "0x%x" % i)


def dump_scene(sc, raw=False, label="intl"):
    P = print
    P("  scene[%s] duration=%d frames (%.2f s @60)  render=%d u1=%d u2=%d actors=%d   parsed %d/%d bytes%s" % (
        label, sc["duration"], sc["duration"] / 60.0, sc["render"], sc["u1"], sc["u2"], sc["actorCnt"],
        sc["_consumed"], sc["_size"], "" if sc["_size"] - sc["_consumed"] < 16 else "  <-- UNPARSED TAIL"))
    models = sc["models"]
    P("  model resources (%d):" % len(models))
    for i, m in enumerate(models):
        src = LOC.get(m["loc"], "loc%d" % m["loc"])
        extra = (" char=%s" % (CHARID[m["sel"]] if m["sel"] < len(CHARID) else m["sel"])) if m["loc"] == 2 else \
                (" sel=%d" % m["sel"])
        P("    M%-2d %-8s%s  %s   w=%s u2=%d u3=%d u4=%.3f u5=%d" % (
            i, src, extra, m["path"], m["w"], m["u2"], m["u3"], m["u4"], m["u5"]))
    P("  animation resources (%d) u9=%d:" % (len(sc["animres"]), sc["u9"]))
    for i, a in enumerate(sc["animres"]):
        P("    A%-2d %s   w=%s" % (i, a["path"], a["w"]))
    P("  paths (%d):" % len(sc["paths"]))
    for i, p in enumerate(sc["paths"]):
        p0 = p["pos"][0] if p["pos"] else None; p1 = p["pos"][-1] if p["pos"] else None
        P("    P%-2d keys=%d u1=%d u2=%d  first=%s last=%s" % (
            i, p["keys"], p["u1"], p["u2"],
            "(%.1f,%.1f,%.1f)" % p0 if p0 else "-", "(%.1f,%.1f,%.1f)" % p1 if p1 else "-"))
    if sc["unk7"]: P("  unk7:", sc["unk7"])
    if sc["unk12"]: P("  unk12 strings:", sc["unk12"])
    P("  fades:")
    for f in sc["fades"]:
        P("    frames %4d-%4d (start %d, %d frames)  %s %s" % (f["start"], f["start"] + f["dur"], f["start"], f["dur"], ["FADE-IN", "FADE-OUT"][f["type"]] if f["type"] < 2 else f["type"],
                                         ["black", "white"][f["colour"]] if f["colour"] < 2 else "colour%d" % f["colour"]))
    if not sc["fades"]: P("    (none)")
    P("  ADX cues:")
    for x in sc["adx"]:
        P("    frame %4d  %s" % (x["start"], fmt_adx(x["id"])))
    if not sc["adx"]: P("    (none)")
    P("  actors:")
    for i, a in enumerate(sc["actors"]):
        kind = ["Camera", "CameraTarget", "Model"][a["type"]] if a["type"] < 3 else "type%d" % a["type"]
        if i == 0: kind = "Camera"
        elif i == 1: kind = "CameraTarget"
        mdl = ""
        if i >= 2:
            m = a["model"]
            if m < len(models):
                mm = models[m]
                mdl = " model=M%d %s [%s%s]" % (m, os.path.basename(mm["path"].replace("\\", "/")),
                                                LOC.get(mm["loc"], "loc%d" % mm["loc"]),
                                                ("/" + CHARID[mm["sel"]]) if mm["loc"] == 2 and mm["sel"] < len(CHARID) else "")
            else:
                mdl = " model=%d(out of range)" % m
        P("   #%d %s%s  defAnim=%d u1=%d u2=%d" % (i, kind, mdl, a["defAnim"], a["u1"], a["u2"]))
        for p in a["paths"]:
            P("       path P%d  frames %d-%d" % (p["path"], p["start"], p["end"]))
        for an in a["anims"]:
            ar = sc["animres"][an["anim"]]["path"] if an["anim"] < len(sc["animres"]) else "?"
            P("       anim A%d %s  frames %d-%d speed=%.3f  (u2=%d off=%d/%d u3=%d)" % (
                an["anim"], os.path.basename(ar.replace("\\", "/")), an["start"], an["end"], an["speed"],
                an["u2"], an["off1"], an["off2"], an["u3"]))
        for f in a["follows"]:
            P("       follow actor#%d frames %d-%d offset=(%.1f,%.1f,%.1f)" % (f["actor"], f["start"], f["end"], *f["off"]))
        for y in a["yaws"]:
            P("       yaw %d deg @%d (u=%d)" % (y["deg"], y["start"], y["u"]))
        for y in a["rolls"]:
            P("       roll %d deg @frame %d (3rd field=%d; hexpat calls it endFrame, looks like a flag)" % (y["deg"], y["start"], y["end"]))
        for z in a["zooms"]:
            P("       zoom %d @frame %d smooth=%d" % (z["zoom"], z["frame"], z["smooth"]))
        for s in a["sounds"]:
            P("       sound %s #%d frames %d-%d rest=%s" % (SEN.get(s["file"], "file%d" % s["file"]), s["idx"],
                                                         s["start"], s["end"], s["rest"]))
        for nm in ("unk3", "unk5", "unk11", "unk12", "unk13"):
            if a[nm]:
                P("       %s x%d: %s" % (nm, len(a[nm]), a[nm] if raw else a[nm][:3]))


def camera_cuts(sc):
    """Camera 'cuts' = boundaries of camera/target path segments."""
    cam = sc["actors"][0]; tgt = sc["actors"][1]
    b = sorted(set([p["start"] for p in cam["paths"]] + [p["start"] for p in tgt["paths"]]))
    return b


def timeline(sc):
    P = print
    P("  TIMELINE (frame ranges on screen, from path/anim assignments):")
    ev = []
    for i, a in enumerate(sc["actors"][2:], 2):
        m = sc["models"][a["model"]] if a["model"] < len(sc["models"]) else None
        name = os.path.basename(m["path"].replace("\\", "/")) if m else "?"
        src = LOC.get(m["loc"], "?") if m else "?"
        rng = [(p["start"], p["end"]) for p in a["paths"]] + [(x["start"], x["end"]) for x in a["anims"]] + \
              [(x["start"], x["end"]) for x in a["follows"]]
        if rng:
            lo = min(r[0] for r in rng); hi = max(r[1] for r in rng)
        else:
            lo, hi = 0, sc["duration"]
        ev.append((lo, "  %4d-%4d  #%d %s (%s)" % (lo, hi, i, name, src)))
    for f in sc["fades"]:
        ev.append((f["start"], "  %4d-%4d  %s %s" % (f["start"], f["start"] + f["dur"], ["fade-in", "fade-out"][f["type"] & 1],
                                                    ["black", "white"][f["colour"] & 1])))
    for x in sc["adx"]:
        ev.append((x["start"], "  %4d       ADX %s" % (x["start"], fmt_adx(x["id"]))))
    for c in camera_cuts(sc):
        ev.append((c, "  %4d       camera segment start" % c))
    for a in sc["actors"]:
        for s in a["sounds"]:
            ev.append((s["start"], "  %4d-%4d  SE %s #%d" % (s["start"], s["end"], SEN.get(s["file"], s["file"]), s["idx"])))
    for _, s in sorted(ev, key=lambda t: t[0]):
        P("   " + s)


def dump(path, raw=False, jp=False):
    d = open(path, "rb").read()
    secs = mult_sections(d)
    print("=" * 100)
    print("%s  (%d bytes)" % (path, len(d)))
    print("  sections: " + "  ".join("%s=%d" % (SECT[i] if i < len(SECT) else i, len(s)) for i, s in enumerate(secs)))
    for idx, label in ((0, "intl"),) + (((1, "JP"),) if jp else ()):
        for a, sz in secs[idx]:
            try:
                sc = parse_scene(d, a, a + sz)
            except Exception as e:
                print("  scene[%s] PARSE ERROR: %s" % (label, e)); continue
            dump_scene(sc, raw, label)
            timeline(sc)
    # compare intl vs jp
    if secs[0] and secs[1]:
        a0, s0 = secs[0][0]; a1, s1 = secs[1][0]
        print("  sceneJP identical to intl: %s" % (d[a0:a0 + s0] == d[a1:a1 + s1]))
    for a, sz in secs[2]:
        try:
            t = parse_talk(d, a, sz)
        except Exception as e:
            print("  talk PARSE ERROR", e); continue
        if not t:
            print("  talk: (none)"); continue
        print("  talk (EN), flags=0x%x:" % t["flags"])
        for s in t["sections"]:
            c = s["char"]
            print("    @%4d %-14s %-12s" % (s["start"], TALKCHAR[c] if c is not None and c < len(TALKCHAR) else c,
                                          DISPLAY[s["disp"]] if s["disp"] < len(DISPLAY) else s["disp"]))
            for du, ln in s["lines"]:
                print("         [%4d] %s" % (du, ln))
    for si, label in ((3, "models"), (4, "heads"), (8, "shadow?"), (10, "effects")):
        for j, (a, sz) in enumerate(secs[si]):
            print("  %s[%d]: %s" % (label, j, model_info(d, a, sz)))
    for j, (a, sz) in enumerate(secs[9]):
        t = tex_info(d, a, sz)
        print("  texture[%d]: id=%08x %dx%d flags=0x%05x levels=%d payload=0x%x -> %s%s%s%s%s" % (
            j, t["id"], t["w"], t["h"], t["flags"], t["levels"], t["payload"], t["fmt"],
            "" if t["sizeok"] else "  SIZE-MISMATCH", "" if t["supported"] else "  ** NOT IN RENDERER LIST **",
            (" [%s]" % t["notes"]) if t["notes"] else "",
            ("  alpha nibbles used=%s" % t["alpha_nibbles"]) if t["alpha_nibbles"] is not None else ""))
        if PNGDIR and t["nv"] in (0x0C, 0x0E):
            try:
                from PIL import Image
                rgba = dxt_decode(d, a + 32, t["w"], t["h"], t["nv"])
                fn = os.path.join(PNGDIR, "%s_tex%d_%08x.png" % (os.path.basename(path)[:-4], j, t["id"]))
                Image.frombytes("RGBA", (t["w"], t["h"]), rgba).save(fn)
                print("      -> %s" % fn)
            except Exception as e:
                print("      (png failed: %s)" % e)
    for j, (a, sz) in enumerate(secs[11]):
        print("  sound[%d]: %s size=0x%x" % (j, d[a:a + 4], sz))
    for k in (5, 6, 7):
        if secs[k]: print("  section %d has %d items (normally unused)" % (k + 1, len(secs[k])))


if __name__ == "__main__":
    raw = "--raw" in sys.argv; jp = "--jp" in sys.argv
    for x in sys.argv:
        if x.startswith("--png="):
            PNGDIR = x[6:]; os.makedirs(PNGDIR, exist_ok=True)
    for arg in [x for x in sys.argv[1:] if not x.startswith("--")]:
        p = arg if os.path.exists(arg) else os.path.join(EVDIR, "e%03d.dat" % int(arg))
        dump(p, raw, jp)
