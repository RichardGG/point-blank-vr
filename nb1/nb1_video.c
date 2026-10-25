/* NB-1 video: C355 sprites, C123 tilemaps, C116 palette. Port of oracle/nb1render.py (itself checked pixel-exact
 * against MAME 0.264). Keep operation order identical to the oracle. */
#include "nb1.h"
#include <strings.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sext(int v, int bits) { v &= (1 << bits) - 1; return (v & (1 << (bits - 1))) ? v - (1 << bits) : v; }
static long fdiv(long a, long b) { long q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
static int pmod(int a, int m) { int r = a % m; return r < 0 ? r + m : r; }

/* ---------------------------------------------------------------- ROMs / state */
/* ---- reading ROM files from a folder or a MAME zip (stored or deflate; no zip64) */
static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint8_t *zip_member(const uint8_t *z, size_t zn, const char *name, size_t *n) {
    if (zn < 22) return NULL;
    size_t e = zn - 22; int found = 0;
    for (;; e--) { if (le32(z + e) == 0x06054b50) { found = 1; break; } if (e == 0 || zn - e > 22 + 65535) break; }
    if (!found) return NULL;
    unsigned count = le16(z + e + 10); size_t p = le32(z + e + 16);
    for (unsigned i = 0; i < count && p + 46 <= zn; i++) {
        if (le32(z + p) != 0x02014b50) return NULL;
        unsigned method = le16(z + p + 10), nl = le16(z + p + 28), xl = le16(z + p + 30), cl = le16(z + p + 32); size_t csize = le32(z + p + 20), usize = le32(z + p + 24), lo = le32(z + p + 42);
        if (nl == strlen(name) && !strncmp((const char *)z + p + 46, name, nl)) {
            if (lo + 30 > zn || le32(z + lo) != 0x04034b50) return NULL;
            size_t ds = lo + 30 + le16(z + lo + 26) + le16(z + lo + 28); if (ds + csize > zn) return NULL;
            uint8_t *out = malloc(usize ? usize : 1);
            if (method == 0) { memcpy(out, z + ds, usize); }
            else if (method == 8) {
                z_stream zs; memset(&zs, 0, sizeof zs); if (inflateInit2(&zs, -15) != Z_OK) { free(out); return NULL; }
                zs.next_in = (Bytef *)(z + ds); zs.avail_in = (uInt)csize; zs.next_out = out; zs.avail_out = (uInt)usize;
                int r = inflate(&zs, Z_FINISH); inflateEnd(&zs); if (r != Z_STREAM_END) { free(out); return NULL; }
            } else { free(out); return NULL; }
            *n = usize; return out;
        }
        p += 46 + nl + xl + cl;
    }
    return NULL;
}
static uint8_t *g_zip; static size_t g_zip_n; static char g_zip_path[1024];
static uint8_t *slurp(const char *dir, const char *name, size_t *n) {
    size_t dl = strlen(dir);
    if (dl > 4 && !strcasecmp(dir + dl - 4, ".zip")) {
        if (!g_zip || strcmp(g_zip_path, dir)) {
            free(g_zip); g_zip = NULL; FILE *f = fopen(dir, "rb"); if (!f) { fprintf(stderr, "nb1: cannot open %s\n", dir); return NULL; }
            fseek(f, 0, SEEK_END); g_zip_n = (size_t)ftell(f); fseek(f, 0, SEEK_SET); g_zip = malloc(g_zip_n);
            if (fread(g_zip, 1, g_zip_n, f) != g_zip_n) { fclose(f); free(g_zip); g_zip = NULL; return NULL; }
            fclose(f); snprintf(g_zip_path, sizeof g_zip_path, "%s", dir);
        }
        uint8_t *b = zip_member(g_zip, g_zip_n, name, n); if (!b) fprintf(stderr, "nb1: %s not found in %s\n", name, dir);
        return b;
    }
    char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "nb1: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(len ? len : 1);
    if (fread(b, 1, len, f) != (size_t)len) { fclose(f); free(b); return NULL; }
    fclose(f); *n = len; return b;
}
int nb1_roms_load(Nb1Roms *r, const char *dir) {
    memset(r, 0, sizeof *r);
    static const char *sf[4] = { "gn1obj0l.ic1", "gn1obj0u.ic2", "gn1obj1l.ic3", "gn1obj1u.ic4" };
    static const size_t so[4] = { 0, 2, 0x400000, 0x400002 };
    uint8_t *spr = calloc(0x800000, 1);
    for (int k = 0; k < 4; k++) {
        size_t n; uint8_t *b = slurp(dir, sf[k], &n); if (!b) { free(spr); return -1; }
        for (size_t i = 0; i + 1 < n; i += 2) { size_t idx = so[k] + 4 * (i / 2); if (idx + 1 < 0x800000) { spr[idx] = b[i]; spr[idx + 1] = b[i + 1]; } }
        free(b);
    }
    r->spr = spr; r->nspr = 0x800000 / 256;
    static const char *cf[4] = { "gn1-chr0.8j", "gn1-chr1.9j", "gn1-chr2.10j", "gn1-chr3.11j" };
    uint8_t *chr = NULL; size_t cn = 0;
    for (int k = 0; k < 4; k++) {
        size_t n; uint8_t *b = slurp(dir, cf[k], &n); if (!b) return -1;
        chr = realloc(chr, cn + n); memcpy(chr + cn, b, n); cn += n; free(b);
    }
    r->chr = chr; r->nchr = cn / 64;
    size_t mn; r->mask = slurp(dir, "gn1-sha0.5m", &mn); if (!r->mask) return -1; r->nmask = mn / 8;
    return 0;
}
void nb1_roms_free(Nb1Roms *r) {
    free(g_zip); g_zip = NULL; g_zip_n = 0; g_zip_path[0] = 0;
     free(r->spr); free(r->chr); free(r->mask); memset(r, 0, sizeof *r); }

#define NB1_STATE_BYTES (0x20000 + 0x10000 + 0x40 + 0x10 + 8 + 0x8000)
Nb1State *nb1_state_from_bytes(const uint8_t *b, size_t len) {
    if (len < NB1_STATE_BYTES) return NULL;
    Nb1State *s = calloc(1, sizeof *s);
    uint16_t *dst[5] = { s->spriteram, s->vram, s->ctl, s->bank, s->pos };
    static const size_t cnt[5] = { 0x10000, 0x8000, 0x20, 8, 4 };
    for (int k = 0; k < 5; k++) { for (size_t i = 0; i < cnt[k]; i++) dst[k][i] = (uint16_t)(b[2 * i] << 8 | b[2 * i + 1]); b += cnt[k] * 2; }
    memcpy(s->pal, b, 0x8000);
    return s;
}
Nb1State *nb1_state_load(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    uint8_t *buf = malloc(NB1_STATE_BYTES); size_t n = fread(buf, 1, NB1_STATE_BYTES, f); fclose(f);
    Nb1State *s = nb1_state_from_bytes(buf, n); free(buf); return s;
}

void nb1_palette_rgb(const Nb1State *st, uint8_t rgb[0x2000][3]) {
    for (int blk = 0; blk < 4; blk++)
        for (int c = 0; c < 3; c++)
            for (int n = 0; n < 0x800; n++) rgb[blk * 0x800 + n][c] = st->pal[blk * 0x2000 + c * 0x800 + n];
}

/* ---------------------------------------------------------------- tilemaps */
static const int TM_OFF[6] = { 0x0000, 0x1000, 0x2000, 0x3000, 0x4008, 0x4408 };

/* pen (incl. palette offset) and opaque for one layer in screen space */
static void tilemap_screen(const Nb1State *st, const Nb1Roms *roms, int i, int32_t *pen, uint8_t *opq) {
    const uint16_t *ctl = st->ctl;
    int flip = !!(ctl[1] & 0x8000), cols, rows, ox, oy;
    if (i < 4) {
        int sx = ctl[4 * i + 1] & 0x1ff, sy = ctl[4 * i + 3] & 0x1ff;
        if (flip) { sx = -sx; sy = -sy; }
        int adj = (int[]){ 4, 2, 1, 0 }[i], dx = 44 + adj, dy = 24;
        cols = rows = 64;
        ox = !flip ? sx + dx : -sx + 288 + dx;
        oy = !flip ? sy + dy : -sy + 224 + 24;
    } else { cols = 36; rows = 28; ox = oy = 0; }
    int pal_off = ((ctl[0x18 + i] & 7) << 8) + 0x1000;
    const uint16_t *vr = st->vram + TM_OFF[i];
    int th = rows * 8, tw = cols * 8;
    for (int y = 0; y < NB1_H; y++) {
        int ys = pmod(y + oy, th); if (flip && i < 4) ys = th - 1 - ys;
        for (int x = 0; x < NB1_W; x++) {
            int xs = pmod(x + ox, tw); if (flip && i < 4) xs = tw - 1 - xs;
            int code = vr[(ys / 8) * cols + xs / 8], ty = ys % 8, tx = xs % 8;
            size_t c = (size_t)code % roms->nchr, m = (size_t)code % roms->nmask;
            pen[y * NB1_W + x] = roms->chr[c * 64 + ty * 8 + tx] + pal_off;
            opq[y * NB1_W + x] = (roms->mask[m * 8 + ty] >> (7 - tx)) & 1;
        }
    }
}

void nb1_tilemap_tex(const Nb1State *st, const Nb1Roms *roms, int i, uint8_t rgb[0x2000][3], uint8_t *rgba, int32_t *pen) {
    int32_t *p = malloc(NB1_W * NB1_H * 4); uint8_t *o = malloc(NB1_W * NB1_H);
    tilemap_screen(st, roms, i, p, o);
    for (int k = 0; k < NB1_W * NB1_H; k++) {
        if (o[k]) { int idx = p[k] & 0x1fff; rgba[k * 4] = rgb[idx][0]; rgba[k * 4 + 1] = rgb[idx][1]; rgba[k * 4 + 2] = rgb[idx][2]; rgba[k * 4 + 3] = 255; pen[k] = idx; }
        else { rgba[k * 4] = rgba[k * 4 + 1] = rgba[k * 4 + 2] = rgba[k * 4 + 3] = 0; pen[k] = -1; }
    }
    free(p); free(o);
}

/* the whole 64x64-tile map of a scrolling layer, and where the screen window sits in it */
int nb1_tilemap_map_tex(const Nb1State *st, const Nb1Roms *roms, int i, uint8_t rgb[0x2000][3], uint8_t *rgba, int *ox_out, int *oy_out, int *flip_out) {
    if (i < 0 || i > 3) return 0;
    const uint16_t *ctl = st->ctl; int flip = !!(ctl[1] & 0x8000);
    int sx = ctl[4 * i + 1] & 0x1ff, sy = ctl[4 * i + 3] & 0x1ff; if (flip) { sx = -sx; sy = -sy; }
    int adj = (int[]){ 4, 2, 1, 0 }[i], dx = 44 + adj, dy = 24;
    *ox_out = !flip ? sx + dx : -sx + 288 + dx; *oy_out = !flip ? sy + dy : -sy + 224 + 24; *flip_out = flip;
    int pal_off = ((ctl[0x18 + i] & 7) << 8) + 0x1000; const uint16_t *vr = st->vram + TM_OFF[i];
    for (int y = 0; y < 512; y++) for (int x = 0; x < 512; x++) {
        int code = vr[(y / 8) * 64 + x / 8], ty = y % 8, tx = x % 8; size_t c = (size_t)code % roms->nchr, m = (size_t)code % roms->nmask;
        uint8_t *q = rgba + ((size_t)y * 512 + x) * 4;
        if ((roms->mask[m * 8 + ty] >> (7 - tx)) & 1) { int idx = (roms->chr[c * 64 + ty * 8 + tx] + pal_off) & 0x1fff; q[0] = rgb[idx][0]; q[1] = rgb[idx][1]; q[2] = rgb[idx][2]; q[3] = 255; }
        else q[0] = q[1] = q[2] = q[3] = 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- sprites */
int nb1_sprite_entries(const Nb1State *st, Nb1Entry *out) {
    const uint16_t *ram = st->spriteram;
    const int tbl = 0x10000 / 2, lst = 0x14000 / 2;
    int xscroll = sext(st->pos[1] & 0x1ff, 9) + 0x26, yscroll = sext(st->pos[0] & 0x1ff, 9) + 0x19, n = 0;
    for (int i = 0; i < 256; i++) {
        int w = ram[lst + i], which = w & 0xff, e = tbl + which * 8;
        Nb1Entry *sp = &out[n++]; memset(sp, 0, sizeof *sp);
        int palette = ram[e + 6], pri = (palette >> 4) & 0xf, link = ram[e + 0] & 0x7ff, offset = ram[e + 1];
        int hpos = ram[e + 2] - xscroll, vpos = ram[e + 3] - yscroll, hsize = ram[e + 4], vsize = ram[e + 5];
        int clipi = (palette >> 8) & 0xf, cl[4];
        for (int k = 0; k < 4; k++) cl[k] = ram[0x2400 / 2 + clipi * 4 + k];
        sp->clip[0] = sext(cl[0] - xscroll, 16); sp->clip[1] = sext(cl[1] - xscroll, 16);
        sp->clip[2] = sext(cl[2] - yscroll, 16); sp->clip[3] = sext(cl[3] - yscroll, 16);
        hpos = sext(hpos, 11); vpos = sext(vpos, 11);
        int fe = 0x4000 / 2 + link * 4, tindex = ram[fe], fmt = ram[fe + 1], dx = ram[fe + 2] & 0x1ff, dy = ram[fe + 3] & 0x1ff;
        int ncols = (fmt >> 4) & 0xf; if (!ncols) ncols = 16;
        int nrows = fmt & 0xf; if (!nrows) nrows = 16;
        int flipx = !!(hsize & 0x8000); hsize &= 0x3ff;
        int flipy = !!(vsize & 0x8000); vsize &= 0x3ff;
        if (hsize && vsize) {
            long zx = ((long)hsize << 16) / (ncols * 16);
            long dxz = (((dx & 0xff) * zx + 0x8000) >> 16); if (dx & 0x100) dxz = -dxz;
            hpos += flipx ? dxz : -dxz;
            long zy = ((long)vsize << 16) / (nrows * 16);
            long dyz = (((dy & 0xff) * zy + 0x8000) >> 16); if (dy & 0x100) dyz = -dyz;
            vpos += flipy ? dyz : -dyz;
            long src_h = nrows * 16, scr_h = vsize, y = vpos;
            for (int row = 0; row < nrows; row++) {
                long tsh = fdiv(16 * scr_h, src_h), zoomy = fdiv(scr_h << 16, src_h);
                if (flipy) y -= tsh;
                long src_w = ncols * 16, scr_w = hsize, x = hpos;
                for (int col = 0; col < ncols; col++) {
                    long tsw = fdiv(16 * scr_w, src_w), zoomx = fdiv(scr_w << 16, src_w);
                    if (flipx) x -= tsw;
                    int ti = 0x8000 / 2 + tindex; tindex++;
                    int t = ti < 0x10000 ? ram[ti] : 0x8000;
                    if (!(t & 0x8000) && sp->ntiles < 256)
                        sp->tiles[sp->ntiles++] = (Nb1Tile){ t, (int)x, (int)y, (int)((zoomx * 16 + 0x8000) >> 16), (int)((zoomy * 16 + 0x8000) >> 16) };
                    if (!flipx) x += tsw;
                    scr_w -= tsw; src_w -= 16;
                }
                if (!flipy) y += tsh;
                scr_h -= tsh; src_h -= 16;
            }
        }
        sp->which = which; sp->pri = pri; sp->color = palette & 0xf; sp->offset = offset; sp->flipx = flipx; sp->flipy = flipy;
        sp->ncols = ncols; sp->nrows = nrows; sp->hsize = hsize; sp->vsize = vsize; sp->x = hpos; sp->y = vpos; sp->palette = palette; sp->link = link;
        if (w & 0x100) break;
    }
    return n;
}

/* source tile row/column maps for a w*h destination (nearest, the C355 stretch) */
static int tile_maps(int w, int h, int flipx, int flipy, int *xi, int *yi) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    long dx = (16L << 16) / w, dy = (16L << 16) / h;
    for (int k = 0; k < w; k++) xi[k] = (int)(((flipx ? (w - 1 - k) : k) * dx) >> 16);
    for (int k = 0; k < h; k++) yi[k] = (int)(((flipy ? (h - 1 - k) : k) * dy) >> 16);
    return 1;
}
static const uint8_t *spr_tile(const Nb1State *st, const Nb1Roms *roms, const Nb1Entry *sp, int t) {
    int code = ((t & 0x7ff) | (st->bank[(t >> 11) & 7] << 11)) + sp->offset;
    return roms->spr + (size_t)(((code % (long)roms->nspr) + roms->nspr) % roms->nspr) * 256;
}

int nb1_entry_tex(const Nb1State *st, const Nb1Roms *roms, const Nb1Entry *sp, uint8_t rgb[0x2000][3], Nb1Tex *out) {
    if (!sp->ntiles) return 0;
    int xs0 = sp->tiles[0].x, ys0 = sp->tiles[0].y, xs1 = xs0, ys1 = ys0;
    for (int i = 0; i < sp->ntiles; i++) {
        const Nb1Tile *t = &sp->tiles[i];
        if (t->x < xs0) xs0 = t->x;
        if (t->y < ys0) ys0 = t->y;
        if (t->x + t->w > xs1) xs1 = t->x + t->w;
        if (t->y + t->h > ys1) ys1 = t->y + t->h;
    }
    int w = xs1 - xs0, h = ys1 - ys0;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return 0;
    int32_t *pen = malloc((size_t)w * h * 4);
    for (size_t k = 0; k < (size_t)w * h; k++) pen[k] = -1;
    int base = sp->color * 256;
    static int xi[4096], yi[4096];
    for (int i = 0; i < sp->ntiles; i++) {
        const Nb1Tile *t = &sp->tiles[i];
        if (t->w <= 0 || t->h <= 0) continue;
        const uint8_t *src = spr_tile(st, roms, sp, t->t);
        if (!tile_maps(t->w, t->h, sp->flipx, sp->flipy, xi, yi)) continue;
        for (int yy = 0; yy < t->h; yy++) for (int xx = 0; xx < t->w; xx++) {
            int v = src[yi[yy] * 16 + xi[xx]]; if (v == 0xff) continue;
            pen[(size_t)(t->y - ys0 + yy) * w + (t->x - xs0 + xx)] = (base + v) & 0xfff;
        }
    }
    for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++) {   /* C355 clip window */
        int X = xs0 + xx, Y = ys0 + yy;
        if (!(X >= sp->clip[0] && X <= sp->clip[1] && Y >= sp->clip[2] && Y <= sp->clip[3])) pen[(size_t)yy * w + xx] = -1;
    }
    uint8_t *rgba = calloc((size_t)w * h, 4);
    for (size_t k = 0; k < (size_t)w * h; k++) {
        if (pen[k] == 0xffe) pen[k] = -2;
        else if (pen[k] >= 0) { rgba[k * 4] = rgb[pen[k]][0]; rgba[k * 4 + 1] = rgb[pen[k]][1]; rgba[k * 4 + 2] = rgb[pen[k]][2]; rgba[k * 4 + 3] = 255; }
    }
    out->x0 = xs0; out->y0 = ys0; out->w = w; out->h = h; out->rgba = rgba; out->pen = pen; return 1;
}

/* ---------------------------------------------------------------- composite */
void nb1_render_composite(const Nb1State *st, const Nb1State *sst, const Nb1Roms *roms, uint8_t *img) {
    uint8_t rgb[0x2000][3]; nb1_palette_rgb(st, rgb);
    int clipreg[6];
    for (int k = 0; k < 6; k++) clipreg[k] = (st->pal[0x1800 + 2 * k] << 8) | st->pal[0x1801 + 2 * k];
    int cx0 = sext(clipreg[0], 16) - 0x4a; if (cx0 < 0) cx0 = 0;
    int cx1 = sext(clipreg[1], 16) - 0x4a - 1; if (cx1 > NB1_W - 1) cx1 = NB1_W - 1;
    int cy0 = sext(clipreg[2], 16) - 0x21; if (cy0 < 0) cy0 = 0;
    int cy1 = sext(clipreg[3], 16) - 0x21 - 1; if (cy1 > NB1_H - 1) cy1 = NB1_H - 1;
    const int N = NB1_W * NB1_H;
    int32_t *tpen[6]; uint8_t *topq[6];
    for (int i = 0; i < 6; i++) { tpen[i] = malloc(N * 4); topq[i] = malloc(N); tilemap_screen(st, roms, i, tpen[i], topq[i]); }
    /* sprite buffer: last wins */
    int32_t *spen = malloc(N * 4); int8_t *spri = calloc(N, 1);
    for (int k = 0; k < N; k++) spen[k] = 0xffff;
    Nb1Entry *ents = malloc(256 * sizeof *ents); int ne = nb1_sprite_entries(sst, ents);
    static int xi[4096], yi[4096];
    for (int n = 0; n < ne; n++) {
        const Nb1Entry *sp = &ents[n];
        int ccx0 = sp->clip[0] > 0 ? sp->clip[0] : 0, ccy0 = sp->clip[2] > 0 ? sp->clip[2] : 0;
        int ccx1 = sp->clip[1] < NB1_W - 1 ? sp->clip[1] : NB1_W - 1, ccy1 = sp->clip[3] < NB1_H - 1 ? sp->clip[3] : NB1_H - 1;
        int base = sp->color * 256;
        for (int i = 0; i < sp->ntiles; i++) {
            const Nb1Tile *t = &sp->tiles[i];
            if (t->w <= 0 || t->h <= 0) continue;
            const uint8_t *src = spr_tile(sst, roms, sp, t->t);
            if (!tile_maps(t->w, t->h, sp->flipx, sp->flipy, xi, yi)) continue;
            int x0 = t->x > ccx0 ? t->x : ccx0, y0 = t->y > ccy0 ? t->y : ccy0;
            int x1 = t->x + t->w < ccx1 + 1 ? t->x + t->w : ccx1 + 1, y1 = t->y + t->h < ccy1 + 1 ? t->y + t->h : ccy1 + 1;
            for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
                int v = src[yi[y - t->y] * 16 + xi[x - t->x]]; if (v == 0xff) continue;
                spen[y * NB1_W + x] = (base + v) & 0xfff; spri[y * NB1_W + x] = (int8_t)sp->pri;
            }
        }
    }
    int32_t *dest = malloc(N * 4);
    for (int k = 0; k < N; k++) dest[k] = -1;
    for (int p = 0; p < 8; p++) {
        for (int i = 0; i < 6; i++) {
            int c = st->ctl[0x10 + i];
            if ((c & 7) == p && !(c & 8)) for (int k = 0; k < N; k++) if (topq[i][k]) dest[k] = tpen[i][k];
        }
        for (int k = 0; k < N; k++) {
            if (spen[k] == 0xffff || spri[k] != p) continue;
            if (spen[k] == 0xffe) { if (dest[k] >= 0) dest[k] |= 0x800; }
            else dest[k] = spen[k];
        }
    }
    for (int y = 0; y < NB1_H; y++) for (int x = 0; x < NB1_W; x++) {
        int k = y * NB1_W + x; uint8_t *o = img + k * 3;
        int inside = x >= cx0 && x <= cx1 && y >= cy0 && y <= cy1;
        if (dest[k] >= 0 && inside) { int idx = dest[k] & 0x1fff; o[0] = rgb[idx][0]; o[1] = rgb[idx][1]; o[2] = rgb[idx][2]; } else o[0] = o[1] = o[2] = 0;
    }
    for (int i = 0; i < 6; i++) { free(tpen[i]); free(topq[i]); }
    free(spen); free(spri); free(ents); free(dest);
}
