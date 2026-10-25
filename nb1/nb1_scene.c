/* NB-1 depth scene: depth table, erasers, quads, z-buffered view, aim solver, gun registers.
 * Port of oracle/depthscene.py. */
#include "nb1_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const double NB1_FOCAL = 346.0;

/* ---------------------------------------------------------------- depths */
static double fclampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

static void assign_depths(Nb1Item *items, int n, const Nb1Table *tab) {
    char *explicit_ = calloc(n, 1);
    for (int a = 0; a < n; a++) {
        Nb1Item *it = &items[a]; it->rule = -1;
        for (int k = 0; k < tab->nrules; k++) {
            const Rule *r = &tab->rules[k];
            if (!nb1_match_item(&r->m, it)) continue;
            it->rule = k;
            if (r->hud) it->hud = 1;
            if (r->has_z) { it->z = r->z; explicit_[a] = 1; }
            if (r->has_plane) { memcpy(it->pn, r->pn, sizeof it->pn); it->pd = r->pd; it->has_plane = 1; explicit_[a] = 1; }
            if (r->has_fc) { double dy = (it->y0 + it->h) - CY; if (dy > 1) { it->z = fclampd(FOCAL * fabs(r->fc) / dy, 1.5, tab->far); explicit_[a] = 1; } }
            if (r->has_cc) { double dy = CY - it->y0; if (dy > 1) { it->z = fclampd(FOCAL * fabs(r->cc) / dy, 1.5, tab->far); explicit_[a] = 1; } }
            if (r->has_zd && it->zoom != 0) { it->z = fclampd(r->zd / fmax(it->zoom, 0.02), r->zlo, r->zhi); explicit_[a] = 1; }
            it->tilt_x = r->tilt_x; it->tilt_y = r->tilt_y;
            break;
        }
    }
    for (int a = 0; a < n; a++) if (items[a].hud) { items[a].z = tab->hud_z; explicit_[a] = 1; }
    /* a FIXED slot per (priority, kind): an untagged layer's depth never depends on which other slots exist */
    double step = (tab->z_back - tab->z_front) / 15.0;
    for (int a = 0; a < n; a++) if (!explicit_[a])
        items[a].z = tab->z_back - step * (items[a].pri * 2 + (items[a].kind == NB1_TILEMAP ? 0 : 1));
    for (int k = 0; k < tab->nscales; k++)                                   /* "scales": move matching layers along the eye's rays */
        for (int a = 0; a < n; a++) {
            Nb1Item *it = &items[a]; if (it->hud || !nb1_match_item(&tab->scales[k].m, it)) continue;
            it->z *= tab->scales[k].k; it->pd *= tab->scales[k].k; explicit_[a] = 1;
        }
    for (int a = 0; a < n; a++) {
        Nb1Item *it = &items[a];
        if (it->has_plane) {
            double ray[3]; unproject(it->x0 + it->w / 2.0, it->y0 + it->h / 2.0, 1.0, ray);
            double den = dot(ray, it->pn);
            it->z = (fabs(den) > 1e-9 && it->pd / den > 0) ? -(ray[2] * (it->pd / den)) : 30.0;
        }
        it->z -= tab->slot_gap * it->order;
    }
    /* an untagged item drawn later must not end up behind something earlier */
    int *ord = malloc(n * sizeof *ord); for (int i = 0; i < n; i++) ord[i] = i;
    for (int i = 1; i < n; i++) { int v = ord[i], j = i - 1; while (j >= 0 && items[ord[j]].order > items[v].order) { ord[j + 1] = ord[j]; j--; } ord[j + 1] = v; }
    double floor_ = INFINITY;
    for (int i = 0; i < n; i++) {
        Nb1Item *it = &items[ord[i]];
        if (!explicit_[ord[i]] && it->z >= floor_) it->z = floor_ - tab->slot_gap;
        if (it->z < floor_) floor_ = it->z;
    }
    free(ord); free(explicit_);
}

