#pragma GCC diagnostic ignored "-Wmisleading-indentation"
/* The world around the play screen, built from the scene's own layers (see nb1_scene_add_surround in nb1.h).
 *
 * Everything here is a NB1_SURROUND item: a texture plus convex polygons in world coordinates. The polygons are cut so that nothing lies inside the
 * pyramid the arcade eye sees the play window through (the window itself is drawn by the real layer items), and a black cap closes the pyramid
 * behind the deepest layer, so the arcade-eye picture is unchanged. The recipe is the table's "surround" object, e.g.
 *
 *   "surround": {
 *     "clear":   "sample" | [r,g,b],                                  background colour of the world (fog colour everything fades into)
 *     "horizon_y": 112,                                              arcade pixel row of the horizon
 *     "extend":  {"layers": "all" | [0,1], "x": 2, "y": 0, "fade": [1.5, 4.5]},   continue the tilemap layers from their real scroll position:
 *                                                                    x screens left and right, y pixels above and below; fade in window half-widths
 *     "sky":     {"top": "sample" | [r,g,b], "horizon": "auto" | [r,g,b], "radius": 150, "stars": 0.0},
 *     "ground":  {"y": -1.5, "color": "sample" | [r,g,b], "texture": "sample" | "none", "rows": [150, 210], "tile": [4, 4], "fade": [15, 120]},
 *     "ceiling": {"y": 1.5, ...as ground, "rows": [20, 80]}
 *   }
 *
 * "sample" colours and textures come from the scene's own tilemap pixels (the window picture without sprites); the ground/ceiling texture is that
 * picture rectified onto the plane, mirrored and blurred. Without a "surround" section the automatic recipe only continues the layers sideways. */
#include "nb1_internal.h"
#include <time.h>

#define PYR_MARGIN 0.5          /* the cut pyramid is half a pixel smaller than the window, so the continued layer overlaps the real one by a hair */

/* ---------------------------------------------------------------- json helpers */
static double jnum(const Json *o, const char *k, double d) { const Json *v = o ? json_get(o, k) : NULL; return v && v->t == J_NUM ? v->num : d; }
/* colour: 0 absent, 1 a string ("sample" / "auto"), 2 an [r,g,b] array written to out */
static int jcol(const Json *o, const char *k, double out[3]) {
    const Json *v = o ? json_get(o, k) : NULL; if (!v) return 0;
    if (v->t == J_STR) return 1;
    if (v->t == J_ARR && v->n >= 3) { for (int i = 0; i < 3; i++) out[i] = v->kid[i]->num; return 2; }
    return 0;
}
static int jpair(const Json *o, const char *k, double *a, double *b) { const Json *v = o ? json_get(o, k) : NULL; if (!v || v->t != J_ARR || v->n < 2) return 0; *a = v->kid[0]->num; *b = v->kid[1]->num; return 1; }

