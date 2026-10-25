#!/usr/bin/env python3
"""C vs Python: does automatic table selection (scene_tables.json + scene_refs.json) pick the same table for every state file?

    python3 -I tests/run_select.py SCENE_TABLES.json STATE_FILE [STATE_FILE ...]      (state files in the snap.lua layout)
Needs state files that are not in this repository. Exit status 0 only if C and Python agree on all of them."""
import os, sys, subprocess, collections
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'oracle'))
import nb1render as R, depthscene as D
tables, files = sys.argv[1], sys.argv[2:]
tset = D.load_table_set(tables)
out = subprocess.run([os.path.join(os.path.dirname(HERE), 'nb1_tool'), 'select', tables] + files, capture_output=True, text=True, check=True).stdout.strip().split('\n')
agree = 0; dist = collections.Counter(); diff = []
for f, line in zip(files, out):
    c = line.split('\t')[1]; st = R.State(f); p, _ = D.select_table(tset, st, st); p = p or '-'
    dist[p] += 1
    if p == c: agree += 1
    else: diff.append((f, p, c))
print(f'C and Python choose the same table for {agree} of {len(files)} states; tables chosen: {dict(dist)}')
for d in diff[:10]: print('  differ:', d)
sys.exit(0 if not diff else 1)
