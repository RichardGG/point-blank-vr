#!/usr/bin/env python3
"""Point Blank (NB-1) depth-scene prototype.

* build_items()   : one RGBA quad per C123 tilemap layer and per C355 sprite entry (a sprite entry is already
                    a whole multi-tile object), each given a depth by a rule table (depth_table.json) with an
                    automatic rank-based default so an untagged frame still looks coherent.
* render_view()   : painter's-algorithm software view of those quads from an arbitrary eye (stereo / head sway).
* aim()           : controller ray -> nearest opaque texel -> projected back through the *arcade eye* to an
                    arcade screen pixel (mirrors qvr_aim() in quest_scene.c).
* gun_regs()      : arcade pixel -> the four NB-1 gun registers the game's 0xa378 routine expects.

Conventions follow quest_scene.c: arcade eye at the origin, looking down -z, metres, +y up;
unproject(x,y,z) = ((x-cx)*z/f, (cy-y)*z/f, -z).
"""
import json, math, os
import numpy as np
import nb1render as R

W, H = R.W, R.H
CX, CY = W / 2.0, H / 2.0
FOCAL = 346.0            # px; ~45 deg horizontal FOV, like Time Crisis' 772px on a 640px screen (a choice, not measured)

def unproject(x, y, z):
    return np.array([(x - CX) * z / FOCAL, (CY - y) * z / FOCAL, -z])

def project_arcade(p):
    """World point -> arcade screen pixel as seen from the arcade eye (origin)."""
    return CX + FOCAL * p[0] / -p[2], CY - FOCAL * p[1] / -p[2]

class Item:
    __slots__ = ('kind', 'id', 'pri', 'order', 'x0', 'y0', 'w', 'h', 'tex', 'key', 'pal', 'ncols', 'nrows',
                 'z', 'tilt_x', 'tilt_y', 'hud', 'rule', 'pen', 'rgb', 'shadow', 'listidx', 'erased', 'cust', 'part', 'name', 'zoom', 'plane', 'decal')
    def __init__(self, **k):
        for s in self.__slots__: setattr(self, s, k.get(s))
        self.z = k.get('z', 0.0); self.tilt_x = k.get('tilt_x', 0.0); self.tilt_y = k.get('tilt_y', 0.0)
        self.hud = k.get('hud', False); self.decal = bool(k.get('decal', False))
    def corners(self):
        """Centre, half-width vector, half-height vector of the quad in world space (tilt about its own centre)."""
        ctr = unproject(self.x0 + self.w / 2.0, self.y0 + self.h / 2.0, self.z)
        r = np.array([self.w / 2.0 * self.z / FOCAL, 0, 0]); u = np.array([0, self.h / 2.0 * self.z / FOCAL, 0])
        if self.tilt_y:     # rotate about the vertical axis through the centre
            a = math.radians(self.tilt_y); c, s = math.cos(a), math.sin(a)
            r = np.array([r[0] * c, 0, -r[0] * s])
        if self.tilt_x:     # rotate about the horizontal axis through the centre
            a = math.radians(self.tilt_x); c, s = math.cos(a), math.sin(a)
            u = np.array([0, u[1] * c, u[1] * s])
        return ctr, r, u

# ------------------------------------------------------------------ depth table
# Decals: sprites that are drawn ONTO whatever is underneath (shot marks).  They take their depth, plane and tilt from the item the
# arcade drew them over, so a bullet hole lies on the surface it hit.  Table key "decals": [[lo, hi], ...] (sprite tile codes);
# omitted = the default shot-mark tiles; [] = none.  Decals never block the aim or the visible map.
DEFAULT_DECALS = [[0xee5, 0xee7]]
DECAL_EPS = 0.002

DEFAULT_TABLE = {
    "defaults": {"z_front": 4.0, "z_back": 14.0, "hud_z": 2.0, "slot_gap": 0.0005},
    "rules": [
        {"match": {"kind": "tilemap", "layer": [4, 5]}, "hud": True},
        {"match": {"kind": "sprite", "code": [3799, 3860]}, "hud": True}      # timer digits/ring (3799-3820) and the ammo/heart pill (3857)
    ]
}

def _hit(match, it):
    for k, v in match.items():
        if k == 'kind' and it.kind != v: return False
        if k == 'layer' and (it.kind != 'tilemap' or it.id not in v): return False
        if k == 'pri' and it.pri not in (v if isinstance(v, list) else [v]): return False
        if k == 'pal' and (it.pal not in v): return False
        if k == 'code' and not (it.key is not None and v[0] <= it.key <= v[1]): return False
        if k == 'size' and not (it.ncols == v[0] and it.nrows == v[1]): return False
        if k == 'part' and it.part != v: return False
        if k == 'center_x' and not (v[0] <= it.x0 + it.w / 2.0 < v[1]): return False
        if k == 'center_y' and not (v[0] <= it.y0 + it.h / 2.0 < v[1]): return False
        if k == 'id' and it.id not in (v if isinstance(v, list) else [v]): return False
    return True

