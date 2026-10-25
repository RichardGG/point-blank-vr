/* Depth-table edits: texture replacement, punch-outs and splits (port of apply_edits in oracle/depthscene.py), plus the
 * deterministic hole fill and the polygon rasteriser they use. Integer / float32 arithmetic only, so the C port and the
 * Python oracle agree exactly. */
#include "nb1_internal.h"

/* ---------------------------------------------------------------- polygon fill (float32 scanline, see fill_polygon in depthscene.py) */
static int round_half_up(float v) { double d = v; return (int)(d >= 0 ? floor(d + 0.5) : -floor(fabs(d) + 0.5)); }
static int round_half_down(float v) { double d = v; return (int)(d >= 0 ? ceil(d - 0.5) : -ceil(fabs(d) - 0.5)); }
static int cmp_float(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }

void nb1_fill_polygon(const int *xy, int n, int w, int h, uint8_t *mask) {
    if (n <= 0) return;
    typedef struct { int x0, y0, x1, y1, ymin, ymax; float dx; } Edge;
    Edge *e = malloc(n * sizeof *e), *act = malloc(n * sizeof *e); int na = 0, pymin = 1 << 30, pymax = -(1 << 30);
    for (int i = 0; i < n; i++) {
        int x0 = xy[2 * i], y0 = xy[2 * i + 1], x1 = xy[2 * ((i + 1) % n)], y1 = xy[2 * ((i + 1) % n) + 1];
        e[i] = (Edge){ x0, y0, x1, y1, y0 < y1 ? y0 : y1, y0 > y1 ? y0 : y1, 0 };
        if (y0 != y1) e[i].dx = (float)(x1 - x0) / (float)(y1 - y0);
        if (e[i].ymin < pymin) pymin = e[i].ymin;
        if (e[i].ymax > pymax) pymax = e[i].ymax;
    }
    for (int i = 0; i < n; i++) {
        if (e[i].ymin == e[i].ymax) {                          /* horizontal edge: drawn directly */
            int y = e[i].ymin, xa = e[i].x0 < e[i].x1 ? e[i].x0 : e[i].x1, xb = e[i].x0 < e[i].x1 ? e[i].x1 : e[i].x0;
            if (y >= 0 && y < h) {
                if (xa < 0) xa = 0;
                if (xb > w - 1) xb = w - 1;
                for (int x = xa; x <= xb; x++) mask[y * w + x] = 1;
            }
        } else act[na++] = e[i];
    }
    float *xx = malloc((na * 2 + 2) * sizeof *xx);
    for (int y = pymin > 0 ? pymin : 0; y <= (pymax < h - 1 ? pymax : h - 1); y++) {
        int j = 0;
        for (int i = 0; i < na; i++) {
            if (y >= act[i].ymin && y <= act[i].ymax) xx[j++] = (float)(y - act[i].y0) * act[i].dx + (float)act[i].x0;
            if (y == act[i].ymax && y < pymax) { xx[j] = xx[j - 1]; j++; }
        }
        qsort(xx, j, sizeof *xx, cmp_float);
        for (int i = 1; i < j; i += 2) {
            int xa = round_half_up(xx[i - 1]), xb = round_half_down(xx[i]);
            if (xa < 0) xa = 0;
            if (xb > w - 1) xb = w - 1;
            for (int x = xa; x <= xb; x++) mask[y * w + x] = 1;
        }
    }
    free(xx); free(e); free(act);
}