/* ---------------------------------------------------------------- geometry: polygons cut against the arcade view pyramid */
typedef struct { double p[3], uv[2]; } V;
static int clip_keep(const V *in, int n, const double nr[3], V *out) {          /* keep n . p >= 0 */
    int m = 0;
    for (int i = 0; i < n; i++) {
        const V *a = &in[i], *b = &in[(i + 1) % n]; double da = dot(nr, a->p), db = dot(nr, b->p);
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0)) { double t = da / (da - db); V v; for (int k = 0; k < 3; k++) v.p[k] = a->p[k] + (b->p[k] - a->p[k]) * t; for (int k = 0; k < 2; k++) v.uv[k] = a->uv[k] + (b->uv[k] - a->uv[k]) * t; out[m++] = v; }
        if (m > 14) break;
    }
    return m;
}
static void push_poly(Nb1Item *it, const V *v, int n) {
    if (n < 3 || n > 8) return;
    it->polys = realloc(it->polys, (size_t)(it->npolys + 1) * sizeof *it->polys); Nb1Poly *q = &it->polys[it->npolys++]; q->n = n;
    for (int i = 0; i < n; i++) { memcpy(q->p[i], v[i].p, sizeof q->p[i]); memcpy(q->uv[i], v[i].uv, sizeof q->uv[i]); }
}
/* add the part of polygon v[0..n) that is OUTSIDE the pyramid (and anything behind the eye); `inside_too` keeps the inside as well (the black cap) */
static void add_outside(Nb1Item *it, const V *v, int n) {
    const double ax = (NB1_W / 2.0 - PYR_MARGIN) / NB1_FOCAL, by = (NB1_H / 2.0 - PYR_MARGIN) / NB1_FOCAL;
    const double pl[5][3] = { { 0, 0, -1 }, { 1, 0, -ax }, { -1, 0, -ax }, { 0, 1, -by }, { 0, -1, -by } };        /* near, left, right, bottom, top: inside = n . p >= 0 */
    V cur[16], out[16], keep[16]; int nc = n; memcpy(cur, v, (size_t)n * sizeof *v);
    for (int k = 0; k < 5 && nc >= 3; k++) {
        double neg[3] = { -pl[k][0], -pl[k][1], -pl[k][2] };
        int no = clip_keep(cur, nc, neg, out);                                  /* the part outside plane k */
        if (no >= 3 && no <= 8) push_poly(it, out, no);
        nc = clip_keep(cur, nc, pl[k], keep); memcpy(cur, keep, (size_t)nc * sizeof *keep);   /* continue with the part inside */
    }
}

/* ---------------------------------------------------------------- items */
typedef struct { Nb1Scene *s; int cap; } B;
static Nb1Item *new_item(Nb1Scene *s, int w, int h) {
    s->items = realloc(s->items, (size_t)(s->n + 1) * sizeof *s->items); Nb1Item *it = &s->items[s->n++]; memset(it, 0, sizeof *it);
    it->kind = NB1_SURROUND; it->id = -1; it->rule = -1; it->order = -1000 + s->n; it->w = w; it->h = h; it->tex = calloc((size_t)w * h * 4, 1);
    return it;
}
static void set_fade(Nb1Item *it, double cx, double cy, double cz, double r0, double r1, const double col[3]) {
    it->fade_on = 1; it->fade_c[0] = cx; it->fade_c[1] = cy; it->fade_c[2] = cz; it->fade_r0 = r0; it->fade_r1 = r1; memcpy(it->fade_col, col, sizeof it->fade_col);
}

/* ---------------------------------------------------------------- the window picture without sprites (for sampling) */
typedef struct { uint8_t rgb[NB1_W * NB1_H][3]; uint8_t opq[NB1_W * NB1_H]; } Comp;
static void composite(const Nb1Scene *s, Comp *c) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < s->n; i++) {
        const Nb1Item *it = &s->items[i]; if (it->kind != NB1_TILEMAP || it->hud || it->decal) continue;
        for (int y = 0; y < it->h; y++) for (int x = 0; x < it->w; x++) {
            int X = it->x0 + x, Y = it->y0 + y; if (X < 0 || X >= NB1_W || Y < 0 || Y >= NB1_H) continue;
            const uint8_t *q = it->tex + ((size_t)y * it->w + x) * 4; if (!q[3]) continue;
            int k = Y * NB1_W + X; c->rgb[k][0] = q[0]; c->rgb[k][1] = q[1]; c->rgb[k][2] = q[2]; c->opq[k] = 1;
        }
    }
}
static int avg_rows(const Comp *c, int y0, int y1, double out[3]) {
    double s[3] = { 0, 0, 0 }; long n = 0; if (y0 < 0) y0 = 0; if (y1 > NB1_H) y1 = NB1_H;
    for (int y = y0; y < y1; y++) for (int x = 0; x < NB1_W; x++) { int k = y * NB1_W + x; if (!c->opq[k]) continue; for (int j = 0; j < 3; j++) s[j] += c->rgb[k][j]; n++; }
    if (!n) return 0; for (int j = 0; j < 3; j++) out[j] = s[j] / n; return 1;
}
static void mix3(const double a[3], const double b[3], double t, double o[3]) { for (int i = 0; i < 3; i++) o[i] = a[i] + (b[i] - a[i]) * t; }