def assign_depths(items, table=DEFAULT_TABLE):
    d = table.get('defaults', DEFAULT_TABLE['defaults'])
    explicit = set()
    for it in items:
        it.rule = None
        for n, r in enumerate(table.get('rules', [])):
            if _hit(r['match'], it):
                it.rule = n
                if r.get('hud'): it.hud = True
                if 'z' in r: it.z = float(r['z']); explicit.add(id(it))
                if 'plane' in r:       # projected plane: {"y": -1.1} floor, {"y": 0.8} ceiling, {"x": -1.8} left wall ...
                    (ax, v), = r['plane'].items(); nrm = {'x': (1, 0, 0), 'y': (0, 1, 0), 'z': (0, 0, 1)}[ax]
                    it.plane = (np.array(nrm, float), float(v)); explicit.add(id(it))
                if 'floor_contact' in r:       # object standing on a floor plane y=v (v<0): its bottom edge fixes the depth
                    v = float(r['floor_contact']); dy = (it.y0 + it.h) - CY
                    if dy > 1: it.z = float(min(max(FOCAL * abs(v) / dy, 1.5), FAR)); explicit.add(id(it))
                if 'ceiling_contact' in r:     # hanging from a ceiling plane y=v (v>0): its top edge fixes the depth
                    v = float(r['ceiling_contact']); dy = CY - it.y0
                    if dy > 1: it.z = float(min(max(FOCAL * abs(v) / dy, 1.5), FAR)); explicit.add(id(it))
                if 'zoom_depth' in r and it.zoom:      # pseudo-3D scenes: apparent size ~ 1/depth  =>  z = K / zoom (metres)
                    lo, hi = r.get('zoom_clamp', [1.5, 40.0])
                    it.z = float(min(max(r['zoom_depth'] / max(it.zoom, 0.02), lo), hi)); explicit.add(id(it))
                it.tilt_x = r.get('tilt_x', 0.0); it.tilt_y = r.get('tilt_y', 0.0)
                break
    # HUD art stays on one flat plane at the arcade-screen distance
    for it in items:
        if it.hud: it.z = d['hud_z']; explicit.add(id(it))
    # everything else without an explicit depth: a FIXED slot per (priority, kind), so the depth of an untagged layer
    # never changes when another slot appears or disappears (16 slots: pri 0..7 x tilemap/sprite, back to front)
    step = (d['z_back'] - d['z_front']) / 15.0
    for it in items:
        if id(it) not in explicit:
            it.z = d['z_back'] - step * (it.pri * 2 + (0 if it.kind == 'tilemap' else 1))
    # strict ordering inside a depth plane: later-drawn is closer (the arcade's last-wins rule)
    for it in items:
        if it.plane is not None:                     # a projected plane's depth = where the arcade eye's centre ray meets it
            n, dd = it.plane; ray = unproject(it.x0 + it.w / 2.0, it.y0 + it.h / 2.0, 1.0)
            den = float(ray @ n)
            it.z = float(-(ray * (dd / den))[2]) if abs(den) > 1e-9 and dd / den > 0 else 30.0
        it.z -= d['slot_gap'] * it.order
    # an UNTAGGED item the arcade draws after something must not end up behind it (e.g. a pri-7 fade/shadow sprite is
    # drawn after the flat HUD tilemap and darkens it): clamp automatic depths to stay in front of everything earlier.
    # Explicitly tagged items are left exactly where the table put them.
    floor = float('inf')
    for it in sorted(items, key=lambda i: i.order):
        if id(it) not in explicit and it.z >= floor:
            it.z = floor - d['slot_gap']
        floor = min(floor, it.z)
    return items

def _is_decal_code(it, ranges):
    return it.kind == 'sprite' and it.key is not None and any(lo <= it.key <= hi for lo, hi in ranges)

def _place_decal(m, owner):
    """Give decal piece m the plane / tilt / depth of the surface `owner` it lies on."""
    m.hud = owner.hud; m.tilt_x = owner.tilt_x; m.tilt_y = owner.tilt_y; m.plane = owner.plane
    ray = unproject(m.x0 + m.w / 2.0, m.y0 + m.h / 2.0, 1.0)
    if owner.hud:
        m.z = owner.z - DECAL_EPS
    elif owner.plane is not None:
        n, dd = owner.plane; den = float(ray @ n)
        m.z = float(-(ray * (dd / den))[2]) - DECAL_EPS if abs(den) > 1e-9 and dd / den > 0 else owner.z - DECAL_EPS
    else:
        ctr, r, u = owner.corners(); n = np.cross(r, u); den = float(ray @ n)
        t = float((ctr @ n) / den) if abs(den) > 1e-12 else -1.0
        m.z = (t if t > 0 else owner.z) - DECAL_EPS

def apply_decals(items, table=DEFAULT_TABLE):
    """Each decal pixel belongs to the surface the arcade eye sees there among the items drawn before the decal.  A decal that
    straddles two surfaces is split into one piece per surface, each lying on its own, so the arcade-eye picture stays exact.
    The first (largest) piece replaces the decal in place; the others are appended."""
    ranges = table.get('decals', DEFAULT_DECALS)
    extra = []
    for m in list(items):
        if not _is_decal_code(m, ranges): continue
        m.decal = True
        # the surface under each decal pixel = what the arcade eye sees there among the items drawn BEFORE the decal
        # (the z-buffer decides, so a depth table that puts an earlier item in front of a later one is respected)
        under = [n for n, o in enumerate(items) if o is not m and o.kind != 'mask' and o.order < m.order and not _is_decal_code(o, ranges)]
        sub = np.full((H, W), -1, np.int32)
        render_cam([items[n] for n in under], (0, 0, 0), np.array([0, 0, -1.0]), np.array([1.0, 0, 0]), np.array([0, 1.0, 0]), FOCAL, (W, H), ids=sub) if under else None
        own = np.full((m.h, m.w), -1, np.int32); mine = m.tex[..., 3] > 0
        x0, y0, x1, y1 = max(m.x0, 0), max(m.y0, 0), min(m.x0 + m.w, W), min(m.y0 + m.h, H)
        if x1 > x0 and y1 > y0:
            blk = sub[y0:y1, x0:x1]; mapped = np.where(blk >= 0, np.array(under + [-1])[np.where(blk >= 0, blk, len(under))], -1)
            own[y0 - m.y0:y1 - m.y0, x0 - m.x0:x1 - m.x0] = mapped
        owners = sorted({int(v) for v in own[mine & (own >= 0)]}, key=lambda n: (-int((own[mine] == n).sum()), n))
        if not owners: continue
        orig_tex, orig_pen = m.tex.copy(), m.pen.copy()
        for k, n in enumerate(owners):
            sel = mine & ((own == n) | ((own < 0) if k == 0 else False))
            piece = m if k == 0 else Item(**{sl: getattr(m, sl) for sl in Item.__slots__})
            piece.decal = True
            piece.tex = orig_tex.copy(); piece.tex[~sel] = 0
            piece.pen = np.where(sel, orig_pen, -1).astype(np.int32)
            _place_decal(piece, items[n])
            if k: extra.append(piece)
    items.extend(extra)
    return items


# ------------------------------------------------------------------ automatic table selection (scene_tables.json)
def art_pairs(st):
    """{(layer, tile code)} of the enabled scrolling tilemap layers 0-3: the background art on screen, independent of scroll position."""
    out = set()
    for L in range(4):
        if int(st.ctl[0x10 + L]) & 8: continue
        out |= {(L, int(c)) for c in np.unique(st.vram[R.TM_OFF[L]:R.TM_OFF[L] + 4096])}
    return out

