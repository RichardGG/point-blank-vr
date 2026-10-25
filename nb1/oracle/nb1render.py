#!/usr/bin/env python3
"""Offline NB-1 video renderer (C355 sprites + C123 tilemaps + C116 palette) for Point Blank.

Reimplements the draw path of MAME 0.264 (namco_c355spr.cpp / namco_c123tmap.cpp / namconb1_v.cpp)
from a raw RAM snapshot so the picture can be rebuilt as *separate layers*:

    for pri in 0..7:   tilemaps whose priority nibble == pri  (layer order 0..5)
                       then sprite pixels whose sprite priority == pri

render(state, split=True) returns the ordered list of layers [(kind, id, pri, rgba)], and the
exact composite. Nothing here is game content; ROMs are read from a directory the caller supplies.
"""
import numpy as np, struct, os

W, H = 288, 224

class Roms:
    def __init__(self, d):
        if os.path.isfile(d) and d.lower().endswith('.zip'):      # MAME rom zip (ptblank.zip) works as well as an extracted folder
            import zipfile
            zf = zipfile.ZipFile(d); rd = lambda n: zf.read(n)
        else:
            rd = lambda n: open(os.path.join(d, n), 'rb').read()
        # c355spr region: ROM_LOAD32_WORD, 16-bit word i of each file lands at offset+4*i
        spr = np.zeros(0x800000, np.uint8)
        for fn, off in (('gn1obj0l.ic1', 0), ('gn1obj0u.ic2', 2), ('gn1obj1l.ic3', 0x400000), ('gn1obj1u.ic4', 0x400002)):
            a = np.frombuffer(rd(fn), np.uint8).reshape(-1, 2)
            idx = off + 4 * np.arange(len(a))
            spr[idx] = a[:, 0]; spr[idx + 1] = a[:, 1]
        self.spr = spr.reshape(-1, 16, 16)                      # [tile][y][x], 8 bpp
        chr_ = b''.join(rd(f) for f in ('gn1-chr0.8j', 'gn1-chr1.9j', 'gn1-chr2.10j', 'gn1-chr3.11j'))
        self.chr = np.frombuffer(chr_, np.uint8).reshape(-1, 8, 8)   # [tile][y][x], 8 bpp
        self.mask = np.frombuffer(rd('gn1-sha0.5m'), np.uint8).reshape(-1, 8)  # 8 bytes per tile, 1 bpp

class State:
    """Raw RAM as dumped by lua/snap.lua."""
    def __init__(self, path=None, data=None):
        b = data if data is not None else open(path, 'rb').read()
        o = 0
        def take(n):
            nonlocal o
            r = b[o:o + n]; o += n; return r
        self.spriteram = np.frombuffer(take(0x20000), '>u2')
        self.vram = np.frombuffer(take(0x10000), '>u2')
        self.ctl = np.frombuffer(take(0x40), '>u2')
        self.bank = np.frombuffer(take(0x10), '>u2')
        self.pos = np.frombuffer(take(8), '>u2')
        self.pal = np.frombuffer(take(0x8000), np.uint8)

def palette_rgb(pal):
    """C116: four 0x2000 blocks, each R[0x800] G[0x800] B[0x800] (+regs)."""
    out = np.zeros((0x2000, 3), np.uint8)
    for blk in range(4):
        base = blk * 0x2000
        for c in range(3):
            out[blk * 0x800:(blk + 1) * 0x800, c] = pal[base + c * 0x800: base + (c + 1) * 0x800]
    return out