/* ---------------------------------------------------------------- sky dome */
static uint32_t rng_state = 12345u;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }
static void build_sky(Nb1Scene *s, const Json *sk, const Comp *c, double horizon_y, double top[3], double hor[3], double *R_out, double stars) {
    double R = jnum(sk, "radius", 150); *R_out = R;
    int TW = stars > 0 ? 1024 : 4, TH = stars > 0 ? 512 : 128; Nb1Item *it = new_item(s, TW, TH); it->wrap = 1;
    const double lo = -25.0, hi = 90.0, eh = atan((NB1_H / 2.0 - horizon_y) / NB1_FOCAL) * 180.0 / M_PI, ee = atan((NB1_H / 2.0) / NB1_FOCAL) * 180.0 / M_PI;      /* horizon elevation; the picture's top edge */
    double zen[3]; { double zc[3]; if (jcol(sk, "zenith", zc) == 2) memcpy(zen, zc, sizeof zen); else for (int i = 0; i < 3; i++) zen[i] = top[i] * 0.72; }
    for (int y = 0; y < TH; y++) {
        double e = hi - (hi - lo) * (y + 0.5) / TH, col[3];
        if (e <= eh) { double d = fmin(1.0, (eh - e) / 25.0); double dark[3] = { hor[0] * 0.85, hor[1] * 0.85, hor[2] * 0.85 }; mix3(hor, dark, d, col); }
        else if (e <= ee) { double t = (e - eh) / (ee - eh); t = t * t * (3 - 2 * t); mix3(hor, top, t, col); }        /* horizon -> the colour the picture has at its top edge */
        else { double t = pow(fmin(1.0, (e - ee) / (hi - ee)), 0.7); mix3(top, zen, t, col); }
        for (int x = 0; x < TW; x++) { uint8_t *q = it->tex + ((size_t)y * TW + x) * 4; q[0] = (uint8_t)col[0]; q[1] = (uint8_t)col[1]; q[2] = (uint8_t)col[2]; q[3] = 255; }
    }
    if (stars > 0) {                                                            /* equirectangular star field: more stars per row near the horizon so the density is even */
        int n = (int)(stars * 5000);
        for (int i = 0; i < n; i++) {
            double u = (rnd() & 0xffff) / 65536.0, z = 1 - 2 * ((rnd() & 0xffff) / 65536.0), e = asin(z) * 180.0 / M_PI;           /* uniform on the sphere */
            if (e < eh) continue; int x = (int)(u * TW), y = (int)((hi - e) / (hi - lo) * TH); if (x < 0 || x >= TW || y < 0 || y >= TH) continue;
            int b = 140 + (rnd() & 0x7f); uint8_t *q = it->tex + ((size_t)y * TW + x) * 4; q[0] = q[1] = (uint8_t)b; q[2] = (uint8_t)(b > 230 ? 255 : b + 25);
        }
    }
    static const double ring[] = { -25, -8, 0, 4, 9, 16, 25, 36, 50, 65, 80, 90 }; const int NR = (int)(sizeof ring / sizeof *ring), NA = 36;
    for (int r = 0; r + 1 < NR; r++) for (int a = 0; a < NA; a++) {
        double a0 = 2 * M_PI * a / NA, a1 = 2 * M_PI * (a + 1) / NA, e0 = ring[r] * M_PI / 180, e1 = ring[r + 1] * M_PI / 180; V q[4];
        double az[4] = { a0, a1, a1, a0 }, el[4] = { e0, e0, e1, e1 };
        for (int k = 0; k < 4; k++) {
            q[k].p[0] = R * cos(el[k]) * sin(az[k]); q[k].p[1] = R * sin(el[k]); q[k].p[2] = -R * cos(el[k]) * cos(az[k]);
            q[k].uv[0] = az[k] / (2 * M_PI); q[k].uv[1] = (hi - el[k] * 180.0 / M_PI) / (hi - lo);
        }
        add_outside(it, q, 4);
    }
}