static void item_corners(const Nb1Item *it, double ctr[3], double r[3], double u[3]);
/* Decals (shot marks) lie on whatever the arcade drew them over: they inherit that item's plane, tilt and depth. */
#define DECAL_EPS 0.002
static int is_decal_code(const Nb1Table *tab, const Nb1Item *it) {
    if (it->kind != NB1_SPRITE || !it->has_key) return 0;
    for (int k = 0; k < tab->ndecals; k++) if (tab->decal_lo[k] <= it->key && it->key <= tab->decal_hi[k]) return 1;
    return 0;
}
static void prepare(Nb1Item *it);
static void render_cam(const Nb1Scene *s, const Nb1Cam *c, uint8_t *rgb, int32_t *ids, const uint8_t *include);

/* Give decal piece m the plane / tilt / depth of the surface `owner` it lies on. */
static void place_decal(Nb1Item *m, const Nb1Item *owner) {
    m->hud = owner->hud; m->tilt_x = owner->tilt_x; m->tilt_y = owner->tilt_y; m->has_plane = owner->has_plane;
    memcpy(m->pn, owner->pn, sizeof m->pn); m->pd = owner->pd;
    double ray[3]; unproject(m->x0 + m->w / 2.0, m->y0 + m->h / 2.0, 1.0, ray);
    if (owner->hud) m->z = owner->z - DECAL_EPS;
    else if (owner->has_plane) {
        double den = dot(ray, owner->pn);
        m->z = (fabs(den) > 1e-9 && owner->pd / den > 0) ? -(ray[2] * (owner->pd / den)) - DECAL_EPS : owner->z - DECAL_EPS;
    } else {
        double ctr[3], r[3], u[3], nn[3]; item_corners(owner, ctr, r, u); cross(r, u, nn);
        double den = dot(ray, nn), t = fabs(den) > 1e-12 ? dot(ctr, nn) / den : -1.0;
        m->z = (t > 0 ? t : owner->z) - DECAL_EPS;
    }
}

/* Each decal pixel belongs to the surface the arcade eye sees there among the items drawn BEFORE the decal. A decal that straddles
 * surfaces is split into one piece per surface (largest first; the first piece replaces the decal in place, the rest are appended).
 * Returns the new item count; the array must have room for `cap` items. */
static int apply_decals(Nb1Item *items, int n, int cap, const Nb1Table *tab, double far, uint8_t rgb[0x2000][3]) {
    int n0 = n;
    Nb1Scene tmp = { items, n, far, { { 0 } } }; memcpy(tmp.rgb, rgb, sizeof tmp.rgb);
    Nb1Cam eye; nb1_arcade_camera(&eye, 1);
    uint8_t *include = malloc(cap), *img = malloc(NB1_W * NB1_H * 3); int32_t *sub = malloc(NB1_W * NB1_H * 4);
    for (int a = 0; a < n0; a++) {
        Nb1Item *m = &items[a]; if (!is_decal_code(tab, m)) continue;
        m->decal = 1;
        for (int b = 0; b < n; b++) include[b] = (b != a && items[b].kind != NB1_MASK && items[b].order < m->order && !is_decal_code(tab, &items[b]));
        for (int k = 0; k < NB1_W * NB1_H; k++) sub[k] = -1;
        tmp.n = n; render_cam(&tmp, &eye, img, sub, include);
        size_t np = (size_t)m->w * m->h; int *own = malloc(np * sizeof *own); int counts[64] = { 0 }, owners[64], no = 0;
        for (int y = 0; y < m->h; y++) for (int x = 0; x < m->w; x++) {
            int X = m->x0 + x, Y = m->y0 + y, o = (X >= 0 && X < NB1_W && Y >= 0 && Y < NB1_H) ? sub[Y * NB1_W + X] : -1;
            own[(size_t)y * m->w + x] = o;
            if (o >= 0 && m->tex[((size_t)y * m->w + x) * 4 + 3]) { int k = 0; while (k < no && owners[k] != o) k++; if (k == no && no < 64) owners[no++] = o; if (k < no) counts[k]++; }
        }
        for (int i = 1; i < no; i++) { int ov = owners[i], cv = counts[i], j = i - 1;      /* count desc, then index asc */
            while (j >= 0 && (counts[j] < cv || (counts[j] == cv && owners[j] > ov))) { owners[j + 1] = owners[j]; counts[j + 1] = counts[j]; j--; } owners[j + 1] = ov; counts[j + 1] = cv; }
        if (no) {
            uint8_t *orig_tex = malloc(np * 4); int32_t *orig_pen = malloc(np * 4); memcpy(orig_tex, m->tex, np * 4); memcpy(orig_pen, m->pen, np * 4);
            Nb1Item base = *m;
            for (int k = 0; k < no; k++) {
                Nb1Item *piece = m;
                if (k) { if (n >= cap) break; piece = &items[n++]; *piece = base; piece->tex = malloc(np * 4); piece->pen = malloc(np * 4); }
                for (size_t q = 0; q < np; q++) {
                    int sel = orig_tex[q * 4 + 3] && (own[q] == owners[k] || (k == 0 && own[q] < 0));
                    for (int ch = 0; ch < 4; ch++) piece->tex[q * 4 + ch] = sel ? orig_tex[q * 4 + ch] : 0;
                    piece->pen[q] = sel ? orig_pen[q] : -1;
                }
                piece->decal = 1; place_decal(piece, &items[owners[k]]); prepare(piece);
            }
            free(orig_tex); free(orig_pen);
        }
        free(own);
    }
    free(include); free(img); free(sub); return n;
}

