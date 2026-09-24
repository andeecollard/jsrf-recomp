import sys, os, glob, json
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../events'))
import evdump as E
out = {}
for p in sorted(glob.glob(os.path.join(E.EVDIR, 'e*.dat'))):
    eid = int(os.path.basename(p)[1:4])
    d = open(p, 'rb').read()
    info = {"file_bytes": len(d)}
    try:
        secs = E.mult_sections(d)
        dur = None; chars = []; models = 0; adx = []
        for a, sz in secs[0]:
            sc = E.parse_scene(d, a, a + sz)
            dur = sc["duration"]; models = len(sc["models"])
            for m in sc["models"]:
                if m["loc"] == 2 and m["sel"] < len(E.CHARID): chars.append(E.CHARID[m["sel"]])
                elif m["loc"] >= 3: chars.append(E.LOC.get(m["loc"], "loc%d" % m["loc"]))
            adx = [x["id"] for x in sc["adx"]]
        info.update(duration_frames=dur, seconds=round(dur / 60, 2) if dur is not None else None,
                    model_resources=models, cast=sorted(set(chars)), adx_cues=adx,
                    textures=len(secs[9]), sounds=len(secs[11]))
        talk = None
        for a, sz in secs[2]:
            talk = E.parse_talk(d, a, sz)
        if talk:
            lines = [ln for s in talk["sections"] for _, ln in s["lines"]]
            spk = [E.TALKCHAR[s["char"]] if s["char"] is not None and s["char"] < len(E.TALKCHAR) else s["char"] for s in talk["sections"]]
            info.update(dialogue_lines=len(lines), speakers=sorted(set(map(str, spk))),
                        first_line=(lines[0][:90] if lines else None),
                        display=sorted(set(E.DISPLAY[s["disp"]] if s["disp"] < len(E.DISPLAY) else str(s["disp"]) for s in talk["sections"])))
        else:
            info.update(dialogue_lines=0)
    except Exception as e:
        info["parse_error"] = str(e)
    out["e%03d" % eid] = info
json.dump(out, open('evinfo.json', 'w'), indent=1)
print(len(out), sum(1 for v in out.values() if 'parse_error' in v))
