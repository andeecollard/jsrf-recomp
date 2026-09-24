"""Build catalogue.json + catalogue.md: every e*.dat event, where it plays, and how to reach it unattended.
Run: /usr/bin/python3 evinfo.py && /usr/bin/python3 build.py >/dev/null && /usr/bin/python3 catalogue.py"""
import json, os
from sim import MISSIONS, run_chain, ldesc
from req import need, fl
from build import PLAYER_G, PLAYER_S, wsf
HERE = os.path.dirname(os.path.abspath(__file__))
EV = json.load(open(os.path.join(HERE, 'evinfo.json')))
AN = json.load(open(os.path.join(HERE, 'analysis.json')))
REPORTED = {"e200": "DJ K (player report)", "e210": "DJ K (player report)", "e214": "DJ K (player report)",
            "e201": "police (player report)", "e213": "police (player report)",
            "e034": "Poison Jam (player report)", "e111": "Poison Jam (player report)", "e211": "Poison Jam (player report)"}
RANK = {'A': 0, 'B': 1, 'C': 2, 'C?': 3, 'D': 4}
# event resource loads: resources type 9 and nbl 0x32
loads = {}
for mid, m in MISSIONS.items():
    for c in m['cmds']:
        if (c['blk'] == 'res' and c['a'][0] == 9) or c['op'] == 0x32:
            eid = "e%03d" % (c['a'][1] if c['blk'] == 'res' else c['a'][0])
            loads.setdefault(eid, []).append(dict(mission="mssn%04d" % mid, how="resource" if c['blk'] == 'res' else "%s[%d] 0x32" % (c['blk'], c['i']),
                                                  cond=[fl(*x) for x in c['cond']]))
def etype(v):
    if v['file_bytes'] <= 1100 and not v['dialogue_lines']: return "stub (empty placeholder file)"
    if v['model_resources'] == 0: return "fly-over/caption (camera path + text/voice, no models)"
    return "full cutscene (%d models)" % v['model_resources']
events = {}
for eid, v in sorted(EV.items()):
    plays = [p for p in AN['plays'] if p['event'] == eid]
    for p in plays:
        p['talk_triggered'] = any('talks to talk-character' in a for a in p['player_actions'])
    best = min(plays, key=lambda p: RANK[p['cls']]) if plays else None
    cls = best['cls'] if best else 'D'
    rec = dict(event=eid, duration_frames=v['duration_frames'], seconds=v['seconds'], type=etype(v),
               cast=v['cast'], speakers=v.get('speakers', []), dialogue_lines=v['dialogue_lines'],
               first_line=v.get('first_line'), file_bytes=v['file_bytes'],
               cls=cls, recipe=best['recipe'] if best else ("never played by any mission (no 0xE3 command); " +
                   ("loaded only: %s" % loads[eid] if eid in loads else "not even loaded")),
               talk_triggered=bool(best and best['talk_triggered']),
               plays=plays, loaded_by=loads.get(eid, []), reported=REPORTED.get(eid))
    events[eid] = rec
# TalkEvents (0xE7) for completeness
tes = {}
for mid, m in sorted(MISSIONS.items()):
    for c in m['cmds']:
        if c['op'] != 0xE7: continue
        te = "TE%03d" % c['a'][0]
        a_hit = [s['jump'] for s in AN['sweeps'].values() for x in s['events'] if x['e'] == te and x['mission'] == "mssn%04d" % mid]
        rq = need(mid, c)
        tes.setdefault(te, []).append(dict(mission="mssn%04d" % mid, block="blk[%d]" % c['i'], cond=[fl(*x) for x in c['cond']],
            cls='A' if a_hit else ('C' if rq['actions'] else 'B/C?'), a_jumps=sorted(set(a_hit)),
            player_actions=list(dict.fromkeys(rq['actions'])), chapter_flags_needed=sorted(f for f in rq['flags'] if f[0] == 'C')))