def sprite_codes(sst):
    """First-tile code (bank + offset applied) of every sprite entry: the same key items carry."""
    codes = set()
    for sp in R.sprite_entries(sst):
        if sp['tiles']:
            t = sp['tiles'][0][0]; codes.add(((t & 0x7ff) | (int(sst.bank[t >> 11]) << 11)) + sp['offset'])
    return codes

def load_table_set(path):
    """scene_tables.json (+ scene_refs.json beside it).  Entry: {"name", "table", "when": {"codes_all", "codes_any", "art_scenes", "min_similarity"}}.
    Entries with codes_* match exactly; entries with art_scenes match when the state's background art is at least min_similarity (default 0.8,
    Jaccard on (layer, tile code) pairs) like one of those captured reference scenes.  Codes entries win in order; among art entries the
    most similar wins."""
    base = os.path.dirname(os.path.abspath(path)); rules = json.load(open(path))
    rp = os.path.join(base, 'scene_refs.json'); refs = {k: {(int(L), c) for L, cs in v.items() for c in cs} for k, v in json.load(open(rp)).items()} if os.path.exists(rp) else {}
    return dict(base=base, rules=rules, refs=refs)

def select_table(tset, st, sst=None):
    """-> (entry name, table dict with '_path') or (None, None)."""
    sst = sst or st; codes = None; pairs = None; best = None
    for r in tset['rules']:
        w = r['when']
        if 'art_scenes' in w:
            if pairs is None: pairs = art_pairs(st)
            sim = 0.0
            for sid in w['art_scenes']:
                ref = tset['refs'].get(str(sid))
                if ref:
                    u = pairs | ref
                    sim = max(sim, len(pairs & ref) / len(u) if u else 0.0)
            if sim >= w.get('min_similarity', 0.8) and (best is None or sim > best[0] + 1e-12): best = (sim, r)
        else:
            if codes is None: codes = sprite_codes(sst)
            if all(c in codes for c in w.get('codes_all', [])) and (not w.get('codes_any') or any(c in codes for c in w['codes_any'])):
                best = (2.0, r); break          # exact code matches take precedence, first one wins
    if best is None: return None, None
    r = best[1]; p = os.path.join(tset['base'], r['table']); t = json.load(open(p)); t['_path'] = p
    return r['name'], t

# ------------------------------------------------------------------ editing: your own art, punch-outs, splits
def _label(it):
    return f"{it.kind}{it.id}" + (f"_{it.key}" if it.key is not None else "")

def to_custom(it):
    """Convert an indexed item to RGB 'custom' texels (pen -4) so its pixels can be edited freely."""
    if it.cust is not None: return
    cust = np.zeros((it.h, it.w, 3), np.uint8)
    ok = it.pen >= 0
    cust[ok] = it.rgb[it.pen[ok] & 0x1fff]
    it.cust = cust; it.pen = np.where(ok, -4, it.pen)

def _fill(img, hole, alpha, spec, only=None):
    """spec: "inpaint" (OpenCV Telea) or {"clone": [[dx,dy], ...]} = copy from those offsets (first one that lands on art wins);
    repeating backdrops (fences, walls, gravel) look far better cloned; remaining holes fall back to inpainting."""
    if isinstance(spec, dict) and 'clone' in spec:
        img = img.copy(); todo = (hole if only is None else only) & (alpha > 0)
        for dx, dy in spec['clone']:
            ys, xs = np.nonzero(todo)
            sy, sx = ys + dy, xs + dx
            ok = (sy >= 0) & (sy < img.shape[0]) & (sx >= 0) & (sx < img.shape[1])
            ok[ok] = (alpha[sy[ok], sx[ok]] > 0) & ~hole[sy[ok], sx[ok]]
            img[ys[ok], xs[ok]] = img[sy[ok], sx[ok]]
            todo[ys[ok], xs[ok]] = False
        return _inpaint(img, todo, alpha)
    return _inpaint(img, hole if only is None else only, alpha)

_INPAINT_W = {}
for _dy in range(-2, 3):
    for _dx in range(-2, 3):
        if _dx or _dy: _INPAINT_W[(_dx, _dy)] = 12 // (_dx * _dx + _dy * _dy)      # 12, 6, 3, 2, 1: integer weights by squared distance

def _inpaint(img, hole, alpha):
    """Deterministic fill (same in the C port, integer arithmetic only): holes are filled in layers from their edge inwards; each
    layer pixel becomes the weighted mean of the opaque, already-known pixels in its 5x5 neighbourhood.  Hole pixels with nothing known
    near them (the whole sprite is hole) are left alone.  Replaces OpenCV's Telea inpaint, which cannot be reproduced bit-exactly in C."""
    hole = hole & (alpha > 0)
    if not hole.any(): return img
    ys, xs = np.nonzero(hole)
    y0, y1, x0, x1 = max(ys.min() - 2, 0), min(ys.max() + 3, img.shape[0]), max(xs.min() - 2, 0), min(xs.max() + 3, img.shape[1])
    h, w = y1 - y0, x1 - x0
    pad = np.zeros((h + 4, w + 4, 3), np.int64); known = np.zeros((h + 4, w + 4), bool)
    pad[2:-2, 2:-2] = img[y0:y1, x0:x1]; known[2:-2, 2:-2] = (alpha[y0:y1, x0:x1] > 0) & ~hole[y0:y1, x0:x1]
    remaining = np.zeros((h + 4, w + 4), bool); remaining[2:-2, 2:-2] = hole[y0:y1, x0:x1]
    while remaining.any():
        near = np.zeros_like(remaining)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dx or dy: near[2:-2, 2:-2] |= known[2 + dy:h + 2 + dy, 2 + dx:w + 2 + dx]
        cand = remaining & near
        if not cand.any(): break
        S = np.zeros((h, w, 3), np.int64); D = np.zeros((h, w), np.int64)
        for (dx, dy), wt in _INPAINT_W.items():
            k = known[2 + dy:h + 2 + dy, 2 + dx:w + 2 + dx]
            S += wt * pad[2 + dy:h + 2 + dy, 2 + dx:w + 2 + dx] * k[..., None]; D += wt * k
        c = cand[2:-2, 2:-2]; Dc = np.maximum(D, 1)[..., None]
        val = (2 * S + Dc) // (2 * Dc)
        inner = pad[2:-2, 2:-2]; inner[c] = val[c]
        known |= cand; remaining &= ~cand
    out = img.copy(); sel = hole[y0:y1, x0:x1]; out[y0:y1, x0:x1][sel] = pad[2:-2, 2:-2].astype(np.uint8)[sel]
    return out