/* ---------------------------------------------------------------- ground / ceiling plane */
static void blur_rgba(uint8_t *img, int w, int h, int r) {
    uint8_t *tmp = malloc((size_t)w * h * 4);
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            int sum[3] = { 0, 0, 0 }, n = 0;
            for (int d = -r; d <= r; d++) { int xx = pass == 0 ? x + d : x, yy = pass == 0 ? y : y + d; if (xx < 0) xx = 0; if (xx >= w) xx = w - 1; if (yy < 0) yy = 0; if (yy >= h) yy = h - 1; const uint8_t *q = img + ((size_t)yy * w + xx) * 4; sum[0] += q[0]; sum[1] += q[1]; sum[2] += q[2]; n++; }
            uint8_t *o = tmp + ((size_t)y * w + x) * 4; o[0] = sum[0] / n; o[1] = sum[1] / n; o[2] = sum[2] / n; o[3] = 255;
        }
        memcpy(img, tmp, (size_t)w * h * 4);
    }
    free(tmp);
}
static void build_plane(Nb1Scene *s, const Json *pl, const Comp *c, double horizon_y, double y_plane, double R, const double fog[3], int is_ceiling) {
    double col[3] = { 90, 90, 90 }; int cm = jcol(pl, "color", col); double rows0 = is_ceiling ? 20 : 150, rows1 = is_ceiling ? 80 : 210; jpair(pl, "rows", &rows0, &rows1);
    double tile_x = 4, tile_z = 4; jpair(pl, "tile", &tile_x, &tile_z); double f0 = 15, f1 = 120; jpair(pl, "fade", &f0, &f1);
    const Json *tx = pl ? json_get(pl, "texture") : NULL; int textured = tx && tx->t == J_STR && !strcmp(tx->str, "sample");
    double sampled[3];
    if (cm != 2) { if (avg_rows(c, (int)rows0, (int)rows1, sampled)) memcpy(col, sampled, sizeof col); }
    double h = fabs(y_plane); int TW = textured ? 128 : 2, TH = textured ? 128 : 2; Nb1Item *it = new_item(s, TW, TH); it->wrap = 2;
    for (size_t k = 0; k < (size_t)TW * TH; k++) { it->tex[k * 4] = (uint8_t)col[0]; it->tex[k * 4 + 1] = (uint8_t)col[1]; it->tex[k * 4 + 2] = (uint8_t)col[2]; it->tex[k * 4 + 3] = 255; }
    if (textured) {                                                              /* rectify the picture's floor rows onto the plane: ground (x, z) <- the pixel the arcade eye sees there */
        double dy0 = fabs(rows0 - NB1_H / 2.0) < 4 ? 4 : fabs(rows0 - NB1_H / 2.0), dy1 = fabs(rows1 - NB1_H / 2.0);
        double zmin = NB1_FOCAL * h / fmax(dy0, dy1), zmax = NB1_FOCAL * h / fmin(dy0, dy1);        /* depth range covered by the rows */
        double xw = 2 * (NB1_W / 2.0 - 8) * zmin / NB1_FOCAL;                    /* width such that the nearest row stays inside the picture */
        tile_x = xw; tile_z = zmax - zmin; if (tile_z < 1) tile_z = 1;
        for (int ty = 0; ty < TH; ty++) for (int txp = 0; txp < TW; txp++) {
            double z = zmin + (zmax - zmin) * (ty + 0.5) / TH, x = (txp + 0.5) / TW * xw - xw / 2;
            int px = (int)floor(NB1_W / 2.0 + NB1_FOCAL * x / z), py = (int)floor(NB1_H / 2.0 + (is_ceiling ? -1 : 1) * NB1_FOCAL * h / z);
            if (px < 0) px = 0; if (px >= NB1_W) px = NB1_W - 1; if (py < 0) py = 0; if (py >= NB1_H) py = NB1_H - 1;
            int k = py * NB1_W + px; if (!c->opq[k]) continue;
            uint8_t *q = it->tex + ((size_t)ty * TW + txp) * 4; q[0] = c->rgb[k][0]; q[1] = c->rgb[k][1]; q[2] = c->rgb[k][2];
        }
        blur_rgba(it->tex, TW, TH, 3);
    }
    V q[4] = { { { -R, y_plane, -R }, { -R / tile_x, 0 } }, { { R, y_plane, -R }, { R / tile_x, 0 } }, { { R, y_plane, R }, { R / tile_x, 2 * R / tile_z } }, { { -R, y_plane, R }, { -R / tile_x, 2 * R / tile_z } } };
    q[0].uv[1] = 0; q[1].uv[1] = 0; q[0].uv[0] = -R / tile_x; q[1].uv[0] = R / tile_x; q[2].uv[1] = q[3].uv[1] = 2 * R / tile_z;
    add_outside(it, q, 4);
    set_fade(it, 0, y_plane, 0, f0, f1, fog);
}

