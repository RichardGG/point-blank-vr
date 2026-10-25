/* Depth table: JSON parsing (rules, decals, edits) and item matching. */
#include "nb1_internal.h"

static int num_list(const Json *j, int *out, int max) {
    if (!j) return 0;
    if (j->t == J_NUM) { out[0] = (int)j->num; return 1; }
    int n = 0; for (int i = 0; i < j->n && n < max; i++) if (j->kid[i]->t == J_NUM) out[n++] = (int)j->kid[i]->num;
    return n;
}

static void parse_match(const Json *mj, Match *m) {
    memset(m, 0, sizeof *m); m->kind = -1;
    for (int k = 0; mj && mj->t == J_OBJ && k < mj->n; k++) {
        const char *key = mj->key[k]; const Json *v = mj->kid[k];
        if (!strcmp(key, "kind")) m->kind = (v->t == J_STR && !strcmp(v->str, "tilemap")) ? NB1_TILEMAP : (v->t == J_STR && !strcmp(v->str, "sprite")) ? NB1_SPRITE : NB1_MASK;
        else if (!strcmp(key, "layer")) m->nlayer = num_list(v, m->layer, 16);
        else if (!strcmp(key, "pri")) m->npri = num_list(v, m->pri, 16);
        else if (!strcmp(key, "pal")) m->npal = num_list(v, m->pal, 16);
        else if (!strcmp(key, "id")) m->nid = num_list(v, m->id, 32);
        else if (!strcmp(key, "code") && v->n >= 2) { m->has_code = 1; m->code_lo = (int)v->kid[0]->num; m->code_hi = (int)v->kid[1]->num; }
        else if (!strcmp(key, "size") && v->n >= 2) { m->has_size = 1; m->sw = (int)v->kid[0]->num; m->sh = (int)v->kid[1]->num; }
        else if (!strcmp(key, "part")) {
            if (v->t == J_NULL) m->part_mode = 1;
            else if (v->t == J_STR) { m->part_mode = 2; snprintf(m->part, sizeof m->part, "%s", v->str); }
        }
        else if (!strcmp(key, "center_x") && v->n >= 2) { m->has_cx = 1; m->cx0 = v->kid[0]->num; m->cx1 = v->kid[1]->num; }
        else if (!strcmp(key, "center_y") && v->n >= 2) { m->has_cy = 1; m->cy0 = v->kid[0]->num; m->cy1 = v->kid[1]->num; }
    }
}

static void parse_fill(const Json *v, Fill *f) {
    memset(f, 0, sizeof *f);
    if (!v) return;
    if (v->t == J_STR && !strcmp(v->str, "none")) f->type = 1;
    else if (v->t == J_OBJ) {
        const Json *c = json_get(v, "clone");
        if (c && c->t == J_ARR) {
            f->type = 2;
            for (int i = 0; i < c->n && f->nclone < 8; i++) if (c->kid[i]->n >= 2) { f->off[f->nclone][0] = (int)c->kid[i]->kid[0]->num; f->off[f->nclone][1] = (int)c->kid[i]->kid[1]->num; f->nclone++; }
        }
    }
}