def _round_half_up(v): return int(math.floor(float(v) + 0.5)) if v >= 0 else -int(math.floor(abs(float(v)) + 0.5))
def _round_half_down(v): return int(math.ceil(float(v) - 0.5)) if v >= 0 else -int(math.ceil(abs(float(v)) - 0.5))

def fill_polygon(pts, w, h):
    """Scanline polygon fill with integer vertices (same in the C port). Per row: crossings of every edge that spans the row
    (both end rows included, an edge's lower end counted twice), float32 arithmetic, filled from round-half-up(left) to
    round-half-down(right).  Identical to Pillow's ImageDraw.polygon on every polygon the tables use; it differs from Pillow by a few
    pixels at thin vertices of some other shapes."""
    f32 = np.float32
    n = len(pts); edges = []
    for i in range(n):
        (x0, y0), (x1, y1) = pts[i], pts[(i + 1) % n]
        edges.append((x0, y0, x1, y1, min(y0, y1), max(y0, y1), f32(0) if y0 == y1 else f32(f32(x1 - x0) / f32(y1 - y0))))
    m = np.zeros((h, w), bool)
    if n == 0: return m
    pymin, pymax = min(e[4] for e in edges), max(e[5] for e in edges)
    def hline(xa, y, xb):
        if 0 <= y < h:
            xa = max(xa, 0); xb = min(xb, w - 1)
            if xb >= xa: m[y, xa:xb + 1] = True
    act = []
    for e in edges:
        if e[4] == e[5]: hline(min(e[0], e[2]), e[4], max(e[0], e[2]))
        else: act.append(e)
    for y in range(max(pymin, 0), min(pymax, h - 1) + 1):
        xx = []
        for (x0, y0, x1, y1, ymin, ymax, dx) in act:
            if ymin <= y <= ymax: xx.append(f32(f32(f32(y - y0) * dx) + f32(x0)))
            if y == ymax and y < pymax: xx.append(xx[-1])
        xx.sort()
        for i in range(1, len(xx), 2): hline(_round_half_up(xx[i - 1]), y, _round_half_down(xx[i]))
    return m

def _dilate_rect(mask, g):
    """Binary dilation with a (2g+1)x(2g+1) square."""
    out = mask.copy(); h, w = mask.shape
    for dy in range(-g, g + 1):
        for dx in range(-g, g + 1):
            ys, ye, xs, xe = max(dy, 0), h + min(dy, 0), max(dx, 0), w + min(dx, 0)
            out[ys:ye, xs:xe] |= mask[ys - dy:ye - dy, xs - dx:xe - dx]
    return out

def _region_mask(it, spec, base):
    """Boolean mask over the item's texture for a rect [x0,y0,x1,y1] (arcade pixels) or a PNG mask (any alpha>0)."""
    m = np.zeros((it.h, it.w), bool)
    if 'rect' in spec:
        x0, y0, x1, y1 = spec['rect']
        m[max(y0 - it.y0, 0):max(y1 - it.y0, 0), max(x0 - it.x0, 0):max(x1 - it.x0, 0)] = True
    elif 'poly' in spec:      # polygon(s) in arcade pixel coordinates: [[x,y],...] or a list of such lists
        polys = spec['poly'] if isinstance(spec['poly'][0][0], list) else [spec['poly']]
        mm = np.zeros((H, W), bool)
        for p in polys: mm |= fill_polygon([(int(a), int(b)) for a, b in p], W, H)
        for yy in range(it.h):
            sy = it.y0 + yy
            if 0 <= sy < H:
                xs0, xs1 = max(it.x0, 0), min(it.x0 + it.w, W)
                if xs1 > xs0: m[yy, xs0 - it.x0:xs1 - it.x0] = mm[sy, xs0:xs1]
    elif 'mask' in spec:
        from PIL import Image
        a = np.array(Image.open(os.path.join(base, spec['mask'])).convert('RGBA'))
        mm = (a[..., 3] > 0) & (a[..., :3].sum(-1) > 0) if spec.get('mask_white', True) else a[..., 3] > 0
        # mask PNGs are in arcade screen coordinates (288x224)
        for yy in range(it.h):
            sy = it.y0 + yy
            if 0 <= sy < a.shape[0]:
                xs0, xs1 = max(it.x0, 0), min(it.x0 + it.w, a.shape[1])
                if xs1 > xs0: m[yy, xs0 - it.x0:xs1 - it.x0] = mm[sy, xs0:xs1]
    elif 'from_item' in spec:
        pass
    return m