/* a wall plane x = xw (left: xw < 0): the picture's wall strip rectified, mirrored along the corridor */
static void build_wall(Nb1Scene *s, const Json *wj, const Comp *c, double xw, double R, const double fog[3]) {
    double ylo = -1.5, yhi = 1.5, z0 = NB1_FOCAL * fabs(xw) / (NB1_W / 2.0 - 6), z1 = 24; jpair(wj, "y", &ylo, &yhi); jpair(wj, "z", &z0, &z1);
    double f0 = 15, f1 = 120; jpair(wj, "fade", &f0, &f1); double col[3] = { 120, 120, 120 };
    const int TW = 128, TH = 128; Nb1Item *it = new_item(s, TW, TH); it->wrap = 2;
    double sum[3] = { 0, 0, 0 }; long cnt = 0;
    for (int ty = 0; ty < TH; ty++) for (int tx = 0; tx < TW; tx++) {                 /* tx runs along the corridor (depth), ty down the wall (height) */
        double z = z0 + (z1 - z0) * (tx + 0.5) / TW, y = yhi + (ylo - yhi) * (ty + 0.5) / TH;
        int px = (int)floor(NB1_W / 2.0 + NB1_FOCAL * xw / z), py = (int)floor(NB1_H / 2.0 - NB1_FOCAL * y / z);
        if (px < 0) px = 0; if (px >= NB1_W) px = NB1_W - 1; if (py < 0) py = 0; if (py >= NB1_H) py = NB1_H - 1;
        int k = py * NB1_W + px; uint8_t *q = it->tex + ((size_t)ty * TW + tx) * 4;
        if (c->opq[k]) { q[0] = c->rgb[k][0]; q[1] = c->rgb[k][1]; q[2] = c->rgb[k][2]; sum[0] += q[0]; sum[1] += q[1]; sum[2] += q[2]; cnt++; } else { q[0] = q[1] = q[2] = 0; }
        q[3] = 255;
    }
    if (cnt) for (int j = 0; j < 3; j++) col[j] = sum[j] / cnt;
    for (int k = 0; k < TW * TH; k++) if (!it->tex[k * 4] && !it->tex[k * 4 + 1] && !it->tex[k * 4 + 2]) { it->tex[k * 4] = (uint8_t)col[0]; it->tex[k * 4 + 1] = (uint8_t)col[1]; it->tex[k * 4 + 2] = (uint8_t)col[2]; }
    blur_rgba(it->tex, TW, TH, 1);
    double len = z1 - z0; V q[4] = { { { xw, yhi, R }, { -R / len, 0 } }, { { xw, yhi, -R }, { R / len, 0 } }, { { xw, ylo, -R }, { R / len, 1 } }, { { xw, ylo, R }, { -R / len, 1 } } };
    add_outside(it, q, 4); set_fade(it, 0, 0, 0, f0, f1, fog);
}

