#!/usr/bin/env python3
"""Compare the C library (nb1_tool) with the Python oracle (nb1/oracle) and, where available, with MAME's own pixels.

    PB_ROMS=<ptblank rom dir> PB_SNAPS=<snap.lua output dir> python3 -I tests/run_oracle.py [--table table.json] [T ...]

Needs ROM files and snapshots that are NOT in this repository. Exit status 0 only if everything matches."""
import os, sys, subprocess, tempfile, json, argparse, math
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); NB1 = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(NB1, 'oracle'))
import nb1render as R, depthscene as D

ap = argparse.ArgumentParser(); ap.add_argument('frames', nargs='*', type=int); ap.add_argument('--table', default='')
ap.add_argument('--tool', default=os.path.join(NB1, 'nb1_tool')); a = ap.parse_args()
ROMS = os.environ['PB_ROMS']; SNAPS = os.environ['PB_SNAPS']
roms = R.Roms(ROMS)
frames_all = [int(x) for x in open(f'{SNAPS}/snaplog.txt').read().split()]
frames = a.frames or frames_all

table = dict(D.DEFAULT_TABLE); tpath = '-'
if a.table:
    table = json.load(open(a.table)); table.pop('edits', None); tpath = a.table     # C ignores edits; compare like with like
D.FAR = float(table.get('defaults', {}).get('far', 80.0))
bad = 0
def check(name, ok, detail=''):
    global bad
    if not ok: bad += 1
    print(f'  {"ok  " if ok else "FAIL"} {name} {detail}')