def apply_edits(items, table, base='.'):
    """table['edits'] = ordered list of
         {"match": {...}, "texture": "file.png"}                                  replace the item's art (RGBA PNG, arcade pixel size of its box)
         {"match": {...}, "punch": {"from": {...match of the layers to cut}, "grow": 1, "fill": "inpaint"}}
                                    cut this item's opaque shape out of the matched layers (an occluder sprite that duplicates background art)
         {"match": {...}, "split": [{"part": "name", "rect"|"mask": ..., "z": m, "tilt_x": deg, "fill": "inpaint"}, ...]}
                                    cut regions of the item into separate depth pieces; what is left behind is inpainted
       Matching uses the same keys as depth rules (kind, layer, pri, pal, code, size, id, part)."""
    from PIL import Image
    out = list(items)
    for e in table.get('edits', []):
        targets = [it for it in out if _hit(e['match'], it)]
        for it in targets:
            if 'texture' in e:
                a = np.array(Image.open(os.path.join(base, e['texture'])).convert('RGBA'))
                if a.shape[:2] != (it.h, it.w):      # nearest-neighbour resize by exact integer centre sampling (same in the C port)
                    ix = ((2 * np.arange(it.w) + 1) * a.shape[1]) // (2 * it.w); iy = ((2 * np.arange(it.h) + 1) * a.shape[0]) // (2 * it.h)
                    a = a[iy][:, ix]
                it.cust = a[..., :3].copy(); it.tex = a.copy()
                it.pen = np.where(a[..., 3] > 0, -4, -1).astype(np.int32)
            if 'punch' in e:
                pu = e['punch']
                opaque = it.tex[..., 3] > 0
                if pu.get('grow'): opaque = _dilate_rect(opaque, int(pu['grow']))
                for tgt in [t for t in out if t is not it and _hit(pu['from'], t)]:
                    # overlap in screen coordinates
                    x0, y0 = max(it.x0, tgt.x0), max(it.y0, tgt.y0)
                    x1, y1 = min(it.x0 + it.w, tgt.x0 + tgt.w), min(it.y0 + it.h, tgt.y0 + tgt.h)
                    if x1 <= x0 or y1 <= y0: continue
                    hole = np.zeros((tgt.h, tgt.w), bool)
                    hole[y0 - tgt.y0:y1 - tgt.y0, x0 - tgt.x0:x1 - tgt.x0] = opaque[y0 - it.y0:y1 - it.y0, x0 - it.x0:x1 - it.x0]
                    hole &= tgt.tex[..., 3] > 0
                    if not hole.any(): continue
                    to_custom(tgt)
                    if pu.get('fill', 'inpaint') != 'none':
                        tgt.cust = _fill(tgt.cust, hole, tgt.tex[..., 3], pu.get('fill', 'inpaint'))
                    else:
                        tgt.tex = tgt.tex.copy(); tgt.tex[hole, 3] = 0; tgt.pen = np.where(hole, -1, tgt.pen)
                    tgt.erased = (tgt.erased or 0) + int(hole.sum())
                    t = tgt.tex.copy(); t[..., :3] = tgt.cust; tgt.tex = t
            if 'split' in e:
                to_custom(it)
                orig = it.cust.copy(); alpha0 = it.tex[..., 3] > 0
                remaining = alpha0.copy(); cuts = []
                for part in e['split']:
                    m = _region_mask(it, part, base) & remaining
                    if not m.any(): continue
                    remaining &= ~m; cuts.append((m, part))
                    ys, xs = np.nonzero(m)
                    cy0, cy1, cx0, cx1 = int(ys.min()), int(ys.max()) + 1, int(xs.min()), int(xs.max()) + 1
                    sub = m[cy0:cy1, cx0:cx1]
                    tex = np.zeros((cy1 - cy0, cx1 - cx0, 4), np.uint8)
                    tex[sub, :3] = orig[cy0:cy1, cx0:cx1][sub]; tex[sub, 3] = 255
                    child = Item(kind=it.kind, id=it.id, pri=it.pri, order=it.order, x0=it.x0 + cx0, y0=it.y0 + cy0,
                                 w=cx1 - cx0, h=cy1 - cy0, tex=tex, key=it.key, pal=it.pal, ncols=it.ncols, nrows=it.nrows,
                                 pen=np.where(sub, -4, -1).astype(np.int32), rgb=it.rgb, listidx=it.listidx, part=part.get('part'))
                    child.cust = tex[..., :3].copy(); child.hud = it.hud
                    if 'z' in part: child.z = part['z']
                    child.name = part.get('part')
                    out.append(child)
                union = np.zeros_like(alpha0)
                for m, _ in cuts: union |= m
                fillimg = orig.copy()
                for m, part in cuts:       # the parent keeps plausible art behind each piece (visible when the head moves)
                    if part.get('fill', 'inpaint') != 'none':
                        fillimg = _fill(fillimg, union, alpha0, part.get('fill', 'inpaint'), only=m)
                    else:
                        pass
                it.cust = fillimg
                t = it.tex.copy(); t[..., :3] = fillimg
                for m, part in cuts:
                    if part.get('fill', 'inpaint') == 'none': t[m, 3] = 0
                it.tex = t
                it.pen = np.where(it.tex[..., 3] > 0, -4, -1).astype(np.int32)
    return out

def export_items(items, outdir):
    """Write every item as RGBA PNG (+ manifest.json): the starting point for repainting. Reload with a "texture" edit."""
    from PIL import Image
    os.makedirs(outdir, exist_ok=True)
    man = []
    for it in items:
        if it.kind == 'mask': continue
        a = it.tex.copy()
        name = f"{_label(it)}_pri{it.pri}" + (f"_{it.part}" if it.part else "") + ".png"
        Image.fromarray(a).save(os.path.join(outdir, name))
        man.append(dict(file=name, kind=it.kind, id=it.id, pri=it.pri, code=it.key, pal=it.pal, x0=int(it.x0), y0=int(it.y0), w=int(it.w), h=int(it.h), z=round(float(it.z), 3), part=it.part))
    json.dump(man, open(os.path.join(outdir, 'manifest.json'), 'w'), indent=1)

def _apply_erasers(items):
    """The C355 sprite buffer is single-layer, last-in-list wins.  A LATER sprite with a LOWER priority therefore
    erases an earlier sprite's pixels (the arcade uses it to cut holes), and any later SHADOW pixel replaces them too.
    Depth order alone would draw the earlier sprite on top, so remove those pixels from its texture.  (A later sprite
    of equal/higher priority needs nothing: it is nearer and hides the pixels anyway, and keeping them lets a moved
    head see round it.)"""
    spr = sorted((it for it in items if it.kind == 'sprite'), key=lambda i: -i.listidx)
    later_opaque = [np.zeros((H, W), bool) for _ in range(9)]       # by priority
    later_shadow = np.zeros((H, W), bool)
    for it in spr:
        # visible-screen window of this item's texture
        sx0, sy0 = max(it.x0, 0), max(it.y0, 0); sx1, sy1 = min(it.x0 + it.w, W), min(it.y0 + it.h, H)
        if sx1 <= sx0 or sy1 <= sy0: continue
        win = (slice(sy0 - it.y0, sy1 - it.y0), slice(sx0 - it.x0, sx1 - it.x0))
        scr = (slice(sy0, sy1), slice(sx0, sx1))
        pen = it.pen[win]
        opaque = pen >= 0; shadow = pen == -2
        erase = later_shadow[scr].copy()
        for p in range(0, it.pri):
            erase |= later_opaque[p][scr]
        if erase.any():
            sub = it.pen[win]; sub[erase] = -1
            it.pen[win] = sub
            t = it.tex[win]; t[erase] = 0; it.tex[win] = t
            it.erased = int(erase.sum())
        later_opaque[it.pri][scr] |= opaque
        later_shadow[scr] |= shadow

