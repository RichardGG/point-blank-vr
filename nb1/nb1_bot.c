#pragma GCC diagnostic ignored "-Wmisleading-indentation"
/* The auto-player: reads the arcade picture's sprite list (exactly what the game is drawing) and decides where to aim and when to fire.
 * What to shoot comes from a rules file (research/data/bot_rules.json): per sprite (tile code, palette colour) hit rates measured by shooting every sprite of every captured
 * scene (research/tools/bot/probe.lua), per scene family preferences ("only the red discs", "the bird above the lifeboat first"), and it keeps learning while it plays: a sprite
 * whose hit costs a life is avoided afterwards, one that scores is preferred. See nb1.h. */
#include "nb1.h"
#include "nb1_json.h"
#include "nb1_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { int code_lo, code_hi, col_lo, col_hi, pri_lo, pri_hi; double weight; } BSel;
typedef struct { char name[64]; int only; BSel *prefer, *avoid; int np, na; } BScene;
typedef struct { int code, col; double n, hits, pen; } BKey;
typedef struct { int code, col, pri, cx, cy, w, h; double wt; } Cand;
typedef struct { double min_size, max_size, unknown, speed, gap, hold; } BDef;

struct Nb1Bot {
    BDef d; BScene *sc; int nsc; BKey *keys; int nkeys, capkeys; BScene *cur;
    double ax, ay;                       /* where the gun points now */
    int have_aim; unsigned last_frame; int trig_left, gap_left;
    int cur_code, cur_col, cur_cx, cur_cy, have_target, target_shots; unsigned target_since;
    int shot_code, shot_col; unsigned shot_frame; int shot_pending;
    int last_score, last_lives, last_hit_visible; unsigned last_coin, last_start;
    Cand prev[64]; int nprev;
    char status[160]; unsigned nshots, nhits, npen;
};

static int sel_parse(const Json *a, BSel **out) {
    int n = a && a->t == J_ARR ? a->n : 0; *out = calloc((size_t)(n ? n : 1), sizeof **out);
    for (int i = 0; i < n; i++) {
        const Json *o = a->kid[i]; BSel *s = &(*out)[i]; s->code_lo = 0; s->code_hi = 1 << 30; s->col_lo = 0; s->col_hi = 15; s->pri_lo = 0; s->pri_hi = 15; s->weight = 3;
        const Json *v;
        if ((v = json_get(o, "code"))) { if (v->t == J_ARR && v->n == 2) { s->code_lo = (int)v->kid[0]->num; s->code_hi = (int)v->kid[1]->num; } else if (v->t == J_NUM) s->code_lo = s->code_hi = (int)v->num; }
        if ((v = json_get(o, "col"))) { if (v->t == J_ARR && v->n == 2) { s->col_lo = (int)v->kid[0]->num; s->col_hi = (int)v->kid[1]->num; } else if (v->t == J_NUM) s->col_lo = s->col_hi = (int)v->num; }
        if ((v = json_get(o, "pri"))) { if (v->t == J_ARR && v->n == 2) { s->pri_lo = (int)v->kid[0]->num; s->pri_hi = (int)v->kid[1]->num; } else if (v->t == J_NUM) s->pri_lo = s->pri_hi = (int)v->num; }
        if ((v = json_get(o, "weight"))) s->weight = v->num;
    }
    return n;
}
static int sel_match(const BSel *s, const Cand *c) { return c->code >= s->code_lo && c->code <= s->code_hi && c->col >= s->col_lo && c->col <= s->col_hi && c->pri >= s->pri_lo && c->pri <= s->pri_hi; }

static BKey *key_get(Nb1Bot *b, int code, int col, int create) {
    for (int i = 0; i < b->nkeys; i++) if (b->keys[i].code == code && b->keys[i].col == col) return &b->keys[i];
    if (!create) return NULL;
    if (b->nkeys == b->capkeys) { b->capkeys = b->capkeys ? b->capkeys * 2 : 256; b->keys = realloc(b->keys, (size_t)b->capkeys * sizeof *b->keys); }
    BKey *k = &b->keys[b->nkeys++]; memset(k, 0, sizeof *k); k->code = code; k->col = col; return k;
}

