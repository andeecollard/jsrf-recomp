#!/usr/bin/env python3
"""Scan D:\\Media\\Mission\\mssn*.bin: stage, events loaded/played (with state
flag conditions/writes), talk events, exits.  Layout from GG-Notebook
missions/mission_bin.hexpat (BlockTable of 45 {offset,count} at 0x34; Command
= 84 bytes; MissionExit = 68 bytes; Resource = 32 bytes)."""
import os, struct, sys, glob
MD = os.path.expanduser("~/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/Jet Set Radio Future (US)/Media/Mission")
LISTS = "MCGS"

def flag(v):
    return f"{LISTS[v&7] if (v&7)<4 else '?'}{(v>>3)&0xffff}={(v>>19)&1}"

def parse(path):
    d = open(path, 'rb').read()
    minor, major = struct.unpack_from('<HH', d, 0)
    bt = [struct.unpack_from('<II', d, 0x34 + 8*i) for i in range(45)]
    def flags(off, cnt):
        return [flag(struct.unpack_from('<I', d, off + 4*i)[0]) for i in range(cnt)]
    res = []
    o, n = bt[0]
    for i in range(n):
        t, rid, fro, frc, fwo, fwc = struct.unpack_from('<6I', d, o + 32*i)
        res.append((t, rid, flags(fro, frc), flags(fwo, fwc)))
    cmds = []
    for blk, name in zip(range(40, 44), ("imm", "lis", "nbl", "blk")):
        o, n = bt[blk]
        for i in range(n):
            base = o + 84*i
            co, cc, wo, wc, op = struct.unpack_from('<5I', d, base)
            args = struct.unpack_from('<16I', d, base + 20)
            cmds.append((name, i, op, args, flags(co, cc), flags(wo, wc)))
    o, n = bt[44]
    exits = [struct.unpack_from('<IIi', d, o + 68*i) for i in range(n)]
    return (major, minor), res, cmds, exits

if __name__ == '__main__':
    for p in sorted(glob.glob(os.path.join(MD, 'mssn*.bin'))):
        mid = os.path.basename(p)[4:8]
        if len(sys.argv) > 1 and mid not in sys.argv[1:]:
            continue
        st, res, cmds, exits = parse(p)
        ev_res = [r[1] for r in res if r[0] == 9]
        ev_load = [c[3][0] for c in cmds if c[2] == 0x32]
        plays = [c for c in cmds if c[2] in (0xE3, 0xE7)]
        print(f"mssn{mid} stg{st[0]}{st[1]} events-res={ev_res} events-load={ev_load} "
              f"exits={[(hex(t), m, s) for t, m, s in exits]}")
        for c in plays:
            kind = "EVENT e%03d" % c[3][0] if c[2] == 0xE3 else "talk TE%03d" % c[3][0]
            print(f"    {c[0]}[{c[1]}] {kind} if {c[4]} -> {c[5]}")