/* ---------------------------------------------------------------- hole fill (layers from the edge inwards, integer weights) */
void nb1_inpaint(uint8_t *img, const uint8_t *hole, const uint8_t *alpha, int w, int h) {
    size_t N = (size_t)w * h; uint8_t *rem = calloc(N, 1), *known = calloc(N, 1); int y0 = h, y1 = -1, x0 = w, x1 = -1;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        size_t k = (size_t)y * w + x; int eff = hole[k] && alpha[k] > 0;
        rem[k] = (uint8_t)eff; known[k] = alpha[k] > 0 && !eff;
        if (eff) {
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
        }
    }
    if (y1 >= 0) {
        static int wt[5][5]; for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) wt[dy + 2][dx + 2] = (dx || dy) ? 12 / (dx * dx + dy * dy) : 0;
        uint8_t *val = malloc(N * 3), *cand = malloc(N);
        for (;;) {
            int any = 0;
            memset(cand, 0, N);
            for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
                size_t k = (size_t)y * w + x; if (!rem[k]) continue;
                int near = 0;
                for (int dy = -1; dy <= 1 && !near; dy++) for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    int yy = y + dy, xx = x + dx;
                    if (yy >= 0 && yy < h && xx >= 0 && xx < w && known[(size_t)yy * w + xx]) { near = 1; break; }
                }
                if (!near) continue;
                long S[3] = { 0, 0, 0 }, D = 0;
                for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) {
                    int yy = y + dy, xx = x + dx, wv = wt[dy + 2][dx + 2];
                    if (!wv || yy < 0 || yy >= h || xx < 0 || xx >= w) continue;
                    size_t q = (size_t)yy * w + xx; if (!known[q]) continue;
                    for (int c = 0; c < 3; c++) S[c] += (long)wv * img[q * 3 + c];
                    D += wv;
                }
                for (int c = 0; c < 3; c++) val[k * 3 + c] = (uint8_t)((2 * S[c] + D) / (2 * D));
                cand[k] = 1; any = 1;
            }
            if (!any) break;
            int left = 0;
            for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
                size_t k = (size_t)y * w + x;
                if (cand[k]) { memcpy(img + k * 3, val + k * 3, 3); known[k] = 1; rem[k] = 0; }
                left |= rem[k];
            }
            if (!left) break;
        }
        free(val); free(cand);
    }
    free(rem); free(known);
}

/* ---------------------------------------------------------------- edit helpers */
static void to_custom(Nb1Item *it, uint8_t rgb[0x2000][3]) {
    if (it->cust) return;
    it->cust = calloc((size_t)it->w * it->h, 3);
    for (size_t k = 0; k < (size_t)it->w * it->h; k++)
        if (it->pen[k] >= 0) { memcpy(it->cust + k * 3, rgb[it->pen[k] & 0x1fff], 3); it->pen[k] = -4; }
}

/* Fill hole pixels of img (RGB w*h): spec clone offsets first (copy from the first offset that lands on opaque, non-hole art), then the
 * layered fill for what is left. `only` (or NULL) restricts which hole pixels are filled; sources never come from `hole`. */
static void fill_apply(uint8_t *img, const uint8_t *hole, const uint8_t *alpha, const uint8_t *only, const Fill *f, int w, int h) {
    size_t N = (size_t)w * h; uint8_t *todo = malloc(N);
    for (size_t k = 0; k < N; k++) todo[k] = (uint8_t)(((only ? only[k] : hole[k]) != 0) && alpha[k] > 0);
    if (f->type == 2) {
        for (int c = 0; c < f->nclone; c++) {
            int dx = f->off[c][0], dy = f->off[c][1];
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                size_t k = (size_t)y * w + x; if (!todo[k]) continue;
                int sy = y + dy, sx = x + dx; if (sy < 0 || sy >= h || sx < 0 || sx >= w) continue;
                size_t q = (size_t)sy * w + sx; if (!(alpha[q] > 0 && !hole[q])) continue;
                memcpy(img + k * 3, img + q * 3, 3); todo[k] = 0;
            }
        }
    }
    nb1_inpaint(img, todo, alpha, w, h);
    free(todo);
}