/* ---------------------------------------------------------------- erasers */
static void apply_erasers(Nb1Item *items, int n) {
    int *spr = malloc(n * sizeof *spr), ns = 0;
    for (int i = 0; i < n; i++) if (items[i].kind == NB1_SPRITE) spr[ns++] = i;
    for (int i = 1; i < ns; i++) { int v = spr[i], j = i - 1; while (j >= 0 && items[spr[j]].listidx < items[v].listidx) { spr[j + 1] = spr[j]; j--; } spr[j + 1] = v; }
    uint8_t *later_opaque = calloc(9 * NB1_W * NB1_H, 1), *later_shadow = calloc(NB1_W * NB1_H, 1);
    for (int s = 0; s < ns; s++) {
        Nb1Item *it = &items[spr[s]];
        int sx0 = it->x0 > 0 ? it->x0 : 0, sy0 = it->y0 > 0 ? it->y0 : 0;
        int sx1 = it->x0 + it->w < NB1_W ? it->x0 + it->w : NB1_W, sy1 = it->y0 + it->h < NB1_H ? it->y0 + it->h : NB1_H;
        if (sx1 <= sx0 || sy1 <= sy0) continue;
        int cnt = 0;
        for (int y = sy0; y < sy1; y++) for (int x = sx0; x < sx1; x++) {
            size_t k = (size_t)(y - it->y0) * it->w + (x - it->x0); int sp = y * NB1_W + x;
            int opaque = it->pen[k] >= 0, shadow = it->pen[k] == -2;
            int erase = later_shadow[sp];
            for (int p = 0; p < it->pri && !erase; p++) erase |= later_opaque[p * NB1_W * NB1_H + sp];
            if (erase) { it->pen[k] = -1; it->tex[k * 4] = it->tex[k * 4 + 1] = it->tex[k * 4 + 2] = it->tex[k * 4 + 3] = 0; cnt++; }
            if (opaque) later_opaque[it->pri * NB1_W * NB1_H + sp] = 1;
            if (shadow) later_shadow[sp] = 1;
        }
        if (cnt) it->erased = cnt;
    }
    free(spr); free(later_opaque); free(later_shadow);
}