/* ---------------------------------------------------------------- the tilemap layers, continued */
static double opaque_frac(const uint8_t *map, int x0, int y0, int w, int h) {                  /* map coordinates, wrapping */
    long n = 0; for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) n += map[((((y0 + y) % 512 + 512) % 512) * 512 + (((x0 + x) % 512 + 512) % 512)) * 4 + 3] != 0;
    return (double)n / ((double)w * h);
}
static void emit_band(Nb1Item *it, double z, double ypx, double deg, int use_map, int ox, int oy, int flip) {      /* a panorama band round the viewer at depth z */
    const double TH1 = 35.0 * M_PI / 180;                                                              /* flat out to here, then round the viewer */
    double Y0 = -ypx, Y1 = NB1_H + ypx, thmax = deg * M_PI / 180, dth = 2.5 * M_PI / 180; int nseg = (int)ceil(2 * thmax / dth);
        for (int sg = 0; sg < nseg; sg++) {
            double ta = -thmax + sg * dth, tb = ta + dth, th[4] = { ta, tb, tb, ta }, yy[4] = { Y0, Y0, Y1, Y1 }; V q[4];
            for (int k = 0; k < 4; k++) {
                double t = th[k], at = fabs(t), sgn = t < 0 ? -1 : 1, px, wx, wz;
                if (at <= TH1) { wx = z * tan(t); wz = -z; px = NB1_W / 2.0 + NB1_FOCAL * tan(t); }
                else { double rho = z / cos(TH1); wx = rho * sin(t); wz = -rho * cos(t); px = NB1_W / 2.0 + NB1_FOCAL * (tan(TH1) * sgn + (at - TH1) * sgn / cos(TH1)); }
                q[k].p[0] = wx; q[k].p[1] = (NB1_H / 2.0 - yy[k]) * z / NB1_FOCAL; q[k].p[2] = wz;
                if (use_map) { double mx = px + ox, my = yy[k] + oy; if (flip) { mx = 512 - mx; my = 512 - my; } q[k].uv[0] = mx / 512.0; q[k].uv[1] = my / 512.0; }
                else { q[k].uv[0] = px / NB1_W; q[k].uv[1] = yy[k] / NB1_H; }
            }
            add_outside(it, q, 4);
        }
}
static int is_stage_door(const Nb1Item *it) { return it->kind == NB1_SPRITE && it->has_key && (it->key == 2289 || it->key == 2401 || it->key == 2415 || it->key == 2528); }
static void build_extension(Nb1Scene *s, const Nb1State *st, const Nb1Roms *roms, const Json *ex, const double fog[3]) {
    double ypx = jnum(ex, "y", 0), deg = jnum(ex, "deg", 150), f0 = 70, f1 = 150; jpair(ex, "fade", &f0, &f1);          /* azimuth in degrees to either side; fade range */
    double min_area = jnum(ex, "min_area", 9000); int with_sprites = (int)jnum(ex, "sprites", 1);
    const Json *lj = ex ? json_get(ex, "layers") : NULL; int want[4] = { 1, 1, 1, 1 };
    if (lj && lj->t == J_ARR) { for (int i = 0; i < 4; i++) want[i] = 0; for (int k = 0; k < lj->n; k++) { int L = (int)lj->kid[k]->num; if (L >= 0 && L < 4) want[L] = 1; } }
    const Json *mj = ex ? json_get(ex, "mode") : NULL; int use_map_mode = mj && mj->t == J_STR && !strcmp(mj->str, "map");
    int n0 = s->n; double min_cover = jnum(ex, "min_cover", 0.2);
    for (int a = 0; a < n0; a++) {
        const Nb1Item *base = &s->items[a]; if ((base->kind != NB1_TILEMAP && !(with_sprites && base->kind == NB1_SPRITE)) || base->hud || base->has_plane || base->decal || is_stage_door(base)) continue;
        if (base->kind == NB1_TILEMAP && (base->id < 0 || base->id > 3 || !want[base->id])) continue;
        if ((double)base->w * base->h < min_area) continue;
        { long on = 0, tot = (long)base->w * base->h; for (long k = 0; k < tot; k++) on += base->tex[k * 4 + 3] != 0; if ((double)on / (NB1_W * NB1_H) < min_cover) continue; }       /* a layer that fills a fifth of the window at least: banners and text are not scenery */
        double z = base->z * 1.002; int L = base->id, ox = 0, oy = 0, flip = 0; Nb1Item *it; int use_map = 0;
        if (base->kind == NB1_TILEMAP && (s->scroll_hint[L] || getenv("NB1_FORCE_SCROLL")) && !use_map_mode) {                  /* a layer that is scrolling: continue the game's own map so the motion carries on round the viewer */
            uint8_t *map = malloc(512 * 512 * 4);
            if (nb1_tilemap_map_tex(st, roms, L, s->rgb, map, &ox, &oy, &flip)) {
                int mx0 = flip ? 512 - (ox + NB1_W) : ox, my0 = flip ? 512 - (oy + NB1_H) : oy;
                double fin = opaque_frac(map, mx0, my0, NB1_W, NB1_H), fl = opaque_frac(map, mx0 - 64, my0, 64, NB1_H), fr = opaque_frac(map, mx0 + NB1_W, my0, 64, NB1_H);
                if (getenv("NB1_DEBUG")) fprintf(stderr, "surround: scrolling layer %d: opaque in window %.2f, beside it %.2f / %.2f\n", L, fin, fl, fr);
                if (fin > 0.02 && fl >= 0.85 * fin && fr >= 0.85 * fin) { use_map = 1; it = new_item(s, 512, 512); free(it->tex); it->tex = map; it->wrap = 1; } else free(map);
            } else free(map);
        }
        if (!use_map && use_map_mode && base->kind == NB1_TILEMAP) { uint8_t *map = malloc(512 * 512 * 4); if (nb1_tilemap_map_tex(st, roms, L, s->rgb, map, &ox, &oy, &flip)) { use_map = 1; it = new_item(s, 512, 512); free(it->tex); it->tex = map; it->wrap = 1; } else free(map); }
        if (!use_map) {                                                                 /* the item as the window shows it (hidden parts already erased), mirrored outwards */
            const Nb1Item *bs = &s->items[a]; int bx0 = bs->x0, by0 = bs->y0, bw = bs->w, bh = bs->h; const uint8_t *src = bs->tex;
            it = new_item(s, NB1_W, NB1_H); it->wrap = 2; bs = &s->items[a]; src = bs->tex;                      /* new_item may have moved the array */
            for (int y = 0; y < bh; y++) { int Y = by0 + y; if (Y < 0 || Y >= NB1_H) continue; for (int x = 0; x < bw; x++) { int X = bx0 + x; if (X < 0 || X >= NB1_W) continue; memcpy(it->tex + ((size_t)Y * NB1_W + X) * 4, src + ((size_t)y * bw + x) * 4, 4); } }
        }
        emit_band(it, z, ypx, deg, use_map, ox, oy, flip);
        it->fade_on = 2; it->fade_r0 = f0 * M_PI / 180; it->fade_r1 = f1 * M_PI / 180; memcpy(it->fade_col, fog, sizeof it->fade_col);              /* fade_on 2: by azimuth */
    }
}