def s(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v

# ------------------------------------------------------------------ tilemaps
TM_OFF = [0x0000, 0x1000, 0x2000, 0x3000, 0x4008, 0x4408]
def render_tilemap(st, roms, i, mask_bit_set_opaque=True, msb_first=True):
    """Returns (pen[H][W] int32, opaque[H][W] bool) for tilemap i, in screen space."""
    ctl = st.ctl
    flip = bool(ctl[1] & 0x8000)
    if i < 4:
        sx = ctl[4 * i + 1] & 0x1ff
        sy = ctl[4 * i + 3] & 0x1ff
        if flip: sx, sy = -sx, -sy
        adj = (4, 2, 1, 0)[i]
        dx, dy = 44 + adj, 24
        cols = rows = 64
        # tilemap_t: effective scroll = dx_const - value  with  dx_const = -(44+adj)
        ox = (sx + dx) if not flip else (-(sx) + 288 + dx)
        oy = (sy + dy) if not flip else (-(sy) + 224 + 24)
    else:
        cols, rows = 36, 28
        ox = oy = 0
    pal_off = ((int(ctl[0x30 // 2 + i]) & 7) << 8) + 0x1000
    vr = st.vram[TM_OFF[i]:TM_OFF[i] + cols * rows].reshape(rows, cols)
    # build the full tilemap bitmap
    tiles = roms.chr[vr]                        # rows, cols, 8, 8
    mk = roms.mask[vr]                          # rows, cols, 8 (one byte per tile row)
    bits = np.unpackbits(mk, axis=-1, bitorder='big' if msb_first else 'little').reshape(rows, cols, 8, 8)
    opq = bits.astype(bool) if mask_bit_set_opaque else ~bits.astype(bool)
    pen = tiles.transpose(0, 2, 1, 3).reshape(rows * 8, cols * 8).astype(np.int32)
    opq = opq.transpose(0, 2, 1, 3).reshape(rows * 8, cols * 8)
    th, tw = rows * 8, cols * 8
    ys = (np.arange(H) + oy) % th
    xs = (np.arange(W) + ox) % tw
    if flip and i < 4:
        ys = (th - 1 - ((np.arange(H) + oy) % th)); xs = (tw - 1 - ((np.arange(W) + ox) % tw))
    pen = pen[np.ix_(ys, xs)] + pal_off
    opq = opq[np.ix_(ys, xs)]
    return pen, opq

# ------------------------------------------------------------------ sprites
def sprite_entries(st):
    """Decode list 1 (the only list the NB-1 driver draws) like get_single_sprite()."""
    ram = st.spriteram
    tbl, lst = 0x10000 // 2, 0x14000 // 2
    xscroll = s(int(st.pos[1]) & 0x1ff, 9) + 0x26
    yscroll = s(int(st.pos[0]) & 0x1ff, 9) + 0x19
    out = []
    for i in range(256):
        w = int(ram[lst + i]); which = w & 0xff
        e = tbl + which * 8
        palette = int(ram[e + 6]); pri = (palette >> 4) & 0xf
        link = int(ram[e + 0]) & 0x7ff; offset = int(ram[e + 1])
        hpos = int(ram[e + 2]) - xscroll; vpos = int(ram[e + 3]) - yscroll
        hsize = int(ram[e + 4]); vsize = int(ram[e + 5])
        clipi = (palette >> 8) & 0xf
        cl = [int(ram[0x2400 // 2 + clipi * 4 + k]) for k in range(4)]
        clip = (s(cl[0] - xscroll, 16), s(cl[1] - xscroll, 16), s(cl[2] - yscroll, 16), s(cl[3] - yscroll, 16))
        hpos = s(hpos, 11); vpos = s(vpos, 11)
        fe = 0x4000 // 2 + link * 4
        tindex = int(ram[fe]); fmt = int(ram[fe + 1]); dx = int(ram[fe + 2]) & 0x1ff; dy = int(ram[fe + 3]) & 0x1ff
        ncols = (fmt >> 4) & 0xf or 16; nrows = fmt & 0xf or 16
        flipx = bool(hsize & 0x8000); hsize &= 0x3ff
        flipy = bool(vsize & 0x8000); vsize &= 0x3ff
        ok = hsize and vsize
        if ok:
            zx = (hsize << 16) // (ncols * 16)
            dxz = (((dx & 0xff) * zx + 0x8000) >> 16)
            if dx & 0x100: dxz = -dxz
            hpos += dxz if flipx else -dxz
            zy = (vsize << 16) // (nrows * 16)
            dyz = (((dy & 0xff) * zy + 0x8000) >> 16)
            if dy & 0x100: dyz = -dyz
            vpos += dyz if flipy else -dyz
            # per-tile placement exactly as the device loops it
            tiles = []
            src_h, scr_h, y = nrows * 16, vsize, vpos
            for row in range(nrows):
                tsh = 16 * scr_h // src_h
                zoomy = (scr_h << 16) // src_h
                if flipy: y -= tsh
                src_w, scr_w, x = ncols * 16, hsize, hpos
                for col in range(ncols):
                    tsw = 16 * scr_w // src_w
                    zoomx = (scr_w << 16) // src_w
                    if flipx: x -= tsw
                    t = int(ram[0x8000 // 2 + tindex]); tindex += 1
                    if not (t & 0x8000):
                        tiles.append((t, x, y, (zoomx * 16 + 0x8000) >> 16, (zoomy * 16 + 0x8000) >> 16))
                    if not flipx: x += tsw
                    scr_w -= tsw; src_w -= 16
                if not flipy: y += tsh
                scr_h -= tsh; src_h -= 16
        else:
            tiles = []
        out.append(dict(which=which, pri=pri, color=palette & 0xf, offset=offset, flipx=flipx, flipy=flipy,
                        clip=clip, tiles=tiles, ncols=ncols, nrows=nrows, hsize=hsize, vsize=vsize,
                        x=hpos, y=vpos, palette=palette, link=link))
        if w & 0x100: break
    return out

def render_sprites(st, roms, entries=None):
    """Draw every tile in list order into one buffer (last wins, as render_sprites does).
    Returns pen[H][W] (0xffff = empty), pri[H][W], idx[H][W] (index of winning list entry)."""
    entries = sprite_entries(st) if entries is None else entries
    pen = np.full((H, W), 0xffff, np.int32); pri = np.zeros((H, W), np.int8); who = np.full((H, W), -1, np.int32)
    bank = [int(v) for v in st.bank]
    for n, sp in enumerate(entries):
        cx0, cx1, cy0, cy1 = sp['clip']
        cx0, cy0 = max(cx0, 0), max(cy0, 0); cx1, cy1 = min(cx1, W - 1), min(cy1, H - 1)
        base = sp['color'] * 256
        for (t, x, y, w, h) in sp['tiles']:
            if w <= 0 or h <= 0: continue
            code = ((t & 0x7ff) | (bank[t >> 11] << 11)) + sp['offset']
            src = roms.spr[code % len(roms.spr)]
            dx = (16 << 16) // w; dy = (16 << 16) // h
            xi = (np.arange(w) * dx) >> 16; yi = (np.arange(h) * dy) >> 16
            if sp['flipx']: xi = ((w - 1 - np.arange(w)) * dx) >> 16
            if sp['flipy']: yi = ((h - 1 - np.arange(h)) * dy) >> 16
            tile = src[np.ix_(yi, xi)]
            x0, y0 = max(x, cx0), max(y, cy0); x1, y1 = min(x + w, cx1 + 1), min(y + h, cy1 + 1)
            if x1 <= x0 or y1 <= y0: continue
            sub = tile[y0 - y:y1 - y, x0 - x:x1 - x]
            vis = sub != 0xff
            region = (slice(y0, y1), slice(x0, x1))
            pen[region] = np.where(vis, ((base + sub.astype(np.int32)) & 0xfff), pen[region])
            pri[region] = np.where(vis, sp['pri'], pri[region])
            who[region] = np.where(vis, n, who[region])
    return pen, pri, who, entries

# ------------------------------------------------------------------ compose
def render(st, roms, split=False, sst=None, **kw):
    """st = tilemap/palette state; sst = sprite-side state (spriteram/bank/pos), default st."""
    sst = sst or st
    rgb = palette_rgb(st.pal)
    clipreg = [(int(st.pal[0x1800 + 2 * k]) << 8) | int(st.pal[0x1801 + 2 * k]) for k in range(6)]
    cx0, cx1 = max(s(clipreg[0], 16) - 0x4a, 0), min(s(clipreg[1], 16) - 0x4a - 1, W - 1)
    cy0, cy1 = max(s(clipreg[2], 16) - 0x21, 0), min(s(clipreg[3], 16) - 0x21 - 1, H - 1)
    tmaps = [render_tilemap(st, roms, i, **kw) for i in range(6)]
    spen, spri, swho, ents = render_sprites(sst, roms)
    dest = np.full((H, W), -1, np.int32)         # -1 = black fill
    layers = []
    for p in range(8):
        for i in range(6):
            c = int(st.ctl[0x10 + i])
            if (c & 7) == p and not (c & 8):
                pen, opq = tmaps[i]
                dest = np.where(opq, pen, dest)
                if split: layers.append(('tilemap', i, p, pen, opq.copy(), None))
        m = (spen != 0xffff) & (spri == p)
        # 0xffe is the "shadow" pen: sets 0x800 on what is underneath
        shadow = m & (spen == 0xffe)
        dest = np.where(m & ~shadow, spen, dest)
        dest = np.where(shadow & (dest >= 0), dest | 0x800, dest)
        if split: layers.append(('sprites', -1, p, spen.copy(), m.copy(), swho.copy()))
    img = np.zeros((H, W, 3), np.uint8)
    ok = dest >= 0
    img[ok] = rgb[dest[ok] & 0x1fff]
    clipm = np.zeros((H, W), bool); clipm[cy0:cy1 + 1, cx0:cx1 + 1] = True
    img[~clipm] = 0
    if split:
        return img, layers, rgb, ents
    return img

if __name__ == '__main__':
    import sys
    from PIL import Image
    roms = Roms(sys.argv[1]); st = State(sys.argv[2])
    Image.fromarray(render(st, roms)).save(sys.argv[3])


# ------------------------------------------------------------------ per-object textures
def render_entry_rgba(st, roms, sp, rgb, apply_clip=True):
    """Render ONE sprite entry on its own (all its pixels, no occlusion by other sprites, not clipped to the
    screen).  Returns (x0, y0, rgba[h][w][4], pen[h][w]) or None if the entry draws nothing.
    pen: palette index per texel, -1 = transparent, -2 = SHADOW (C355 pen 0xffe: sets bit 0x800 of whatever pen is
    underneath -> a pre-darkened palette entry; the arcade's fades and dimming).  Shadow texels have alpha 0."""
    if not sp['tiles']:
        return None
    bank = [int(v) for v in st.bank]
    xs0 = min(t[1] for t in sp['tiles']); ys0 = min(t[2] for t in sp['tiles'])
    xs1 = max(t[1] + t[3] for t in sp['tiles']); ys1 = max(t[2] + t[4] for t in sp['tiles'])
    w, h = xs1 - xs0, ys1 - ys0
    if w <= 0 or h <= 0 or w > 8192 or h > 8192:
        return None
    pen = np.full((h, w), -1, np.int32)
    base = sp['color'] * 256
    for (t, x, y, tw, th) in sp['tiles']:
        if tw <= 0 or th <= 0: continue
        code = ((t & 0x7ff) | (bank[t >> 11] << 11)) + sp['offset']
        src = roms.spr[code % len(roms.spr)]
        dx = (16 << 16) // tw; dy = (16 << 16) // th
        xi = (np.arange(tw) * dx) >> 16; yi = (np.arange(th) * dy) >> 16
        if sp['flipx']: xi = ((tw - 1 - np.arange(tw)) * dx) >> 16
        if sp['flipy']: yi = ((th - 1 - np.arange(th)) * dy) >> 16
        tile = src[np.ix_(yi, xi)]
        reg = (slice(y - ys0, y - ys0 + th), slice(x - xs0, x - xs0 + tw))
        pen[reg] = np.where(tile != 0xff, (base + tile.astype(np.int32)) & 0xfff, pen[reg])
    ok = pen >= 0
    if apply_clip:     # C355 clip window (palette bits 8..11): how the arcade pops targets up from behind cover
        cx0, cx1, cy0, cy1 = sp['clip']
        yy, xx = np.mgrid[ys0:ys1, xs0:xs1]
        inside = (xx >= cx0) & (xx <= cx1) & (yy >= cy0) & (yy <= cy1)
        pen[~inside] = -1; ok &= inside
    shadow = ok & (pen == 0xffe)
    pen[shadow] = -2
    solid = ok & ~shadow
    rgba = np.zeros((h, w, 4), np.uint8)
    rgba[solid, :3] = rgb[pen[solid]]; rgba[solid, 3] = 255
    return xs0, ys0, rgba, pen

def render_tilemap_rgba(st, roms, i, rgb):
    pen, opq = render_tilemap(st, roms, i)
    rgba = np.zeros((H, W, 4), np.uint8)
    rgba[opq, :3] = rgb[pen[opq] & 0x1fff]; rgba[opq, 3] = 255
    p = np.where(opq, pen & 0x1fff, -1).astype(np.int32)
    return rgba, p