/* ---------------------------------------------------------------- geometry */
static void item_corners(const Nb1Item *it, double ctr[3], double r[3], double u[3]) {
    unproject(it->x0 + it->w / 2.0, it->y0 + it->h / 2.0, it->z, ctr);
    r[0] = it->w / 2.0 * it->z / FOCAL; r[1] = 0; r[2] = 0;
    u[0] = 0; u[1] = it->h / 2.0 * it->z / FOCAL; u[2] = 0;
    if (it->tilt_y) { double a = it->tilt_y * M_PI / 180.0, c = cos(a), s = sin(a), r0 = r[0]; r[0] = r0 * c; r[1] = 0; r[2] = -r0 * s; }
    if (it->tilt_x) { double a = it->tilt_x * M_PI / 180.0, c = cos(a), s = sin(a), u1 = u[1]; u[0] = 0; u[1] = u1 * c; u[2] = u1 * s; }
}
static void prepare(Nb1Item *it) {
    item_corners(it, it->ctr, it->r, it->u); cross(it->r, it->u, it->nrm); it->rr = dot(it->r, it->r); it->uu = dot(it->u, it->u);
}

int nb1_build_scene(const Nb1State *st, const Nb1State *sst, const Nb1Roms *roms, const Nb1Table *tab, Nb1Scene *out) {
    memset(out, 0, sizeof *out);
    Nb1Table *own = NULL; if (!tab) tab = own = nb1_table_default();
    out->far = tab->far;
    nb1_palette_rgb(st, out->rgb);
    int cap = 1100; Nb1Item *items = calloc(cap, sizeof *items); int n = 0, order = 0;
    Nb1Entry *ents = malloc(256 * sizeof *ents); int ne = nb1_sprite_entries(sst, ents);
    for (int p = 0; p < 8; p++) {
        for (int i = 0; i < 6; i++) {
            int c = st->ctl[0x10 + i];
            if (!((c & 7) == p && !(c & 8))) continue;
            uint8_t *rgba = malloc(NB1_W * NB1_H * 4); int32_t *pen = malloc(NB1_W * NB1_H * 4);
            nb1_tilemap_tex(st, roms, i, out->rgb, rgba, pen);
            int any = 0; for (int k = 0; k < NB1_W * NB1_H && !any; k++) any = rgba[k * 4 + 3] != 0;
            if (!any) { free(rgba); free(pen); continue; }
            Nb1Item *it = &items[n++]; it->kind = NB1_TILEMAP; it->id = i; it->pri = p; it->order = order++; it->w = NB1_W; it->h = NB1_H;
            it->tex = rgba; it->pen = pen; it->pal = st->ctl[0x18 + i] & 7; it->ncols = 36; it->nrows = 28; it->rule = -1;
        }
        for (int e = 0; e < ne; e++) {
            const Nb1Entry *sp = &ents[e]; if (sp->pri != p) continue;
            Nb1Tex t; if (!nb1_entry_tex(sst, roms, sp, out->rgb, &t)) continue;
            Nb1Item *it = &items[n++]; it->kind = NB1_SPRITE; it->id = sp->which; it->pri = p; it->order = order++;
            it->x0 = t.x0; it->y0 = t.y0; it->w = t.w; it->h = t.h; it->tex = t.rgba; it->pen = t.pen;
            if (sp->ntiles) { const Nb1Tile *f = &sp->tiles[0]; it->has_key = 1; it->key = ((f->t & 0x7ff) | (sst->bank[(f->t >> 11) & 7] << 11)) + sp->offset; }
            it->pal = sp->color; it->ncols = sp->ncols; it->nrows = sp->nrows; it->listidx = e; it->rule = -1;
            it->zoom = sp->hsize / (sp->ncols * 16.0);
            for (size_t k = 0; k < (size_t)t.w * t.h && !it->shadow; k++) it->shadow = t.pen[k] == -2;
        }
    }
    free(ents);
    apply_erasers(items, n);
    n = nb1_apply_edits(items, n, cap - 1, tab, out->rgb);
    /* C116 blanking window: black outside regs 0-3 (iris / curtain) */
    int cr[4], wx0, wx1, wy0, wy1;
    for (int k = 0; k < 4; k++) cr[k] = (st->pal[0x1800 + 2 * k] << 8) | st->pal[0x1801 + 2 * k];
    wx0 = ((cr[0] & 0x8000) ? (cr[0] - 65536) : cr[0]) - 0x4a; if (wx0 < 0) wx0 = 0;
    wx1 = ((cr[1] & 0x8000) ? (cr[1] - 65536) : cr[1]) - 0x4a - 1; if (wx1 > NB1_W - 1) wx1 = NB1_W - 1;
    wy0 = ((cr[2] & 0x8000) ? (cr[2] - 65536) : cr[2]) - 0x21; if (wy0 < 0) wy0 = 0;
    wy1 = ((cr[3] & 0x8000) ? (cr[3] - 65536) : cr[3]) - 0x21 - 1; if (wy1 > NB1_H - 1) wy1 = NB1_H - 1;
    assign_depths(items, n, tab);
    for (int a = 0; a < n; a++) prepare(&items[a]);
    n = apply_decals(items, n, cap - 1, tab, out->far, out->rgb);
    if (wx0 != 0 || wy0 != 0 || wx1 != NB1_W - 1 || wy1 != NB1_H - 1) {
        Nb1Item *it = &items[n]; it->kind = NB1_MASK; it->id = -1; it->pri = 8; it->order = 1000000; it->w = NB1_W; it->h = NB1_H;
        it->tex = calloc(NB1_W * NB1_H, 4); it->pen = malloc(NB1_W * NB1_H * 4); it->rule = -1; it->hud = 1; it->ncols = 36; it->nrows = 28;
        for (int y = 0; y < NB1_H; y++) for (int x = 0; x < NB1_W; x++) {
            int inside = x >= wx0 && x <= wx1 && y >= wy0 && y <= wy1;
            it->pen[y * NB1_W + x] = inside ? -1 : -3; it->tex[(y * NB1_W + x) * 4 + 3] = inside ? 0 : 255;
        }
        double zmin = INFINITY; for (int a = 0; a < n; a++) if (items[a].z < zmin) zmin = items[a].z;
        it->z = zmin - 0.01; n++;
    }
    for (int a = 0; a < n; a++) prepare(&items[a]);
    out->items = items; out->n = n; nb1_table_free(own); return 0;
}
void nb1_scene_free(Nb1Scene *s) { for (int i = 0; i < s->n; i++) { free(s->items[i].tex); free(s->items[i].pen); free(s->items[i].cust); } free(s->items); memset(s, 0, sizeof *s); }