static void dilate_rect(uint8_t *m, int w, int h, int g) {
    uint8_t *src = malloc((size_t)w * h); memcpy(src, m, (size_t)w * h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        if (m[(size_t)y * w + x]) continue;
        int hit = 0;
        for (int dy = -g; dy <= g && !hit; dy++) for (int dx = -g; dx <= g; dx++) {
            int yy = y + dy, xx = x + dx; if (yy >= 0 && yy < h && xx >= 0 && xx < w && src[(size_t)yy * w + xx]) { hit = 1; break; }
        }
        m[(size_t)y * w + x] = (uint8_t)hit;
    }
    free(src);
}

static void full_path(const Nb1Table *t, const char *name, char *out, size_t cap) {
    if (name[0] == '/') snprintf(out, cap, "%s", name); else snprintf(out, cap, "%s%s", t->base, name);
}

/* boolean mask over the item's texture for a split part */
static uint8_t *region_mask(const Nb1Item *it, const Part *p, const Nb1Table *tab) {
    size_t N = (size_t)it->w * it->h; uint8_t *m = calloc(N, 1);
    if (p->region == 1) {
        int ya = it->y0 > p->rect[1] ? 0 : p->rect[1] - it->y0, yb = p->rect[3] - it->y0; if (yb < 0) yb = 0; if (yb > it->h) yb = it->h;
        int xa = it->x0 > p->rect[0] ? 0 : p->rect[0] - it->x0, xb = p->rect[2] - it->x0; if (xb < 0) xb = 0; if (xb > it->w) xb = it->w;
        for (int y = ya; y < yb; y++) for (int x = xa; x < xb; x++) m[(size_t)y * it->w + x] = 1;
    } else if (p->region == 2) {
        uint8_t *mm = calloc((size_t)NB1_W * NB1_H, 1);
        for (int r = 0; r < p->nrings; r++) nb1_fill_polygon(p->ring_xy[r], p->ring_n[r], NB1_W, NB1_H, mm);
        for (int y = 0; y < it->h; y++) {
            int sy = it->y0 + y; if (sy < 0 || sy >= NB1_H) continue;
            for (int x = 0; x < it->w; x++) { int sx = it->x0 + x; if (sx >= 0 && sx < NB1_W) m[(size_t)y * it->w + x] = mm[sy * NB1_W + sx]; }
        }
        free(mm);
    } else if (p->region == 3) {
        char path[768]; int pw, ph; uint8_t *a; full_path(tab, p->mask, path, sizeof path);
        if (nb1_png_load(path, &pw, &ph, &a) == 0) {
            for (int y = 0; y < it->h; y++) {
                int sy = it->y0 + y; if (sy < 0 || sy >= ph) continue;
                for (int x = 0; x < it->w; x++) {
                    int sx = it->x0 + x; if (sx < 0 || sx >= pw) continue;
                    const uint8_t *q = a + ((size_t)sy * pw + sx) * 4;
                    m[(size_t)y * it->w + x] = p->mask_white ? (q[3] > 0 && (q[0] + q[1] + q[2]) > 0) : q[3] > 0;
                }
            }
            free(a);
        } else fprintf(stderr, "nb1: cannot read mask %s\n", path);
    }
    return m;
}

/* ---------------------------------------------------------------- the edits */
static void edit_texture(Nb1Item *it, const Nb1Table *tab, const char *name) {
    char path[768]; int pw, ph; uint8_t *a; full_path(tab, name, path, sizeof path);
    if (nb1_png_load(path, &pw, &ph, &a)) { fprintf(stderr, "nb1: cannot read texture %s\n", path); return; }
    size_t N = (size_t)it->w * it->h; free(it->cust); it->cust = malloc(N * 3);
    for (int y = 0; y < it->h; y++) for (int x = 0; x < it->w; x++) {          /* nearest-neighbour resize when the size differs */
        int sx = x, sy = y;
        if (pw != it->w || ph != it->h) { sx = (int)(((2L * x + 1) * pw) / (2L * it->w)); sy = (int)(((2L * y + 1) * ph) / (2L * it->h)); }   /* exact integer centre sampling */
        const uint8_t *q = a + ((size_t)sy * pw + sx) * 4; size_t k = (size_t)y * it->w + x;
        memcpy(it->cust + k * 3, q, 3); memcpy(it->tex + k * 4, q, 4); it->pen[k] = q[3] > 0 ? -4 : -1;
    }
    free(a);
}