static void parse_part(const Json *pj, Part *p) {
    memset(p, 0, sizeof *p); p->mask_white = 1;
    const Json *v;
    if ((v = json_get(pj, "part")) && v->t == J_STR) { p->has_name = 1; snprintf(p->name, sizeof p->name, "%s", v->str); }
    if ((v = json_get(pj, "rect")) && v->n >= 4) { p->region = 1; for (int i = 0; i < 4; i++) p->rect[i] = (int)v->kid[i]->num; }
    else if ((v = json_get(pj, "poly")) && v->t == J_ARR && v->n > 0) {
        p->region = 2;
        const Json *first = v->kid[0]->kid && v->kid[0]->n > 0 ? v->kid[0]->kid[0] : NULL;
        int nested = first && first->t == J_ARR;               /* [[[x,y],...], ...] vs [[x,y],...] */
        p->nrings = nested ? v->n : 1; p->ring_n = calloc(p->nrings, sizeof(int)); p->ring_xy = calloc(p->nrings, sizeof(int *));
        for (int r = 0; r < p->nrings; r++) {
            const Json *ring = nested ? v->kid[r] : v;
            p->ring_n[r] = ring->n; p->ring_xy[r] = calloc(ring->n * 2 + 2, sizeof(int));
            for (int i = 0; i < ring->n; i++) if (ring->kid[i]->n >= 2) { p->ring_xy[r][2 * i] = (int)ring->kid[i]->kid[0]->num; p->ring_xy[r][2 * i + 1] = (int)ring->kid[i]->kid[1]->num; }
        }
    }
    else if ((v = json_get(pj, "mask")) && v->t == J_STR) { p->region = 3; snprintf(p->mask, sizeof p->mask, "%s", v->str); }
    if ((v = json_get(pj, "mask_white"))) p->mask_white = v->num != 0;
    parse_fill(json_get(pj, "fill"), &p->fill);
}

static Nb1Table *table_alloc(void) {
    Nb1Table *t = calloc(1, sizeof *t); t->far = 80; t->z_front = 4; t->z_back = 14; t->hud_z = 2; t->slot_gap = 0.0005;
    t->ndecals = 1; t->decal_lo[0] = 0xee5; t->decal_hi[0] = 0xee7; return t;
}
Nb1Table *nb1_table_default(void) {
    Nb1Table *t = table_alloc(); t->rules = calloc(2, sizeof(Rule)); t->nrules = 2;
    Rule *r = &t->rules[0]; r->m.kind = NB1_TILEMAP; r->m.nlayer = 2; r->m.layer[0] = 4; r->m.layer[1] = 5; r->hud = 1;
    r = &t->rules[1]; r->m.kind = NB1_SPRITE; r->m.has_code = 1; r->m.code_lo = 3799; r->m.code_hi = 3860; r->hud = 1;    /* timer digits/ring and the ammo/heart pill */
    return t;
}