/* ---------------------------------------------------------------- ray / item */
/* Plane corners (4 x V3) or 0 when any corner misses the plane. */
static int plane_corners(const Nb1Item *it, double far, double c[4][3]) {
    const int xs[4] = { it->x0, it->x0 + it->w, it->x0 + it->w, it->x0 }, ys[4] = { it->y0, it->y0, it->y0 + it->h, it->y0 + it->h };
    for (int k = 0; k < 4; k++) {
        double ray[3]; unproject(xs[k], ys[k], 1.0, ray); double den = dot(ray, it->pn);
        if (fabs(den) < 1e-9 || it->pd / den <= 0) return 0;
        double q = it->pd / den; c[k][0] = ray[0] * q; c[k][1] = ray[1] * q; c[k][2] = ray[2] * q;
        if (-c[k][2] > far) return 0;
    }
    return 1;
}
static int hit_ray(const Nb1Item *it, double far, const double *E, const double *Dw, double *t_out, int *tx_out, int *ty_out) {
    double t, P[3]; int tx, ty;
    if (it->has_plane) {
        double den = dot(Dw, it->pn); t = (it->pd - dot(E, it->pn)) / den;
        if (!isfinite(t)) return 0;
        for (int k = 0; k < 3; k++) P[k] = E[k] + Dw[k] * t;
        double zc = -P[2]; if (!(t > 0 && zc > 0.05 && zc <= far)) return 0;
        double sx = CX + FOCAL * P[0] / zc, sy = CY - FOCAL * P[1] / zc;
        double fx = floor(sx - it->x0), fy = floor(sy - it->y0);
        if (!(fx >= 0 && fx < it->w && fy >= 0 && fy < it->h)) return 0;
        tx = (int)fx; ty = (int)fy;
    } else {
        double ce[3] = { it->ctr[0] - E[0], it->ctr[1] - E[1], it->ctr[2] - E[2] };
        t = dot(ce, it->nrm) / dot(Dw, it->nrm);
        if (!isfinite(t) || !(t > 0)) return 0;
        for (int k = 0; k < 3; k++) P[k] = E[k] + Dw[k] * t;
        double rel[3] = { P[0] - it->ctr[0], P[1] - it->ctr[1], P[2] - it->ctr[2] };
        double s = dot(rel, it->r) / it->rr, q = dot(rel, it->u) / it->uu;
        double fx = floor((s + 1) / 2 * it->w), fy = floor((1 - (q + 1) / 2) * it->h);
        if (!(fx >= 0 && fx < it->w && fy >= 0 && fy < it->h)) return 0;
        tx = (int)fx; ty = (int)fy;
    }
    *t_out = t; *tx_out = tx; *ty_out = ty; return 1;
}