static void edit_punch(Nb1Item *items, int n, int a, const Edit *e, uint8_t rgb[0x2000][3]) {
    Nb1Item *it = &items[a]; size_t N = (size_t)it->w * it->h; uint8_t *opaque = malloc(N);
    for (size_t k = 0; k < N; k++) opaque[k] = it->tex[k * 4 + 3] > 0;
    if (e->grow) dilate_rect(opaque, it->w, it->h, e->grow);
    for (int b = 0; b < n; b++) {
        Nb1Item *tgt = &items[b]; if (b == a || !nb1_match_item(&e->from, tgt)) continue;
        int x0 = it->x0 > tgt->x0 ? it->x0 : tgt->x0, y0 = it->y0 > tgt->y0 ? it->y0 : tgt->y0;
        int x1 = it->x0 + it->w < tgt->x0 + tgt->w ? it->x0 + it->w : tgt->x0 + tgt->w, y1 = it->y0 + it->h < tgt->y0 + tgt->h ? it->y0 + it->h : tgt->y0 + tgt->h;
        if (x1 <= x0 || y1 <= y0) continue;
        size_t TN = (size_t)tgt->w * tgt->h; uint8_t *hole = calloc(TN, 1); int any = 0, cnt = 0;
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
            size_t tk = (size_t)(y - tgt->y0) * tgt->w + (x - tgt->x0);
            if (opaque[(size_t)(y - it->y0) * it->w + (x - it->x0)] && tgt->tex[tk * 4 + 3] > 0) { hole[tk] = 1; any = 1; cnt++; }
        }
        if (!any) { free(hole); continue; }
        to_custom(tgt, rgb);
        uint8_t *alpha = malloc(TN); for (size_t k = 0; k < TN; k++) alpha[k] = tgt->tex[k * 4 + 3];
        if (e->pfill.type != 1) fill_apply(tgt->cust, hole, alpha, NULL, &e->pfill, tgt->w, tgt->h);
        else for (size_t k = 0; k < TN; k++) if (hole[k]) { tgt->tex[k * 4 + 3] = 0; tgt->pen[k] = -1; }
        tgt->erased += cnt;
        for (size_t k = 0; k < TN; k++) memcpy(tgt->tex + k * 4, tgt->cust + k * 3, 3);
        free(hole); free(alpha);
    }
    free(opaque);
}