def build_items(st, st_sprites, roms, table=DEFAULT_TABLE, apply_erase=True):
    """st = tilemap/palette state at frame T; st_sprites = sprite-side state (2 frames earlier, see FINDINGS)."""
    rgb = R.palette_rgb(st.pal)
    items = []
    order = 0
    # arcade composite order: for pri 0..7 -> tilemaps (layer order) then sprites (list order)
    ents = R.sprite_entries(st_sprites)
    textures = {}
    for p in range(8):
        for i in range(6):
            c = int(st.ctl[0x10 + i])
            if (c & 7) == p and not (c & 8):
                tex, pen = R.render_tilemap_rgba(st, roms, i, rgb)
                if tex[..., 3].any():
                    items.append(Item(kind='tilemap', id=i, pri=p, order=order, x0=0, y0=0, w=W, h=H, tex=tex,
                                      key=None, pal=int(st.ctl[0x18 + i]) & 7, ncols=36, nrows=28, pen=pen, rgb=rgb))
                    order += 1
        for n, sp in enumerate(ents):
            if sp['pri'] != p: continue
            r = R.render_entry_rgba(st_sprites, roms, sp, rgb)
            if r is None: continue
            x0, y0, rgba, pen = r
            key = next((((t & 0x7ff) | (int(st_sprites.bank[t >> 11]) << 11)) + sp['offset']
                        for (t, *_rest) in sp['tiles']), None)
            items.append(Item(kind='sprite', id=sp['which'], pri=p, order=order, x0=x0, y0=y0, w=rgba.shape[1],
                              h=rgba.shape[0], tex=rgba, key=key, pal=sp['color'], ncols=sp['ncols'], nrows=sp['nrows'],
                              pen=pen, rgb=rgb, shadow=bool((pen == -2).any()), listidx=n,
                              zoom=sp['hsize'] / (sp['ncols'] * 16.0)))
            order += 1
    if apply_erase:
        _apply_erasers(items)
    items = apply_edits(items, table, os.path.dirname(os.path.abspath(table.get('_path', '.'))) if table.get('_path') else '.')
    # C116 blanking window (regs 0-3): everything outside it is black. The arcade uses it for iris/curtain transitions.
    # Modelled as one flat opaque-black quad with a hole, in front of the HUD plane, so it works from any eye position.
    cr = [(int(st.pal[0x1800 + 2 * k]) << 8) | int(st.pal[0x1801 + 2 * k]) for k in range(4)]
    wx0, wx1 = max(R.s(cr[0], 16) - 0x4a, 0), min(R.s(cr[1], 16) - 0x4a - 1, W - 1)
    wy0, wy1 = max(R.s(cr[2], 16) - 0x21, 0), min(R.s(cr[3], 16) - 0x21 - 1, H - 1)
    items = apply_decals(assign_depths(items, table), table)
    if (wx0, wy0, wx1, wy1) != (0, 0, W - 1, H - 1):
        pen = np.full((H, W), -3, np.int32)
        pen[wy0:wy1 + 1, wx0:wx1 + 1] = -1
        tex = np.zeros((H, W, 4), np.uint8); tex[pen == -3, 3] = 255
        d0 = (table.get('defaults') or DEFAULT_TABLE['defaults'])
        items.append(Item(kind='mask', id=-1, pri=8, order=10 ** 6, x0=0, y0=0, w=W, h=H, tex=tex, key=None, pal=0,
                          ncols=36, nrows=28, pen=pen, rgb=rgb, z=min(it.z for it in items) - 0.01, hud=True))
    return items

# ------------------------------------------------------------------ rendering
FAR = 80.0     # projected planes are cut off beyond this many metres (the horizon of a floor is infinitely far)

def _hits(it, E, Dw):
    """Ray/item intersection for rays Dw (...,3) from E. Returns t (distance along the ray parameter), texel x/y, ok mask.
    Fronto/tilted quads: parametric (s,q) of the quad.  Projected planes: the hit point is projected back through the ARCADE eye
    to find its texel, so the arcade picture is reproduced exactly from the eye and is true geometry from anywhere else."""
    if it.plane is not None:
        n, dd = it.plane
        with np.errstate(divide='ignore', invalid='ignore'):
            t = (dd - E @ n) / (Dw @ n)
        P = E + Dw * t[..., None]
        zc = -P[..., 2]
        with np.errstate(divide='ignore', invalid='ignore'):
            sx = CX + FOCAL * P[..., 0] / zc; sy = CY - FOCAL * P[..., 1] / zc
        tx = np.floor(sx - it.x0).astype(int); ty = np.floor(sy - it.y0).astype(int)
        ok = (t > 0) & (zc > 0.05) & (zc <= FAR) & (tx >= 0) & (tx < it.w) & (ty >= 0) & (ty < it.h)
    else:
        ctr, r, u = it.corners(); n = np.cross(r, u)
        with np.errstate(divide='ignore', invalid='ignore'):
            t = ((ctr - E) @ n) / (Dw @ n)
        P = E + Dw * t[..., None]; rel = P - ctr
        s = (rel @ r) / (r @ r); q = (rel @ u) / (u @ u)
        tx = np.floor((s + 1) / 2 * it.w).astype(int); ty = np.floor((1 - (q + 1) / 2) * it.h).astype(int)
        ok = (t > 0) & (tx >= 0) & (tx < it.w) & (ty >= 0) & (ty < it.h)
    return t, np.clip(tx, 0, it.w - 1), np.clip(ty, 0, it.h - 1), ok

def _plane_corners(it):
    """World corners of a projected plane's box (eye rays through the box corners), or None if a corner misses the plane."""
    n, dd = it.plane; out = []
    for (x, y) in ((it.x0, it.y0), (it.x0 + it.w, it.y0), (it.x0 + it.w, it.y0 + it.h), (it.x0, it.y0 + it.h)):
        ray = unproject(x, y, 1.0); den = float(ray @ n)
        if abs(den) < 1e-9 or dd / den <= 0: return None
        P = ray * (dd / den)
        if -P[2] > FAR: return None
        out.append(P)
    return out