/* ---------------------------------------------------------------- cameras / rendering */
void nb1_orbit_camera(const double target[3], double distance, double yaw_deg, double pitch_deg, Nb1Cam *c) {
    double ya = yaw_deg * M_PI / 180.0, pa = pitch_deg * M_PI / 180.0;
    double off[3] = { sin(ya) * cos(pa), sin(pa), cos(ya) * cos(pa) };
    for (int k = 0; k < 3; k++) c->E[k] = target[k] + distance * off[k];
    double f[3] = { target[0] - c->E[0], target[1] - c->E[1], target[2] - c->E[2] }, l = sqrt(dot(f, f));
    for (int k = 0; k < 3; k++) c->fwd[k] = f[k] / l;
    double up0[3] = { 0, 1, 0 }, r[3]; cross(c->fwd, up0, r); l = sqrt(dot(r, r));
    for (int k = 0; k < 3; k++) c->right[k] = r[k] / l;
    cross(c->right, c->fwd, c->up);
}
void nb1_arcade_camera(Nb1Cam *c, int scale) {
    memset(c, 0, sizeof *c); c->fwd[2] = -1; c->right[0] = 1; c->up[1] = 1; c->f = FOCAL * scale; c->w = NB1_W * scale; c->h = NB1_H * scale;
}