static int edit_split(Nb1Item *items, int n, int cap, int a, const Edit *e, const Nb1Table *tab, uint8_t rgb[0x2000][3]) {
    to_custom(&items[a], rgb);
    Nb1Item *it = &items[a]; int w = it->w, h = it->h; size_t N = (size_t)w * h;
    uint8_t *orig = malloc(N * 3), *alpha0 = malloc(N), *remaining = malloc(N), *uni = calloc(N, 1);
    memcpy(orig, it->cust, N * 3);
    for (size_t k = 0; k < N; k++) alpha0[k] = remaining[k] = it->tex[k * 4 + 3] > 0;
    uint8_t **cuts = calloc(e->nparts ? e->nparts : 1, sizeof *cuts); const Part **cutpart = calloc(e->nparts ? e->nparts : 1, sizeof *cutpart); int ncut = 0;
    for (int pi = 0; pi < e->nparts; pi++) {
        const Part *part = &e->parts[pi]; uint8_t *m = region_mask(it, part, tab); int any = 0;
        for (size_t k = 0; k < N; k++) { m[k] = m[k] && remaining[k]; any |= m[k]; }
        if (!any) { free(m); continue; }
        for (size_t k = 0; k < N; k++) if (m[k]) { remaining[k] = 0; uni[k] = 1; }
        cuts[ncut] = m; cutpart[ncut++] = part;
        int cy0 = h, cy1 = -1, cx0 = w, cx1 = -1;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (m[(size_t)y * w + x]) {
            if (y < cy0) cy0 = y;
            if (y > cy1) cy1 = y;
            if (x < cx0) cx0 = x;
            if (x > cx1) cx1 = x;
        }
        cy1++; cx1++;
        if (n >= cap) { fprintf(stderr, "nb1: out of item slots while splitting\n"); continue; }
        Nb1Item *c = &items[n++]; memset(c, 0, sizeof *c);
        c->kind = it->kind; c->id = it->id; c->pri = it->pri; c->order = it->order; c->x0 = it->x0 + cx0; c->y0 = it->y0 + cy0; c->w = cx1 - cx0; c->h = cy1 - cy0;
        c->has_key = it->has_key; c->key = it->key; c->pal = it->pal; c->ncols = it->ncols; c->nrows = it->nrows; c->listidx = it->listidx; c->rule = -1;
        c->has_part = part->has_name; snprintf(c->part, sizeof c->part, "%s", part->name); c->hud = it->hud;
        size_t CN = (size_t)c->w * c->h; c->tex = calloc(CN, 4); c->pen = malloc(CN * 4); c->cust = calloc(CN, 3);
        for (int y = 0; y < c->h; y++) for (int x = 0; x < c->w; x++) {
            size_t ck = (size_t)y * c->w + x, pk = (size_t)(y + cy0) * w + (x + cx0);
            if (m[pk]) { memcpy(c->tex + ck * 4, orig + pk * 3, 3); c->tex[ck * 4 + 3] = 255; memcpy(c->cust + ck * 3, orig + pk * 3, 3); c->pen[ck] = -4; }
            else c->pen[ck] = -1;
        }
        it = &items[a];                                          /* items[] did not move, but keep the pointer honest */
    }
    /* the parent keeps plausible art behind each piece (visible when the head moves) */
    uint8_t *fillimg = malloc(N * 3); memcpy(fillimg, orig, N * 3);
    for (int c = 0; c < ncut; c++) if (cutpart[c]->fill.type != 1) fill_apply(fillimg, uni, alpha0, cuts[c], &cutpart[c]->fill, w, h);
    memcpy(it->cust, fillimg, N * 3);
    for (size_t k = 0; k < N; k++) memcpy(it->tex + k * 4, fillimg + k * 3, 3);
    for (int c = 0; c < ncut; c++) if (cutpart[c]->fill.type == 1) for (size_t k = 0; k < N; k++) if (cuts[c][k]) it->tex[k * 4 + 3] = 0;
    for (size_t k = 0; k < N; k++) it->pen[k] = it->tex[k * 4 + 3] > 0 ? -4 : -1;
    for (int c = 0; c < ncut; c++) free(cuts[c]);
    free(cuts); free(cutpart); free(fillimg); free(orig); free(alpha0); free(remaining); free(uni);
    return n;
}

int nb1_apply_edits(Nb1Item *items, int n, int cap, const Nb1Table *tab, uint8_t rgb[0x2000][3]) {
    for (int ei = 0; ei < tab->nedits; ei++) {
        const Edit *e = &tab->edits[ei];
        int ntg = 0; int *targets = malloc((n + 1) * sizeof *targets);
        for (int a = 0; a < n; a++) if (nb1_match_item(&e->m, &items[a])) targets[ntg++] = a;   /* fixed before the edit runs: new pieces are not targets of this edit */
        for (int ti = 0; ti < ntg; ti++) {
            int a = targets[ti];
            if (e->has_texture) edit_texture(&items[a], tab, e->texture);
            if (e->has_punch) edit_punch(items, n, a, e, rgb);
            if (e->nparts) n = edit_split(items, n, cap, a, e, tab, rgb);
        }
        free(targets);
    }
    return n;
}