Nb1Table *nb1_table_load(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *txt = malloc(n + 1); if (fread(txt, 1, n, f) != (size_t)n) { fclose(f); free(txt); return NULL; } txt[n] = 0; fclose(f);
    Json *j = json_parse(txt); free(txt); if (!j) return NULL;
    Nb1Table *t = table_alloc();
    const char *slash = nb1_last_sep(path); if (slash) { size_t l = (size_t)(slash - path) + 1; if (l >= sizeof t->base) l = sizeof t->base - 1; memcpy(t->base, path, l); t->base[l] = 0; }
    const Json *d = json_get(j, "defaults");
    if (d) {
        const Json *v;
        if ((v = json_get(d, "far"))) t->far = v->num;
        if ((v = json_get(d, "z_front"))) t->z_front = v->num;
        if ((v = json_get(d, "z_back"))) t->z_back = v->num;
        if ((v = json_get(d, "hud_z"))) t->hud_z = v->num;
        if ((v = json_get(d, "slot_gap"))) t->slot_gap = v->num;
    }
    const Json *dc = json_get(j, "decals");
    if (dc && dc->t == J_ARR) {
        t->ndecals = 0;
        for (int i = 0; i < dc->n && i < 16; i++) if (dc->kid[i]->n >= 2) { t->decal_lo[t->ndecals] = (int)dc->kid[i]->kid[0]->num; t->decal_hi[t->ndecals++] = (int)dc->kid[i]->kid[1]->num; }
    }
    const Json *ed = json_get(j, "edits");
    if (ed && ed->t == J_ARR) {
        t->edits = calloc(ed->n ? ed->n : 1, sizeof(Edit)); t->nedits = ed->n;
        for (int i = 0; i < ed->n; i++) {
            const Json *ej = ed->kid[i]; Edit *e = &t->edits[i]; const Json *v;
            parse_match(json_get(ej, "match"), &e->m);
            if ((v = json_get(ej, "texture")) && v->t == J_STR) { e->has_texture = 1; snprintf(e->texture, sizeof e->texture, "%s", v->str); }
            if ((v = json_get(ej, "punch")) && v->t == J_OBJ) {
                e->has_punch = 1; parse_match(json_get(v, "from"), &e->from);
                const Json *g = json_get(v, "grow"); e->grow = g ? (int)g->num : 0;
                parse_fill(json_get(v, "fill"), &e->pfill);
            }
            if ((v = json_get(ej, "split")) && v->t == J_ARR) {
                e->nparts = v->n; e->parts = calloc(v->n ? v->n : 1, sizeof(Part));
                for (int k = 0; k < v->n; k++) parse_part(v->kid[k], &e->parts[k]);
            }
        }
    }
    const Json *rs = json_get(j, "rules");
    if (rs && rs->t == J_ARR) {
        t->rules = calloc(rs->n ? rs->n : 1, sizeof(Rule)); t->nrules = rs->n;
        for (int i = 0; i < rs->n; i++) {
            const Json *rj = rs->kid[i]; Rule *r = &t->rules[i]; const Json *v;
            parse_match(json_get(rj, "match"), &r->m);
            if ((v = json_get(rj, "hud"))) r->hud = v->num != 0;
            if ((v = json_get(rj, "z"))) { r->has_z = 1; r->z = v->num; }
            if ((v = json_get(rj, "plane")) && v->t == J_OBJ && v->n == 1) {
                int ax = v->key[0][0] - 'x'; if (ax >= 0 && ax < 3) { r->has_plane = 1; r->pn[ax] = 1; r->pd = v->kid[0]->num; }
            }
            if ((v = json_get(rj, "floor_contact"))) { r->has_fc = 1; r->fc = v->num; }
            if ((v = json_get(rj, "ceiling_contact"))) { r->has_cc = 1; r->cc = v->num; }
            if ((v = json_get(rj, "zoom_depth"))) { r->has_zd = 1; r->zd = v->num; r->zlo = 1.5; r->zhi = 40.0; }
            if (r->has_zd && (v = json_get(rj, "zoom_clamp")) && v->n >= 2) { r->zlo = v->kid[0]->num; r->zhi = v->kid[1]->num; }
            if ((v = json_get(rj, "tilt_x"))) r->tilt_x = v->num;
            if ((v = json_get(rj, "tilt_y"))) r->tilt_y = v->num;
        }
    }
    json_free(j); return t;
}

void nb1_table_free(Nb1Table *t) {
    if (!t) return;
    for (int i = 0; i < t->nedits; i++) {
        for (int k = 0; k < t->edits[i].nparts; k++) { Part *p = &t->edits[i].parts[k]; for (int r = 0; r < p->nrings; r++) free(p->ring_xy[r]); free(p->ring_xy); free(p->ring_n); }
        free(t->edits[i].parts);
    }
    free(t->edits); free(t->rules); free(t);
}
double nb1_table_far(const Nb1Table *t) { return t->far; }

static int in_list(const int *l, int n, int v) { for (int i = 0; i < n; i++) if (l[i] == v) return 1; return 0; }
int nb1_match_item(const Match *m, const Nb1Item *it) {
    if (m->kind >= 0 && it->kind != m->kind) return 0;
    if (m->nlayer && !(it->kind == NB1_TILEMAP && in_list(m->layer, m->nlayer, it->id))) return 0;
    if (m->npri && !in_list(m->pri, m->npri, it->pri)) return 0;
    if (m->npal && !in_list(m->pal, m->npal, it->pal)) return 0;
    if (m->has_code && !(it->has_key && m->code_lo <= it->key && it->key <= m->code_hi)) return 0;
    if (m->has_size && !(it->ncols == m->sw && it->nrows == m->sh)) return 0;
    if (m->part_mode == 1 && it->has_part) return 0;
    if (m->part_mode == 2 && !(it->has_part && !strcmp(it->part, m->part))) return 0;
    if (m->has_cx) { double c = it->x0 + it->w / 2.0; if (!(m->cx0 <= c && c < m->cx1)) return 0; }
    if (m->has_cy) { double c = it->y0 + it->h / 2.0; if (!(m->cy0 <= c && c < m->cy1)) return 0; }
    if (m->nid && !in_list(m->id, m->nid, it->id)) return 0;
    return 1;
}