static void draw_item(const Nb1Scene *s, int item_index, const Nb1Item *it, const Nb1Cam *c, int shadow_pass, int32_t *pen, uint8_t *col, double *zb, int32_t *ids) {
    int ow = c->w, oh = c->h; double cx = ow / 2.0, cy = oh / 2.0;
    int x0, x1, y0, y1; double cs[4][3]; int have;
    if (it->has_plane) have = plane_corners(it, s->far, cs);
    else {
        have = 1; int k = 0;
        for (int sx = -1; sx <= 1; sx += 2) for (int sy = -1; sy <= 1; sy += 2, k++)
            for (int a = 0; a < 3; a++) cs[k][a] = it->ctr[a] + sx * it->r[a] + sy * it->u[a];
    }
    if (!have) { x0 = 0; x1 = ow; y0 = 0; y1 = oh; }
    else {
        double us[4], vs[4], zmin = INFINITY;
        for (int k = 0; k < 4; k++) {
            double rel[3] = { cs[k][0] - c->E[0], cs[k][1] - c->E[1], cs[k][2] - c->E[2] }, z = dot(rel, c->fwd);
            if (z < zmin) zmin = z;
            us[k] = cx + c->f * dot(rel, c->right) / z; vs[k] = cy - c->f * dot(rel, c->up) / z;
        }
        if (zmin <= 0.05) { x0 = 0; x1 = ow; y0 = 0; y1 = oh; }
        else {
            double umin = us[0], umax = us[0], vmin = vs[0], vmax = vs[0];
            for (int k = 1; k < 4; k++) { umin = fmin(umin, us[k]); umax = fmax(umax, us[k]); vmin = fmin(vmin, vs[k]); vmax = fmax(vmax, vs[k]); }
            x0 = (int)fmax(floor(umin), 0); x1 = (int)fmin(ceil(umax), ow); y0 = (int)fmax(floor(vmin), 0); y1 = (int)fmin(ceil(vmax), oh);
        }
    }
    if (x1 <= x0 || y1 <= y0) return;
    for (int v = y0; v < y1; v++) for (int u = x0; u < x1; u++) {
        double a = ((u + 0.5) - cx) / c->f, b = (cy - (v + 0.5)) / c->f, Dw[3];
        for (int k = 0; k < 3; k++) Dw[k] = a * c->right[k] + b * c->up[k] + c->fwd[k];
        double t; int tx, ty;
        if (!hit_ray(it, s->far, c->E, Dw, &t, &tx, &ty)) continue;
        int p = it->pen[(size_t)ty * it->w + tx], k = v * ow + u;
        if (shadow_pass) {
            if (p == -2 && t < zb[k]) {
                if (pen[k] >= 0) { pen[k] |= 0x800; const uint8_t *q = s->rgb[pen[k] & 0x1fff]; col[k * 3] = q[0]; col[k * 3 + 1] = q[1]; col[k * 3 + 2] = q[2]; }
                else if (pen[k] == -4) { col[k * 3] /= 2; col[k * 3 + 1] /= 2; col[k * 3 + 2] /= 2; }
            }
            continue;
        }
        if (!(t < zb[k] + 1e-9)) continue;
        if (p >= 0) { pen[k] = p; const uint8_t *q = s->rgb[p & 0x1fff]; col[k * 3] = q[0]; col[k * 3 + 1] = q[1]; col[k * 3 + 2] = q[2]; zb[k] = t; if (ids && !it->decal) ids[k] = item_index; }
        else if (p == -4 && it->cust) { const uint8_t *q = it->cust + ((size_t)ty * it->w + tx) * 3; pen[k] = -4; col[k * 3] = q[0]; col[k * 3 + 1] = q[1]; col[k * 3 + 2] = q[2]; zb[k] = t; if (ids && !it->decal) ids[k] = item_index; }
        else if (p == -3) { pen[k] = -1; col[k * 3] = col[k * 3 + 1] = col[k * 3 + 2] = 0; zb[k] = t; if (ids) ids[k] = -1; }
    }
}
static void render_cam(const Nb1Scene *s, const Nb1Cam *c, uint8_t *rgb, int32_t *ids, const uint8_t *include) {
    size_t N = (size_t)c->w * c->h; int32_t *pen = malloc(N * 4); double *zb = malloc(N * sizeof *zb);
    for (size_t k = 0; k < N; k++) { pen[k] = -1; zb[k] = INFINITY; }
    memset(rgb, 0, N * 3);
    int *ord = malloc(s->n * sizeof *ord); for (int i = 0; i < s->n; i++) ord[i] = i;
    for (int i = 1; i < s->n; i++) { int v = ord[i], j = i - 1; while (j >= 0 && s->items[ord[j]].order > s->items[v].order) { ord[j + 1] = ord[j]; j--; } ord[j + 1] = v; }
    for (int i = 0; i < s->n; i++) if (!include || include[ord[i]]) draw_item(s, ord[i], &s->items[ord[i]], c, 0, pen, rgb, zb, ids);
    for (int i = 0; i < s->n; i++) if (s->items[ord[i]].shadow && (!include || include[ord[i]])) draw_item(s, ord[i], &s->items[ord[i]], c, 1, pen, rgb, zb, NULL);
    free(ord); free(pen); free(zb);
}
void nb1_render_cam(const Nb1Scene *s, const Nb1Cam *c, uint8_t *rgb) { render_cam(s, c, rgb, NULL, NULL); }
void nb1_visible_map(const Nb1Scene *s, int32_t *ids) {
    Nb1Cam c; nb1_arcade_camera(&c, 1);
    for (int k = 0; k < NB1_W * NB1_H; k++) ids[k] = -1;
    uint8_t *rgb = malloc(NB1_W * NB1_H * 3); render_cam(s, &c, rgb, ids, NULL); free(rgb);
}