def render_cam(items, E, fwd, right, up, f, size, ids=None):
    """Shared rasteriser: free camera at E, z-buffered, palette-index compositing (shadow = pen | 0x800, RGB art halves).
    ids: optional int32 (h, w) array filled with the index (in `items`) of the item that owns each pixel, -1 = none/blanked."""
    index = {id(it): n for n, it in enumerate(items)} if ids is not None else None
    ow, oh = size; cx, cy = ow / 2.0, oh / 2.0
    E = np.asarray(E, float)
    pen = np.full((oh, ow), -1, np.int32); col = np.zeros((oh, ow, 3), np.uint8)
    zb = np.full((oh, ow), np.inf)
    rgb = items[0].rgb
    def draw(it, shadow_pass):
        if it.plane is not None:
            cs = _plane_corners(it)            # None => horizon/behind: fall back to the whole frame
        else:
            ctr, r, u = it.corners()
            cs = [ctr + sx * r + sy * u for sx in (-1, 1) for sy in (-1, 1)]
        if cs is None:
            x0, x1, y0, y1 = 0, ow, 0, oh
        else:
            rel = [c - E for c in cs]; zc = [rr @ fwd for rr in rel]
            if min(zc) <= 0.05:
                x0, x1, y0, y1 = 0, ow, 0, oh
            else:
                us = [cx + f * (rr @ right) / z for rr, z in zip(rel, zc)]; vs = [cy - f * (rr @ up) / z for rr, z in zip(rel, zc)]
                x0, x1 = max(int(math.floor(min(us))), 0), min(int(math.ceil(max(us))), ow)
                y0, y1 = max(int(math.floor(min(vs))), 0), min(int(math.ceil(max(vs))), oh)
        if x1 <= x0 or y1 <= y0: return
        uu, vv = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
        Dw = ((uu - cx) / f)[..., None] * right + ((cy - vv) / f)[..., None] * up + fwd
        t, tx, ty, ok = _hits(it, E, Dw)
        p = it.pen[ty, tx]
        rp = pen[y0:y1, x0:x1]; rc = col[y0:y1, x0:x1]; rz = zb[y0:y1, x0:x1]
        if shadow_pass:
            sh = ok & (p == -2) & (t < rz)                 # darkens only what is BEHIND the shadow item
            shi = sh & (rp >= 0)
            rp[shi] = rp[shi] | 0x800; rc[shi] = rgb[rp[shi] & 0x1fff]
            rc[sh & (rp == -4)] //= 2
            return
        front = ok & (t < rz + 1e-9)                       # ties (coplanar art) go to the item the arcade draws later
        idx = front & (p >= 0)
        rp[idx] = p[idx]; rc[idx] = rgb[p[idx] & 0x1fff]; rz[idx] = t[idx]
        if ids is not None and not it.decal: ids[y0:y1, x0:x1][idx] = index[id(it)]
        if it.cust is not None:
            cm = front & (p == -4)
            rp[cm] = -4; rc[cm] = it.cust[ty, tx][cm]; rz[cm] = t[cm]
            if ids is not None and not it.decal: ids[y0:y1, x0:x1][cm] = index[id(it)]
        blk = front & (p == -3)
        rp[blk] = -1; rc[blk] = 0; rz[blk] = t[blk]
        if ids is not None: ids[y0:y1, x0:x1][blk] = -1
    ordered = sorted(items, key=lambda i: i.order)
    for it in ordered: draw(it, False)
    for it in ordered:
        if it.shadow: draw(it, True)
    return col

def render_view(items, eye=(0, 0, 0), scale=2, out_w=None, out_h=None):
    """View from `eye` with the arcade's orientation (looking down -z); at eye=(0,0,0) this is the arcade picture."""
    ow, oh = int((out_w or W) * scale), int((out_h or H) * scale)
    return render_cam(items, eye, np.array([0, 0, -1.0]), np.array([1.0, 0, 0]), np.array([0, 1.0, 0]), FOCAL * scale, (ow, oh))

def orbit_camera(target, distance, yaw, pitch):
    """Camera orbiting `target` at yaw/pitch degrees (yaw 0 = the arcade eye's direction): returns E, forward, right, up."""
    T = np.array(target, float)
    ya, pa = math.radians(yaw), math.radians(pitch)
    E = T + distance * np.array([math.sin(ya) * math.cos(pa), math.sin(pa), math.cos(ya) * math.cos(pa)])
    fwd = (T - E); fwd /= np.linalg.norm(fwd)
    right = np.cross(fwd, [0, 1, 0]); right /= np.linalg.norm(right)
    return E, fwd, right, np.cross(right, fwd)

def render_orbit(items, target=(0, 0, -8.0), distance=14.0, yaw=45.0, pitch=10.0, size=(640, 480), f=420.0,
                 outline=True, hide_hud=False, labels=True):
    """Free camera orbiting `target` (world coordinates; scene depth z=d lives at world -d) at yaw/pitch degrees from the
    arcade axis (yaw 0 = the arcade eye's direction, +yaw = camera moves to the right), plus quad outlines coloured by depth
    (red near, cyan far) with "z=.. m" labels, to see how the billboards and planes are layered."""
    from PIL import Image, ImageDraw
    ow, oh = size; cx, cy = ow / 2.0, oh / 2.0
    E, fwd, right, up = orbit_camera(target, distance, yaw, pitch)
    vis = [it for it in items if not (hide_hud and it.hud) and it.kind != 'mask']
    col = render_cam(vis, E, fwd, right, up, f, size)
    # background: dark (render_cam leaves unwritten pixels black); lift it so black art stays distinguishable
    img = Image.fromarray(col)
    if outline:
        dr = ImageDraw.Draw(img)
        def proj(p):
            rel = p - E; zc = rel @ fwd
            return (cx + f * (rel @ right) / zc, cy - f * (rel @ up) / zc, zc)
        zs = [it.z for it in vis]; zmin, zmax = min(zs), max(zs)
        for it in sorted(vis, key=lambda i: -i.z):
            if it.plane is not None:
                c4 = _plane_corners(it)
                if c4 is None: continue
            else:
                ctr, r, u = it.corners(); c4 = [ctr - r + u, ctr + r + u, ctr + r - u, ctr - r - u]
            pj = [proj(c) for c in c4]
            if min(p[2] for p in pj) <= 0.05: continue
            k = (it.z - zmin) / max(zmax - zmin, 1e-6)
            colr = (int(255 * (1 - k)), int(80 + 150 * k), int(255 * k))
            dr.line([(p[0], p[1]) for p in pj] + [(pj[0][0], pj[0][1])], fill=colr, width=1)
            if labels and (it.w * it.h > 600 or it.kind == 'tilemap'):
                nm = f"{it.kind[:2]}{it.id}" + (f":{it.part}" if it.part else "") + ("[plane]" if it.plane is not None else "")
                dr.text((pj[0][0] + 3, pj[0][1] + 2), f"{nm} z={it.z:.1f}", fill=colr)
    return np.array(img)