/* ---------------------------------------------------------------- entry */
int nb1_scene_add_surround(Nb1Scene *s, const Nb1State *st, const Nb1Roms *roms, const Nb1Table *tab) {
    clock_t t_start = clock(); int n_before = s->n;
    static const char *AUTO = "{\"surround\":{\"extend\":{\"layers\":\"all\",\"x\":1.5,\"fade\":[1.4,3.2]}}}";
    Json *autoj = NULL; const Json *S = tab && tab->root && tab->root->t == J_OBJ ? json_get(tab->root, "surround") : NULL;
    if (!S) { autoj = json_parse(AUTO); S = json_get(autoj, "surround"); }
    Comp *c = malloc(sizeof *c); composite(s, c);
    double horizon_y = jnum(S, "horizon_y", NB1_H / 2.0);
    const Json *sky = json_get(S, "sky"), *ex = json_get(S, "extend"), *gr = json_get(S, "ground"), *ce = json_get(S, "ceiling");
    /* colours */
    double top[3] = { 40, 70, 140 }, hor[3] = { 190, 210, 235 }, fog[3] = { 60, 60, 70 };
    if (sky) {
        double tc[3]; int m = jcol(sky, "top", tc);
        if (m == 2) memcpy(top, tc, sizeof top); else if (!avg_rows(c, 0, 4, top)) { top[0] = 30; top[1] = 40; top[2] = 90; }
        double hc[3]; int hm = jcol(sky, "horizon", hc);
        if (hm == 2) memcpy(hor, hc, sizeof hor); else { double white[3] = { 255, 255, 255 }; mix3(top, white, 0.45, hor); }
        memcpy(fog, hor, sizeof fog);
    } else if (!avg_rows(c, 0, NB1_H, fog)) { fog[0] = fog[1] = fog[2] = 40; }
    { double cc[3]; int m = jcol(S, "clear", cc); if (m == 2) memcpy(fog, cc, sizeof fog); }
    for (int i = 0; i < 3; i++) s->clear[i] = fog[i]; s->has_clear = 1;
    /* extension of the real layers */
    if (ex) build_extension(s, st, roms, ex, fog);
    { const Json *bd = json_get(S, "backdrop");                                  /* a plain coloured wall round the viewer, e.g. behind a flat screen */
      if (bd) {
        double col[3] = { 90, 90, 90 }; int cm = jcol(bd, "color", col); if (cm != 2) avg_rows(c, 0, NB1_H, col); double dim = jnum(bd, "dim", 1.0); for (int i = 0; i < 3; i++) col[i] *= dim;
        double z = jnum(bd, "z", 3.2), f0 = 70, f1 = 150; jpair(bd, "fade", &f0, &f1);
        Nb1Item *it = new_item(s, 2, 2); it->wrap = 0; for (int k = 0; k < 4; k++) { it->tex[k * 4] = (uint8_t)col[0]; it->tex[k * 4 + 1] = (uint8_t)col[1]; it->tex[k * 4 + 2] = (uint8_t)col[2]; it->tex[k * 4 + 3] = 255; }
        emit_band(it, z, jnum(bd, "y", 400), jnum(bd, "deg", 150), 0, 0, 0, 0);
        it->fade_on = 2; it->fade_r0 = f0 * M_PI / 180; it->fade_r1 = f1 * M_PI / 180; memcpy(it->fade_col, fog, sizeof it->fade_col);
      } }
    double R = 150;
    if (sky) build_sky(s, sky, c, horizon_y, top, hor, &R, jnum(sky, "stars", 0));
    if (gr) build_plane(s, gr, c, horizon_y, jnum(gr, "y", -1.5), R * 0.9, fog, 0);
    if (ce) build_plane(s, ce, c, horizon_y, jnum(ce, "y", 1.5), R * 0.9, fog, 1);
    { const Json *wl = json_get(S, "walls"); if (wl) { double xw = jnum(wl, "x", 1.7); build_wall(s, wl, c, -xw, R * 0.5, fog); build_wall(s, wl, c, xw, R * 0.5, fog); } }
    /* the black cap behind the deepest layer: what the arcade shows where no layer draws */
    double zmax = 1; for (int i = 0; i < s->n; i++) { const Nb1Item *it = &s->items[i]; if (it->kind == NB1_TILEMAP || it->kind == NB1_SPRITE) if (!it->hud && it->z > zmax && it->z < 100) zmax = it->z; }
    { Nb1Item *it = new_item(s, 2, 2); it->wrap = 0; for (int k = 0; k < 4; k++) it->tex[k * 4 + 3] = 255;
      double z = zmax * 1.05 + 0.5, hw = NB1_W / 2.0 * z / NB1_FOCAL, hh = NB1_H / 2.0 * z / NB1_FOCAL;
      V q[4] = { { { -hw, hh, -z }, { 0, 0 } }, { { hw, hh, -z }, { 1, 0 } }, { { hw, -hh, -z }, { 1, 1 } }, { { -hw, -hh, -z }, { 0, 1 } } }; push_poly(it, q, 4); }
    if (getenv("NB1_DEBUG")) { long np = 0; for (int i = n_before; i < s->n; i++) np += s->items[i].npolys; fprintf(stderr, "surround: %d items, %ld polygons, %.1f ms\n", s->n - n_before, np, 1000.0 * (clock() - t_start) / CLOCKS_PER_SEC); }
    free(c); json_free(autoj); return 0;
}