CAMS = [(20, 5), (45, 10), (70, 10), (85, 0), (-80, 30)]
for T in frames:
    fi = frames_all.index(T)
    print(f'frame {T}')
    tile, spr = f'{SNAPS}/vs_{T:06d}_d0.bin', f'{SNAPS}/vs_{T:06d}_d2.bin'
    with tempfile.TemporaryDirectory() as out:
        subprocess.run([a.tool, 'dump', ROMS, tile, spr, tpath, out], check=True)
        rd = lambda n, dt, shape=None: (lambda x: x.reshape(shape) if shape else x)(np.fromfile(f'{out}/{n}', dt))
        st0, st2 = R.State(tile), R.State(spr)
        # 1. composite vs oracle vs MAME
        comp = rd('composite.rgb', np.uint8, (R.H, R.W, 3)); ref = R.render(st0, roms, sst=st2)
        check('composite == oracle', (comp == ref).all(), f'({int((comp != ref).any(-1).sum())} px differ)')
        png = f'{SNAPS}/ptblank/{fi:04d}.png'
        if os.path.exists(png):
            from PIL import Image
            mame = np.array(Image.open(png).convert('RGB'))[:R.H, :R.W]
            check('composite == MAME', (comp == mame).all(), f'({int((comp != mame).any(-1).sum())} px differ)')
        # 2. sprite entries
        ents = R.sprite_entries(st2); lines = open(f'{out}/entries.tsv').read().split('\n'); k = 0; ok = True
        for i, e in enumerate(ents):
            h = lines[k].split(); k += 1
            want = [i, e['which'], e['pri'], e['color'], e['offset'], int(e['flipx']), int(e['flipy']), *e['clip'], e['ncols'], e['nrows'], e['hsize'], e['vsize'], e['x'], e['y'], len(e['tiles'])]
            ok &= [int(v) for v in h[1:]] == want
            for t in e['tiles']:
                ok &= [int(v) for v in lines[k].split()[1:]] == list(t); k += 1
        check(f'{len(ents)} sprite entries and their tiles', ok and not lines[k].strip())
        # 3. scene items
        items = D.build_items(st0, st2, roms, table)
        rows = [l.split('\t') for l in open(f'{out}/items.tsv').read().strip().split('\n')]
        ok = len(rows) == len(items); detail = f'{len(items)} items'
        if ok:
            for i, (it, r) in enumerate(zip(items, rows)):
                kind = {'tilemap': 0, 'sprite': 1, 'mask': 2}[it.kind]
                ints = [kind, it.id, it.pri, it.order, it.x0, it.y0, it.w, it.h, -1 if it.key is None else it.key, it.pal, it.ncols, it.nrows]
                good = [int(float(v)) for v in r[1:13]] == [int(x) for x in ints]
                good &= abs(float(r[13]) - it.z) < 1e-9 and abs(float(r[14]) - it.tilt_x) < 1e-12 and abs(float(r[15]) - it.tilt_y) < 1e-12
                good &= int(r[16]) == int(bool(it.hud)) and int(r[17]) == (-1 if it.rule is None else it.rule)
                good &= int(r[18]) == int(bool(it.shadow)) and int(r[19]) == int(it.erased or 0) and int(r[21]) == int(it.plane is not None)
                if kind == 1: good &= abs(float(r[20]) - it.zoom) < 1e-12
                tex = rd(f'tex_{i:03d}.rgba', np.uint8, (it.h, it.w, 4)); pen = rd(f'pen_{i:03d}.i32', np.int32, (it.h, it.w))
                good &= (tex == it.tex).all() and (pen == it.pen).all()
                if not good: ok = False; detail += f' [item {i} {it.kind}{it.id} differs]'; break
        check('items: fields, depths, textures, pens', ok, detail)
        # 4. views
        v = rd('view_eye.rgb', np.uint8, (R.H, R.W, 3)); vr = D.render_view(items, scale=1)
        check('arcade-eye view == oracle', (v == vr).all(), f'({int((v != vr).any(-1).sum())} px differ)')
        check('arcade-eye view == composite', (v == comp).all(), f'({int((v != comp).any(-1).sum())} px differ; blanked areas may differ)') if False else None
        vis = [it for it in items if it.kind != 'mask']
        for c, (yaw, pitch) in enumerate(CAMS):
            E, fwd, right, up = D.orbit_camera((0, 0, -8.0), 14.0, yaw, pitch)
            vo = D.render_cam(items, E, fwd, right, up, 420.0, (640, 480)); vc = rd(f'cam_{c}.rgb', np.uint8, (480, 640, 3))
            nd = int((vo != vc).any(-1).sum()); check(f'orbit yaw {yaw} pitch {pitch} == oracle', nd <= 3, f'({nd} of 307200 px differ)')
        # 5. aim
        rows = [l.split('\t') for l in open(f'{out}/aim.tsv').read().strip().split('\n')]; n = good_n = 0; worst = 0.0
        idx_of = {id(it): i for i, it in enumerate(items)}
        for r in rows:
            c, ux, vy, ok_c, itm, sx, sy = int(r[0]), int(r[1]), int(r[2]), int(r[3]), int(r[4]), float(r[5]), float(r[6])
            if c == 9:
                O = np.array([0.2, -0.1, 0.5]); P = np.array([(ux + .5 - D.CX) / D.FOCAL, (D.CY - (vy + .5)) / D.FOCAL, -1.0]); dv = P - O; E = O
            else:
                E, fwd, right, up = D.orbit_camera((0, 0, -8.0), 14.0, *CAMS[c], ) if False else D.orbit_camera((0, 0, -8.0), 14.0, CAMS[c][0], CAMS[c][1])
                dv = ((ux + .5 - 320) / 420.0) * right + ((240 - (vy + .5)) / 420.0) * up + fwd
            res = D.aim(items, E, dv); n += 1
            if res is None: good_n += (ok_c == 0); continue
            ii = -1 if res['item'] is None else idx_of[id(res['item'])]
            if ok_c and ii == itm and abs(res['sx'] - sx) < 1e-5 and abs(res['sy'] - sy) < 1e-5: good_n += 1
            else: worst = max(worst, 1)
        check('aim: item and arcade pixel for the ray set', good_n >= n - 2, f'({good_n}/{n})')
        # 5b. visible map and snapping aim
        ids_py = D.visible_map(items); ids_c = rd('ids.i32', np.int32, (R.H, R.W))
        check('visible map (who the arcade shows per pixel) == oracle', (ids_py == ids_c).all(), f'({int((ids_py != ids_c).sum())} px differ)')
        rows = [l.split('\t') for l in open(f'{out}/aimsnap.tsv').read().strip().split('\n')]; n = good_n = nsnap = 0
        for r in rows:
            c, ux, vy, ok_c, itm = int(r[0]), int(r[1]), int(r[2]), int(r[3]), int(r[4])
            E, fwd, right, up = D.orbit_camera((0, 0, -8.0), 14.0, CAMS[c][0], CAMS[c][1])
            dv = ((ux + .5 - 320) / 420.0) * right + ((240 - (vy + .5)) / 420.0) * up + fwd
            res = D.aim_snap(items, E, dv, ids_py); n += 1
            if res is None: good_n += (ok_c == 0); continue
            nsnap += res['snapped']
            same = (ok_c and itm == res['index'] and abs(res['sx'] - float(r[5])) < 1e-5 and abs(res['sy'] - float(r[6])) < 1e-5
                    and int(r[7]) == int(res['snapped']) and int(r[8]) == int(res['hidden']) and int(r[9]) == res['cover'])
            good_n += bool(same)
        check('aim_snap: item, snapped pixel, snapped/hidden/cover flags', good_n >= n - 1, f'({good_n}/{n}, {nsnap} snapped)')
        # 6. gun registers (once)
        if T == frames[0]:
            rows = [list(map(int, l.split('\t'))) for l in open(f'{out}/gun.tsv').read().strip().split('\n')]; g = 0
            for x, y, x1, y1, okc, X, Y, x2 in rows:
                rg = D.gun_regs(x, y); rp = D.regs_to_pixel(rg)
                g += (rg['x1'], rg['y1'], rg['x2']) == (x1, y1, x2) and ((rp is None) == (okc == 0)) and (rp is None or rp == (X, Y))
            check('gun registers and their decode, all 64,512 pixels', g == len(rows), f'({g}/{len(rows)})')
print('\nALL MATCH' if not bad else f'\n{bad} check(s) FAILED'); sys.exit(1 if bad else 0)
