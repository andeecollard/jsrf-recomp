#!/usr/bin/env python3
"""Turn an observed drive into a route of waypoints the harness can walk.

Why this exists. Line-of-sight steering leaves the Garage and then circles about
280 units short of the next character: following a wall it is touching is not
the same as knowing the way. The way has to come from somebody who does, and the
observer already samples the position at 20 Hz while they drive.

Why not the pad recordings. Their checkpoints are input hashes with a zero guest
anchor -- `#!ck 256 500 9c2c…77 00000000`, and every anchor in the file reads
zero -- so a checkpoint can say the same buttons were pressed and cannot say
where anybody was. The recordings are also stamped `#!gen c4400dbb0025f56d`
while this tree generates `46bb115cdb053f4e`, so the runtime refuses them
outright. A route is world coordinates: it is the game's data, not the
translator's, and it survives a regeneration that retires every recording.
"""
import argparse
import json
import math
from pathlib import Path


def samples(run):
    """Player 44's position over an observed run, with its frame and scene."""
    path = run / 'events.jsonl'
    for line in path.read_text().splitlines():
        event = json.loads(line)
        if event.get('event') != 'sample':
            continue
        s = event['state']
        who = (s.get('cplayers') or s.get('players') or {}).get('44')
        if who and s.get('table_ok'):
            yield {'at': (who['position'][0], who['position'][2]), 'y': who['position'][1],
                   'frame': s['frame'], 'sequence': s['sequence'], 'state': who['state']}


def waypoints(track, spacing, corner):
    """Keep a point every `spacing` units, and every turn sharper than `corner`
    degrees. Spacing alone cuts corners the drive went round for a reason."""
    kept = []
    corner = math.radians(corner)
    for point in track:
        if not kept:
            kept.append(point)
            continue
        last = kept[-1]
        moved = math.dist(point['at'], last['at'])
        turned = False
        if len(kept) >= 2:
            a = (last['at'][0] - kept[-2]['at'][0], last['at'][1] - kept[-2]['at'][1])
            b = (point['at'][0] - last['at'][0], point['at'][1] - last['at'][1])
            if math.hypot(*a) > 1 and math.hypot(*b) > 1:
                turn = abs(math.atan2(a[0] * b[1] - a[1] * b[0], a[0] * b[0] + a[1] * b[1]))
                turned = turn > corner and moved > spacing / 4
        if moved >= spacing or turned:
            kept.append(point)
    return kept


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run', type=Path, help='An observed run directory')
    ap.add_argument('--out', type=Path, help='Where to write the route JSON')
    ap.add_argument('--spacing', type=float, default=40.0)
    ap.add_argument('--corner', type=float, default=35.0, help='Degrees')
    ap.add_argument('--sequence', type=int, help='Keep only this scene')
    args = ap.parse_args()
    track = [p for p in samples(args.run)
             if args.sequence is None or p['sequence'] == args.sequence]
    if len(track) < 2:
        raise SystemExit('Not enough readings in %s; was the run observed?' % args.run)
    kept = waypoints(track, args.spacing, args.corner)
    walked = sum(math.dist(a['at'], b['at']) for a, b in zip(track, track[1:]))
    route = {'source': str(args.run), 'readings': len(track), 'waypoints': len(kept),
             'units_walked': round(walked, 1),
             'scenes': sorted({p['sequence'] for p in track}),
             'claim': 'One drive, decimated. It is a path somebody took, not the only path '
                      'or the best one, and nothing here checks it is still walkable.',
             'route': [[round(p['at'][0], 1), round(p['at'][1], 1)] for p in kept]}
    text = json.dumps(route, indent=2) + '\n'
    if args.out:
        args.out.write_text(text)
        print('%d readings over %.0f units -> %d waypoints: %s'
              % (len(track), walked, len(kept), args.out))
    else:
        print(text)


if __name__ == '__main__':
    main()