/* ---------------------------------------------------------------- automatic selection */
typedef struct { char name[64]; Nb1Table *table; int ncodes_all, ncodes_any, nart; int *codes_all, *codes_any, *art; double min_sim; } SetRule;
typedef struct { int id; int n; uint32_t *keys; } Ref;          /* keys: layer << 16 | tile code, sorted */
struct Nb1TableSet { int nrules; SetRule *rules; int nrefs; Ref *refs; };

static int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static int *int_array(const Json *j, int *n) {
    *n = 0; if (!j || j->t != J_ARR) return NULL;
    int *a = calloc(j->n ? j->n : 1, sizeof *a); for (int i = 0; i < j->n; i++) a[(*n)++] = (int)j->kid[i]->num; return a;
}

Nb1TableSet *nb1_tableset_load(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *txt = malloc(n + 1); if (fread(txt, 1, n, f) != (size_t)n) { fclose(f); free(txt); return NULL; } txt[n] = 0; fclose(f);
    Json *j = json_parse(txt); free(txt); if (!j || j->t != J_ARR) { json_free(j); return NULL; }
    char base[512] = ""; const char *slash = nb1_last_sep(path); if (slash) { size_t l = (size_t)(slash - path) + 1; if (l >= sizeof base) l = sizeof base - 1; memcpy(base, path, l); base[l] = 0; }
    Nb1TableSet *ts = calloc(1, sizeof *ts); ts->nrules = j->n; ts->rules = calloc(j->n ? j->n : 1, sizeof(SetRule));
    for (int i = 0; i < j->n; i++) {
        const Json *rj = j->kid[i], *w = json_get(rj, "when"), *v; SetRule *r = &ts->rules[i];
        if ((v = json_get(rj, "name")) && v->t == J_STR) snprintf(r->name, sizeof r->name, "%s", v->str);
        r->codes_all = int_array(json_get(w, "codes_all"), &r->ncodes_all); r->codes_any = int_array(json_get(w, "codes_any"), &r->ncodes_any);
        r->art = int_array(json_get(w, "art_scenes"), &r->nart); r->min_sim = (v = json_get(w, "min_similarity")) ? v->num : 0.8;
        if ((v = json_get(rj, "table")) && v->t == J_STR) { char tp[1100]; snprintf(tp, sizeof tp, "%s%s", base, v->str); r->table = nb1_table_load(tp); if (!r->table) fprintf(stderr, "nb1: cannot load table %s\n", tp); }
    }
    json_free(j);
    char rp[1100]; snprintf(rp, sizeof rp, "%sscene_refs.json", base);
    FILE *rf = fopen(rp, "rb");
    if (rf) {
        fseek(rf, 0, SEEK_END); long rn = ftell(rf); fseek(rf, 0, SEEK_SET);
        char *rt = malloc(rn + 1); if (fread(rt, 1, rn, rf) != (size_t)rn) rn = 0; rt[rn] = 0; fclose(rf);
        Json *rj = json_parse(rt); free(rt);
        if (rj && rj->t == J_OBJ) {
            ts->nrefs = rj->n; ts->refs = calloc(rj->n ? rj->n : 1, sizeof(Ref));
            for (int i = 0; i < rj->n; i++) {
                Ref *r = &ts->refs[i]; r->id = atoi(rj->key[i]); const Json *per = rj->kid[i]; int cap = 0;
                for (int k = 0; k < per->n; k++) cap += per->kid[k]->n;
                r->keys = malloc((cap ? cap : 1) * sizeof(uint32_t));
                for (int k = 0; k < per->n; k++) { uint32_t L = (uint32_t)atoi(per->key[k]); for (int c = 0; c < per->kid[k]->n; c++) r->keys[r->n++] = L << 16 | (uint32_t)per->kid[k]->kid[c]->num; }
                qsort(r->keys, r->n, sizeof(uint32_t), cmp_u32);
            }
        }
        json_free(rj);
    }
    return ts;
}