# ------------------------------------------------------------------ aiming
def ray_item(it, O, Dv):
    """Intersect one ray with an item; return (t, tex x, tex y) or None. Texel alpha is tested by the caller."""
    t, tx, ty, ok = _hits(it, np.asarray(O, float), np.asarray(Dv, float).reshape(1, 3))
    if not ok[0]: return None
    return float(t[0]), int(tx[0]), int(ty[0])

def aim(items, O, Dv, fallback_z=None, fallback=True, skip_hud=False):
    """Nearest opaque texel along the ray; returns dict(item, hit, sx, sy) with (sx, sy) the arcade pixel the
    hit point occupies as seen from the arcade eye.  Falls back to a plane at the arcade screen distance."""
    O = np.asarray(O, float); Dv = np.asarray(Dv, float)
    best = None
    for it in items:
        if it.kind == 'mask' or it.decal or (skip_hud and it.hud): continue
        h = ray_item(it, O, Dv)
        if h is None: continue
        t, tx, ty = h
        if it.tex[ty, tx, 3] == 0: continue
        if best is None or t < best[0]: best = (t, it)
    if best is not None:
        P = O + Dv * best[0]; item = best[1]
    else:
        if not fallback: return None
        zf = fallback_z or 2.5
        if Dv[2] >= -1e-6: return None
        t = (-zf - O[2]) / Dv[2]
        if t < 0: return None
        P = O + Dv * t; item = None
    if P[2] >= -1e-3: return None
    sx, sy = project_arcade(P)
    return dict(item=item, hit=P, sx=sx, sy=sy, index=None if item is None else next(n for n, i in enumerate(items) if i is item))

def visible_map(items):
    """Which item the ARCADE shows at each arcade pixel (index into `items`, -1 = nothing/blanked): the arcade-eye view with an id buffer."""
    ids = np.full((H, W), -1, np.int32)
    render_cam(items, (0, 0, 0), np.array([0, 0, -1.0]), np.array([1.0, 0, 0]), np.array([0, 1.0, 0]), FOCAL, (W, H), ids=ids)
    return ids

def aim_snap(items, O, Dv, ids, radius=24.0, skip_hud=False):
    """aim() that lands on a pixel where the hit item is actually visible to the arcade eye (the game only sees 2D pixels).
    No hit -> None (caller keeps its last pixel; there is no fallback plane).  Result adds raw_sx/raw_sy, snapped, hidden
    (the item is not visible within `radius` px, the raw pixel is kept) and cover (index of the item that covers the raw pixel)."""
    a = aim(items, O, Dv, fallback=False, skip_hud=skip_hud)
    if a is None: return None
    a['raw_sx'], a['raw_sy'] = a['sx'], a['sy']; a['snapped'] = False; a['hidden'] = False
    x, y = int(math.floor(a['sx'])), int(math.floor(a['sy']))
    inside = 0 <= x < W and 0 <= y < H
    a['cover'] = int(ids[y, x]) if inside else -1
    if inside and a['cover'] == a['index']: return a
    ys, xs = np.nonzero(ids == a['index'])
    if len(xs) == 0: a['hidden'] = True; return a
    d2 = (xs + .5 - a['sx']) ** 2 + (ys + .5 - a['sy']) ** 2
    k = int(np.argmin(d2))
    if d2[k] > radius * radius: a['hidden'] = True; return a
    a['sx'], a['sy'] = float(xs[k] + .5), float(ys[k] + .5); a['snapped'] = True
    return a

# ------------------------------------------------------------------ gun registers
CAL_X, CAL_Y = 0x26, 0x0f          # EEPROM defaults, read back from the running game (0x218000+0x3a47 / +0x3a46)
def gun_regs(sx, sy, calx=CAL_X, caly=CAL_Y):
    """Registers a perfect gun would return for arcade pixel (sx, sy):
    X = ((Xraw-calX)*0x4ecf)>>14 clamped to 0x11f ; Y = ((Y1+Y2)>>1)-calY ; shot ignored if Y1 <= 0x0f."""
    sx = min(max(int(round(sx)), 0), 287); sy = min(max(int(round(sy)), 1), 223)
    xraw = calx + -(-sx * 16384 // 0x4ecf)            # ceil
    # smallest raw giving >= sx, then step down if that overshoots
    while ((xraw - 1 - calx) * 0x4ecf) >> 14 >= sx and xraw - 1 >= calx: xraw -= 1
    yraw = caly + sy
    return dict(x1=xraw, x2=xraw, y1=yraw, y2=yraw)

def regs_to_pixel(r, calx=CAL_X, caly=CAL_Y):
    """What the game does with the registers (disassembly of 0xa392..0xa440), for testing."""
    if r['y1'] <= 0x0f: return None
    x1 = r['x1'] + (0x100 if r['x1'] < 0x26 else 0); x2 = r['x2'] + (0x100 if r['x2'] < 0x26 else 0)
    d1 = x2
    if x2 > x1 and (x2 - x1) >= 8: d1 = x1
    X = 0 if d1 < calx else min(((d1 - calx) * 0x4ecf) >> 14, 0x11f)
    Y = max(((r['y1'] + r['y2']) >> 1) - caly, 0)
    return X, Y