Nb1Bot *nb1_bot_new(const char *rules_path) {
    Nb1Bot *b = calloc(1, sizeof *b); b->d = (BDef){ 8, 200, 0.35, 14, 12, 4 };
    if (!rules_path) return b;
    FILE *f = fopen(rules_path, "rb"); if (!f) { fprintf(stderr, "bot: cannot read %s (using the defaults)\n", rules_path); return b; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *txt = malloc((size_t)n + 1); if (fread(txt, 1, (size_t)n, f) != (size_t)n) n = 0; txt[n] = 0; fclose(f);
    Json *j = json_parse(txt); free(txt); if (!j) { fprintf(stderr, "bot: %s is not valid JSON\n", rules_path); return b; }
    const Json *d = json_get(j, "defaults");
    if (d) { const Json *v; if ((v = json_get(d, "min_size"))) b->d.min_size = v->num; if ((v = json_get(d, "max_size"))) b->d.max_size = v->num; if ((v = json_get(d, "unknown_weight"))) b->d.unknown = v->num;
             if ((v = json_get(d, "speed"))) b->d.speed = v->num; if ((v = json_get(d, "shot_gap"))) b->d.gap = v->num; if ((v = json_get(d, "trigger_hold"))) b->d.hold = v->num; }
    const Json *ks = json_get(j, "keys");                                  /* [[code, colour, shots, hits, penalties], ...] */
    if (ks && ks->t == J_ARR) for (int i = 0; i < ks->n; i++) { const Json *r = ks->kid[i]; if (r->t != J_ARR || r->n < 5) continue; BKey *k = key_get(b, (int)r->kid[0]->num, (int)r->kid[1]->num, 1); k->n = r->kid[2]->num; k->hits = r->kid[3]->num; k->pen = r->kid[4]->num; }
    const Json *sc = json_get(j, "scenes");
    if (sc && sc->t == J_OBJ) {
        b->sc = calloc((size_t)(sc->n ? sc->n : 1), sizeof *b->sc);
        for (int i = 0; i < sc->n; i++) { BScene *s = &b->sc[b->nsc++]; snprintf(s->name, sizeof s->name, "%s", sc->key[i]); const Json *o = sc->kid[i], *v;
            s->only = (v = json_get(o, "only")) && v->num != 0; s->np = sel_parse(json_get(o, "prefer"), &s->prefer); s->na = sel_parse(json_get(o, "avoid"), &s->avoid); }
    }
    json_free(j); return b;
}
void nb1_bot_free(Nb1Bot *b) { if (!b) return; for (int i = 0; i < b->nsc; i++) { free(b->sc[i].prefer); free(b->sc[i].avoid); } free(b->sc); free(b->keys); free(b); }
void nb1_bot_set_scene(Nb1Bot *b, const char *name) {
    b->cur = NULL; if (!name) return;
    for (int i = 0; i < b->nsc; i++) if (!strcmp(b->sc[i].name, name)) { b->cur = &b->sc[i]; return; }
}
const char *nb1_bot_status(const Nb1Bot *b) { return b->status; }
int nb1_bot_save_stats(const Nb1Bot *b, const char *path) {
    FILE *f = fopen(path, "w"); if (!f) return -1; fprintf(f, "[");
    for (int i = 0; i < b->nkeys; i++) fprintf(f, "%s[%d,%d,%.0f,%.0f,%.0f]", i ? "," : "", b->keys[i].code, b->keys[i].col, b->keys[i].n, b->keys[i].hits, b->keys[i].pen);
    fprintf(f, "]\n"); fclose(f); return 0;
}

static double weight_of(Nb1Bot *b, const Cand *c) {
    double w = b->d.unknown; BKey *k = key_get(b, c->code, c->col, 0);
    if (k && k->n > 0) { w = (k->hits + 0.3) / (k->n + 1.0); if (w < 0.04) w = 0.04; }
    if (k && k->pen >= 1 && k->pen >= k->hits) return 0;                          /* shooting it cost a life more often than it scored */
    if (b->cur) {
        for (int i = 0; i < b->cur->na; i++) if (sel_match(&b->cur->avoid[i], c)) return 0;
        int pref = 0; for (int i = 0; i < b->cur->np; i++) if (sel_match(&b->cur->prefer[i], c)) { if (b->cur->prefer[i].weight > w || !pref) w = b->cur->prefer[i].weight; pref = 1; }
        if (b->cur->only && !pref) w *= 0.03;
    }
    return w;
}

static int is_hud_code(int code) { return (code >= 3799 && code <= 3860) || (code >= 6315 && code <= 6334) || code == 2289 || code == 2401 || code == 2415 || code == 2528; }

void nb1_bot_step(Nb1Bot *b, const Nb1State *st, const Nb1GameInfo *g, unsigned frame, double *sx, double *sy, int *trig, int *coin, int *start) {
    static Nb1Entry ents[256]; int ne = nb1_sprite_entries(st, ents); Cand cs[256]; int nc = 0, hit_visible = 0;
    unsigned dfr = b->have_aim && frame > b->last_frame ? frame - b->last_frame : 2; if (dfr > 30) dfr = 30; b->last_frame = frame;
    for (int i = 0; i < ne; i++) {
        const Nb1Entry *e = &ents[i]; if (!e->ntiles) continue;
        int x0 = e->tiles[0].x, y0 = e->tiles[0].y, x1 = x0, y1 = y0; for (int k = 0; k < e->ntiles; k++) { const Nb1Tile *t = &e->tiles[k]; if (t->x < x0) x0 = t->x; if (t->y < y0) y0 = t->y; if (t->x + t->w > x1) x1 = t->x + t->w; if (t->y + t->h > y1) y1 = t->y + t->h; }
        int t0 = e->tiles[0].t, code = (((t0 & 0x7ff) | (st->bank[(t0 >> 11) & 7] << 11)) + e->offset) & 0x7fff;
        if (code >= 6315 && code <= 6334) hit_visible = 1;
        Cand c = { code, e->color, e->pri, (x0 + x1) / 2, (y0 + y1) / 2, x1 - x0, y1 - y0, 0 };
        if (is_hud_code(code) || c.pri >= 6 || c.w < b->d.min_size || c.h < b->d.min_size || c.w > b->d.max_size || c.h > b->d.max_size || c.cx < 3 || c.cx > 284 || c.cy < 3 || c.cy > 220) continue;
        c.wt = weight_of(b, &c); if (c.wt > 0) cs[nc++] = c;
    }
    /* learning: did the last shot score (the game's HIT! marker or the score going up) or cost a life? */
    if (b->shot_pending && frame - b->shot_frame <= 40) {
        int hit = (hit_visible && !b->last_hit_visible) || (g && g->valid && g->score - b->last_score >= 15 && g->score - b->last_score < 5000);
        int pen = g && g->valid && g->lives < b->last_lives;
        BKey *k = key_get(b, b->shot_code, b->shot_col, 1);
        if (hit) { k->hits++; k->n++; b->nhits++; b->shot_pending = 0; } else if (pen) { k->pen++; k->n++; b->npen++; b->shot_pending = 0; }
    } else if (b->shot_pending) { key_get(b, b->shot_code, b->shot_col, 1)->n++; b->shot_pending = 0; }
    b->last_hit_visible = hit_visible; if (g && g->valid) { b->last_score = g->score; b->last_lives = g->lives; }
    /* choose the target: keep the current one while it is still there and has not had its chances */
    Cand *pick = NULL; double best = -1;
    for (int i = 0; i < nc; i++) {
        Cand *c = &cs[i]; double dx = c->cx - b->ax, dy = c->cy - b->ay, dist = sqrt(dx * dx + dy * dy), s = c->wt / (1.0 + dist / 90.0);
        if (b->have_target && c->code == b->cur_code && c->col == b->cur_col && abs(c->cx - b->cur_cx) < 40 && abs(c->cy - b->cur_cy) < 40) s *= (b->target_shots >= 3 ? 0.1 : 2.5);
        else if (c->code == b->shot_code && c->col == b->shot_col && frame - b->shot_frame < 90 && abs(c->cx - b->cur_cx) < 24 && abs(c->cy - b->cur_cy) < 24) s *= 0.15;   /* just shot, still there */
        if (s > best) { best = s; pick = c; }
    }
    if (pick) {
        if (!b->have_target || pick->code != b->cur_code || pick->col != b->cur_col) { b->target_shots = 0; b->target_since = frame; }
        b->have_target = 1; b->cur_code = pick->code; b->cur_col = pick->col; b->cur_cx = pick->cx; b->cur_cy = pick->cy;
        double vx = 0, vy = 0;                                                 /* a moving target: lead it by its speed over the last state */
        for (int i = 0; i < b->nprev; i++) if (b->prev[i].code == pick->code && abs(b->prev[i].cx - pick->cx) < 30 && abs(b->prev[i].cy - pick->cy) < 30) { vx = (pick->cx - b->prev[i].cx) / (double)dfr; vy = (pick->cy - b->prev[i].cy) / (double)dfr; break; }
        double tx = pick->cx + vx * 6, ty = pick->cy + vy * 6;
        if (!b->have_aim) { b->ax = tx; b->ay = ty; b->have_aim = 1; }
        double dx = tx - b->ax, dy = ty - b->ay, dist = sqrt(dx * dx + dy * dy), step = b->d.speed * dfr; if (dist > step) { dx *= step / dist; dy *= step / dist; }
        b->ax += dx; b->ay += dy;
        double ex = tx - b->ax, ey = ty - b->ay;
        if (b->trig_left > 0) { b->trig_left -= (int)dfr; }
        else if (b->gap_left > 0) { b->gap_left -= (int)dfr; }
        else if (ex * ex + ey * ey < 5.0 * 5.0 && st) { b->trig_left = (int)b->d.hold; b->gap_left = (int)b->d.gap; b->shot_code = pick->code; b->shot_col = pick->col; b->shot_frame = frame; b->shot_pending = 1; b->target_shots++; b->nshots++;
                                                         if (g && g->valid) { b->last_score = g->score; b->last_lives = g->lives; } }
        snprintf(b->status, sizeof b->status, "auto: target code %d col %d (w %.2f)  shots %u hits %u penalties %u", pick->code, pick->col, pick->wt, b->nshots, b->nhits, b->npen);
    } else {
        b->have_target = 0; if (!b->have_aim) { b->ax = 144; b->ay = 112; b->have_aim = 1; }
        if (b->trig_left > 0) b->trig_left -= (int)dfr;
        snprintf(b->status, sizeof b->status, "auto: nothing to shoot   shots %u hits %u penalties %u", b->nshots, b->nhits, b->npen);
    }
    b->nprev = nc < 64 ? nc : 64; memcpy(b->prev, cs, (size_t)b->nprev * sizeof *cs);
    *sx = b->ax; *sy = b->ay; *trig = b->trig_left > 0;
    /* credits and start: when the game is not being played */
    *coin = *start = 0;
    if (g && g->valid && g->mode != 2) { if (frame - b->last_coin > 400) { b->last_coin = frame; } if (frame - b->last_coin < 8) *coin = 1;
                                         if (frame - b->last_start > 240) { b->last_start = frame; } if (frame - b->last_start < 8 && frame - b->last_coin > 120) *start = 1; }
}