sweeps = [dict(v, events=v['events']) for k, v in sorted(AN['sweeps'].items(), key=lambda x: int(x[0])) if v['events']]
stg20 = sorted("%d:%d" % (m // 100, m % 100) for m, x in MISSIONS.items() if x['stage'] == 'stg20' and 100 <= m < 1000)
out = dict(
    generated="2026-09-24", source="D:\\Media\\Mission\\mssn*.bin + D:\\Media\\Event\\Event\\e*.dat (NA dump in iCloud)",
    model_notes=[
        "Everything under 'plays' is CONFIRMED from mission bytecode: mission, block index, flag conditions, flag writes (GG-Notebook layout).",
        "Class A/B are the output of a forward simulation of the command blocks (sim.py); its execution model is INFERRED (GG-Notebook prose + CMission::runListenerCmds 0x56990 jump table), not traced in the engine: res flag writes -> imm in order -> nbl/auto-listeners as flags allow -> one blk at a time lowest index first -> E6 exits followed (C flags kept, M cleared, 0xFF next chapter clears C).",
        "Automatic listeners: only 0x20 (unconditional) and 0x00 (offscreen timer). Everything else (plane pass, talk, challenge region 0x18, fights 0x0a/0x0c/0x0d/0x0f, race 0x29, rival 0x1b, yes/no 0x16) is treated as player action.",
        "Simulated with the player's slot-0 G/S flags (G1,G10 + 20 S flags); re-running with G/S all clear changed no class-A result.",
        "Timings t_frame are from event durations (evdump) + Wait (0x57) args; talk events counted as 300 f, transmissions 240 f (guesses). Load times are not included.",
        "RECOMP_CHAPTER_JUMP clears ALL chapter (C) flags and sets only C2=1 for 1:0; a savetool edit of C flags is wiped by the jump. Class B therefore needs either a jump extension that writes the listed WriteStateFlag dwords after ClearStateFlags (same call the jump already makes for 0x00080011), or a save whose return mission + C flags are set with savetool and loaded with Continue (no jump)."],
    stage20_rokkaku_missions=stg20,
    class_A_sweeps=sweeps, events=events, talk_events=tes)
json.dump(out, open(os.path.join(HERE, 'catalogue.json'), 'w'), indent=1)
# ---------------- markdown
from collections import Counter
L = []
P = L.append
cnt = Counter(e['cls'] for e in events.values())
P("# JSRF cutscene catalogue (all %d e*.dat events)\n" % len(events))
P("Generated 2026-09-24 from mission bytecode (mssn*.bin, all %d missions) and event files. Scripts: evinfo.py, sim.py, req.py, build.py, catalogue.py in this folder.\n" % len(MISSIONS))
P("Classes (best route per event): " + ", ".join("%s=%d" % (k, cnt[k]) for k in ('A', 'B', 'C', 'C?', 'D')) + "\n")
P("- **A** plays automatically after `RECOMP_CHAPTER_JUMP=C:M` (chapter flags cleared). Simulated, so the ordering and timing are inferred; the conditions are confirmed.")
P("- **B** plays on load/idle but needs chapter flags (WriteStateFlag dwords given). The jump cannot write them today.")
P("- **C** needs player action (talk, challenge region, fight, race, tags cleared, yes answer).")
P("- **D** is never played by any mission's 0xE3 command (unused, or only loaded).\n")
P("## Model notes\n")
for n in out['model_notes']: P("- " + n)
P("\n## Class A sweep list (one jump each; events in expected order, t = frames after mission start, loads excluded)\n")
P("| Jump | Missions (chain) | Events in order (start frame, length) | Total | Ends |")
P("|---|---|---|---|---|")
for s in sweeps:
    evs = [x for x in s['events']]
    tot = (evs[-1]['t_frame'] + evs[-1]['dur']) if evs else 0
    P("| `%s` | %s | %s | %d f (%.0f s) | %s |" % (s['jump'], "→".join("%04d" % m for m in s['path']),
      ", ".join("%s%s@%d(%d)" % (x['e'], "(talk)" if x['kind'] == 'talk' else ("⚠" if REPORTED.get(x['e']) else ""), x['t_frame'], x['dur']) for x in evs),
      tot, tot / 60, s['end']))
P("\n⚠ = event the player reported a problem with.\n")
P("## Reported events\n")
for eid in sorted(REPORTED):
    e = events[eid]
    P("- **%s** (%s): class %s, %d f, %s. Route: %s" % (eid, REPORTED[eid], e['cls'], e['duration_frames'], e['type'], e['recipe']))
P("- **Rokkaku crows**: no e*.dat has a crow/karasu model or animation path. The crows are stage-side (stg20 objects), not event data. stg20 missions: %s. Class-A entries into stg20: `2:40` (e032 fly-over, then live stg20) and `6:30` (e073). Any stg20 mission shows the live stage after its events." % ", ".join(stg20))
P("\n## Every event\n")
P("| Event | Frames | Type | Class | Mission / block | Condition → writes | Route / recipe |")
P("|---|---|---|---|---|---|---|")
for eid, e in events.items():
    if not e['plays']:
        P("| %s%s | %s | %s | D | – | – | %s |" % (eid, " ⚠" if e['reported'] else "", e['duration_frames'], e['type'],
          ("loaded only: " + "; ".join("%s %s %s" % (l['mission'], l['how'], l['cond']) for l in e['loaded_by'])) if e['loaded_by'] else "unreferenced"))
        continue
    for p in e['plays']:
        extra = ""
        if p['cls'] == 'B': extra = " WriteStateFlag: %s" % ", ".join(p['chapter_flags_writestateflag'])
        if p['talk_triggered']: extra += " [talk-triggered]"
        P("| %s%s | %s | %s | %s | %s %s | %s → %s | %s%s |" % (eid, " ⚠" if e['reported'] else "", e['duration_frames'], e['type'], p['cls'],
          p['mission'], p['block'], ",".join(p['cond']) or "–", ",".join(p['writes']) or "–", p['recipe'].replace('|', '/'), extra))
P("\n## TalkEvents (0xE7, TE*.bin dialogue boxes, not e*.dat)\n")
tc = Counter(min((x['cls'] for x in v), key=lambda c: {'A': 0, 'B/C?': 1, 'C': 2}[c]) for v in tes.values())
P("%d TalkEvent ids referenced; best class: %s. Class A: %s. Details in catalogue.json → talk_events.\n" % (len(tes), dict(tc),
  ", ".join("%s (%s)" % (k, ",".join(sorted({j for x in v for j in x['a_jumps']}))) for k, v in tes.items() if any(x['cls'] == 'A' for x in v))))
open(os.path.join(HERE, 'catalogue.md'), 'w').write("\n".join(L) + "\n")
print(cnt, len(tes))
