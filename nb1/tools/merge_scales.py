#!/usr/bin/env python3
"""Merge the layer-scale edits saved by pbvr (pbvr_layer_edits.jsonl) into a depth table.

    python3 tools/merge_scales.py pbvr_layer_edits.jsonl --scene 17 --table research/data/tables/windows.json [--out merged.json]
    python3 tools/merge_scales.py pbvr_layer_edits.jsonl --list

Each line pbvr writes is one save: the scene (table name, nearest captured scene number, frame) and every scale edit of that scene. The LAST save
of the chosen scene wins. Its scales are written into the table's "scales" array (an entry with the same "match" is replaced, others are kept),
so the table then reproduces what you set in the headset. Without --out the table file is rewritten in place."""
import argparse, json, sys
ap = argparse.ArgumentParser(); ap.add_argument('edits'); ap.add_argument('--scene', type=int); ap.add_argument('--table'); ap.add_argument('--out'); ap.add_argument('--list', action='store_true')
a = ap.parse_args()
saves = [json.loads(l) for l in open(a.edits) if l.strip()]
if a.list or a.scene is None:
    seen = {}
    for s in saves: seen[s['scene']['nearest_scene']] = s
    for k, s in sorted(seen.items()):
        print(f"scene {k}: table '{s['scene']['table']}', frame {s['scene']['game_frame']}, saved {s['saved']}: " + '; '.join(f"{e['layer']} x{e['scale']:.3f} (z {e['z_authored_before']:.1f} -> {e['z_authored_after']:.1f})" for e in s['scales']))
    sys.exit(0)
last = [s for s in saves if s['scene']['nearest_scene'] == a.scene]
if not last: sys.exit(f'no saves for scene {a.scene}')
snip = last[-1]['table_snippet']['scales']
if not a.table: sys.exit('--table is required')
t = json.load(open(a.table)); cur = t.get('scales', [])
for n in snip:
    cur = [c for c in cur if c['match'] != n['match']] + [n]
t['scales'] = cur
json.dump(t, open(a.out or a.table, 'w'), indent=1); print(f"wrote {len(snip)} scale(s) from the save of {last[-1]['saved']} into {a.out or a.table}")