void nb1_tableset_free(Nb1TableSet *ts) {
    if (!ts) return;
    for (int i = 0; i < ts->nrules; i++) { nb1_table_free(ts->rules[i].table); free(ts->rules[i].codes_all); free(ts->rules[i].codes_any); free(ts->rules[i].art); }
    for (int i = 0; i < ts->nrefs; i++) free(ts->refs[i].keys);
    free(ts->rules); free(ts->refs); free(ts);
}

/* the (layer, tile code) pairs of the enabled scrolling layers, sorted and unique */
static uint32_t *art_keys(const Nb1State *st, int *n_out) {
    static const int off[4] = { 0x0000, 0x1000, 0x2000, 0x3000 };
    uint32_t *k = malloc(4 * 4096 * sizeof *k); int n = 0;
    for (int L = 0; L < 4; L++) {
        if (st->ctl[0x10 + L] & 8) continue;
        int start = n; for (int i = 0; i < 4096; i++) k[n++] = (uint32_t)L << 16 | st->vram[off[L] + i];
        qsort(k + start, 4096, sizeof(uint32_t), cmp_u32);
        int w = start; for (int i = start; i < n; i++) if (i == start || k[i] != k[i - 1]) k[w++] = k[i];
        n = w;
    }
    *n_out = n; return k;
}
static double jaccard(const uint32_t *a, int na, const uint32_t *b, int nb) {
    int i = 0, j = 0, inter = 0;
    while (i < na && j < nb) { if (a[i] == b[j]) { inter++; i++; j++; } else if (a[i] < b[j]) i++; else j++; }
    int uni = na + nb - inter; return uni ? (double)inter / uni : 0.0;
}

const Nb1Table *nb1_tableset_select(const Nb1TableSet *ts, const Nb1State *st, const Nb1State *sst, const char **name) {
    if (name) *name = NULL;
    if (!ts) return NULL;
    if (!sst) sst = st;
    uint32_t *keys = NULL; int nkeys = 0; int *codes = NULL; int ncodes = 0; int have_codes = 0;
    const SetRule *best = NULL; double best_sim = -1;
    for (int i = 0; i < ts->nrules; i++) {
        const SetRule *r = &ts->rules[i]; if (!r->table) continue;
        if (r->nart) {
            if (!keys) keys = art_keys(st, &nkeys);
            double sim = 0;
            for (int a = 0; a < r->nart; a++) for (int q = 0; q < ts->nrefs; q++)
                if (ts->refs[q].id == r->art[a]) { double s = jaccard(keys, nkeys, ts->refs[q].keys, ts->refs[q].n); if (s > sim) sim = s; }
            if (sim >= r->min_sim && (!best || sim > best_sim + 1e-12)) { best = r; best_sim = sim; }
        } else {
            if (!have_codes) {
                Nb1Entry *ents = malloc(256 * sizeof *ents); int ne = nb1_sprite_entries(sst, ents); codes = malloc((ne + 1) * sizeof *codes);
                for (int e = 0; e < ne; e++) if (ents[e].ntiles) { int t = ents[e].tiles[0].t; codes[ncodes++] = ((t & 0x7ff) | (sst->bank[(t >> 11) & 7] << 11)) + ents[e].offset; }
                free(ents); have_codes = 1;
            }
            int ok = 1;
            for (int a = 0; a < r->ncodes_all && ok; a++) { int f = 0; for (int c = 0; c < ncodes; c++) if (codes[c] == r->codes_all[a]) { f = 1; break; } ok = f; }
            if (ok && r->ncodes_any) { int any = 0; for (int a = 0; a < r->ncodes_any && !any; a++) for (int c = 0; c < ncodes; c++) if (codes[c] == r->codes_any[a]) { any = 1; break; } ok = any; }
            if (ok) { best = r; break; }
        }
    }
    free(keys); free(codes);
    if (!best) return NULL;
    if (name) *name = best->name;
    return best->table;
}