/* ---------------------------------------------------------------- aim */
int nb1_aim(const Nb1Scene *s, const double O[3], const double D[3], double fallback_z, Nb1Aim *out) {
    int best = -1; double bt = 0, P[3];
    for (int i = 0; i < s->n; i++) {
        const Nb1Item *it = &s->items[i]; if (it->kind == NB1_MASK || it->decal || (s->aim_skip_hud && it->hud)) continue;
        double t; int tx, ty;
        if (!hit_ray(it, s->far, O, D, &t, &tx, &ty)) continue;
        if (it->tex[((size_t)ty * it->w + tx) * 4 + 3] == 0) continue;
        if (best < 0 || t < bt) { best = i; bt = t; }
    }
    if (best >= 0) for (int k = 0; k < 3; k++) P[k] = O[k] + D[k] * bt;
    else {
        if (fallback_z < 0) return 0;
        double zf = fallback_z != 0 ? fallback_z : 2.5;
        if (D[2] >= -1e-6) return 0;
        double t = (-zf - O[2]) / D[2]; if (t < 0) return 0;
        for (int k = 0; k < 3; k++) P[k] = O[k] + D[k] * t;
    }
    if (P[2] >= -1e-3) return 0;
    out->item = best; memcpy(out->hit, P, sizeof P);
    out->sx = CX + FOCAL * P[0] / -P[2]; out->sy = CY - FOCAL * P[1] / -P[2]; return 1;
}

int nb1_aim_snap(const Nb1Scene *s, const int32_t *ids, const double O[3], const double D[3], double radius, Nb1AimSnap *out) {
    memset(out, 0, sizeof *out);
    if (!nb1_aim(s, O, D, -1, &out->a)) return 0;
    out->raw_sx = out->a.sx; out->raw_sy = out->a.sy;
    int x = (int)floor(out->a.sx), y = (int)floor(out->a.sy), inside = x >= 0 && x < NB1_W && y >= 0 && y < NB1_H;
    out->cover = inside ? ids[y * NB1_W + x] : -1;
    if (inside && out->cover == out->a.item) return 1;
    double best = INFINITY; int bx = -1, by = -1;
    for (int yy = 0; yy < NB1_H; yy++) for (int xx = 0; xx < NB1_W; xx++) {
        if (ids[yy * NB1_W + xx] != out->a.item) continue;
        double dx = xx + .5 - out->a.sx, dy = yy + .5 - out->a.sy, d2 = dx * dx + dy * dy;
        if (d2 < best) { best = d2; bx = xx; by = yy; }
    }
    if (bx < 0 || best > radius * radius) { out->hidden = 1; return 1; }
    out->a.sx = bx + .5; out->a.sy = by + .5; out->snapped = 1; return 1;
}

/* ---------------------------------------------------------------- gun registers */
#define CAL_X 0x26
#define CAL_Y 0x0f
Nb1GunRegs nb1_gun_regs(double sx, double sy) {
    int x = (int)nearbyint(sx), y = (int)nearbyint(sy);
    x = x < 0 ? 0 : x > 287 ? 287 : x; y = y < 1 ? 1 : y > 223 ? 223 : y;
    long num = (long)x * 16384; int xraw = CAL_X + (int)((num + 0x4ecf - 1) / 0x4ecf);          /* ceil; x >= 0 */
    while ((((long)(xraw - 1 - CAL_X)) * 0x4ecf) >> 14 >= x && xraw - 1 >= CAL_X) xraw--;
    Nb1GunRegs r = { xraw, xraw, CAL_Y + y, CAL_Y + y }; return r;
}
int nb1_regs_to_pixel(Nb1GunRegs r, int *X, int *Y) {
    if (r.y1 <= 0x0f) return 0;
    int x1 = r.x1 + (r.x1 < 0x26 ? 0x100 : 0), x2 = r.x2 + (r.x2 < 0x26 ? 0x100 : 0), d1 = x2;
    if (x2 > x1 && (x2 - x1) >= 8) d1 = x1;
    int xx = d1 < CAL_X ? 0 : (int)((((long)(d1 - CAL_X)) * 0x4ecf) >> 14); if (xx > 0x11f) xx = 0x11f;
    int yy = ((r.y1 + r.y2) >> 1) - CAL_Y; if (yy < 0) yy = 0;
    *X = xx; *Y = yy; return 1;
}
