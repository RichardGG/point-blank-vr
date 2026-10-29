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
typedef struct { char id[160]; char text[160]; BKey *keys; int nkeys, cap; BScene sc; int has_sc; int *order, norder; int match_sample, multihit, has_lead; double lead; } BStage;       /* what one stage (difficulty, stage counter, card) has taught: its own sprite statistics and the sentence the game shows before it */
typedef struct { int code, col, pri, cx, cy, w, h; double wt; int still; } Cand;       /* still: frames it has been at the same place (scenery) */
typedef struct { double min_size, max_size, unknown, speed, gap, hold, lead, fire_min; } BDef;

struct Nb1Bot {
    BDef d; BScene *sc; int nsc; BKey *keys; int nkeys, capkeys; BScene *cur; BStage *stages; int nstages; BStage *stage; unsigned rng;
    double ax, ay;                       /* where the gun points now */
    int have_aim; unsigned last_frame; int trig_left, gap_left;
    int cur_code, cur_col, cur_cx, cur_cy, have_target, target_shots; unsigned target_since;
    int shot_code, shot_col; unsigned shot_frame; int shot_pending;
    int last_score, last_lives, last_hit_visible; unsigned last_coin, last_start;
    Cand prev[64]; int nprev;
    char status[160]; unsigned nshots, nhits, npen; int want_mode; char stage_override[160]; struct { int x, y; unsigned f; } rs[8]; int nrs; int shot_score, shot_lives, score_claimed, lives_claimed; struct { unsigned f; int code, col, x, y, sscore, slives, done; double vx, vy, err; } q[8]; int nq; struct { int x, y, code, hit; unsigned f; } hist[32]; int nhist, hist_head;
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

static BKey *keys_get(BKey **arr, int *n, int *cap, int code, int col, int create) {
    for (int i = 0; i < *n; i++) if ((*arr)[i].code == code && (*arr)[i].col == col) return &(*arr)[i];
    if (!create) return NULL;
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 64; *arr = realloc(*arr, (size_t)*cap * sizeof **arr); }
    BKey *k = &(*arr)[(*n)++]; memset(k, 0, sizeof *k); k->code = code; k->col = col; return k;
}
static BKey *key_get(Nb1Bot *b, int code, int col, int create) { return keys_get(&b->keys, &b->nkeys, &b->capkeys, code, col, create); }
static BKey *skey_get(Nb1Bot *b, int code, int col, int create) { return b->stage ? keys_get(&b->stage->keys, &b->stage->nkeys, &b->stage->cap, code, col, create) : NULL; }

static void bot_env(Nb1Bot *b) {                                          /* tuning without a rules file: NB1_BOT_SPEED / GAP / HOLD / LEAD / FIRE_MIN */
    const char *v; if ((v = getenv("NB1_BOT_SPEED"))) b->d.speed = atof(v); if ((v = getenv("NB1_BOT_GAP"))) b->d.gap = atof(v); if ((v = getenv("NB1_BOT_HOLD"))) b->d.hold = atof(v);
    if ((v = getenv("NB1_BOT_LEAD"))) b->d.lead = atof(v); if ((v = getenv("NB1_BOT_FIRE_MIN"))) b->d.fire_min = atof(v);
}
Nb1Bot *nb1_bot_new(const char *rules_path) {
    Nb1Bot *b = calloc(1, sizeof *b); b->d = (BDef){ 8, 200, 0.35, 14, 12, 4, 6, 0.2 }; b->lives_claimed = 99;
    if (!rules_path) { bot_env(b); return b; }
    FILE *f = fopen(rules_path, "rb"); if (!f) { fprintf(stderr, "bot: cannot read %s (using the defaults)\n", rules_path); return b; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *txt = malloc((size_t)n + 1); if (fread(txt, 1, (size_t)n, f) != (size_t)n) n = 0; txt[n] = 0; fclose(f);
    Json *j = json_parse(txt); free(txt); if (!j) { fprintf(stderr, "bot: %s is not valid JSON\n", rules_path); return b; }
    const Json *d = json_get(j, "defaults");
    if (d) { const Json *v; if ((v = json_get(d, "min_size"))) b->d.min_size = v->num; if ((v = json_get(d, "max_size"))) b->d.max_size = v->num; if ((v = json_get(d, "unknown_weight"))) b->d.unknown = v->num;
             if ((v = json_get(d, "speed"))) b->d.speed = v->num; if ((v = json_get(d, "shot_gap"))) b->d.gap = v->num; if ((v = json_get(d, "trigger_hold"))) b->d.hold = v->num; if ((v = json_get(d, "lead"))) b->d.lead = v->num; if ((v = json_get(d, "fire_min"))) b->d.fire_min = v->num; }
    const Json *ks = json_get(j, "keys");                                  /* [[code, colour, shots, hits, penalties], ...] */
    if (ks && ks->t == J_ARR) for (int i = 0; i < ks->n; i++) { const Json *r = ks->kid[i]; if (r->t != J_ARR || r->n < 5) continue; BKey *k = key_get(b, (int)r->kid[0]->num, (int)r->kid[1]->num, 1); k->n = r->kid[2]->num; k->hits = r->kid[3]->num; k->pen = r->kid[4]->num; }
    const Json *stj = json_get(j, "stages");                              /* {"d2_n1_c2": {"text": "...", "keys": [[code, colour, shots, hits, penalties], ...]}} */
    if (stj && stj->t == J_OBJ) { b->stages = calloc((size_t)(stj->n ? stj->n : 1), sizeof *b->stages);
        for (int i = 0; i < stj->n; i++) { BStage *g = &b->stages[b->nstages++]; snprintf(g->id, sizeof g->id, "%s", stj->key[i]); const Json *o = stj->kid[i], *v;
            if ((v = json_get(o, "text")) && v->t == J_STR) snprintf(g->text, sizeof g->text, "%s", v->str);
            g->has_sc = 1; g->sc.only = (v = json_get(o, "only")) && v->num != 0; g->sc.np = sel_parse(json_get(o, "prefer"), &g->sc.prefer); g->sc.na = sel_parse(json_get(o, "avoid"), &g->sc.avoid);
            if ((v = json_get(o, "order")) && v->t == J_ARR) { g->norder = v->n; g->order = malloc((size_t)(v->n ? v->n : 1) * sizeof *g->order); for (int q = 0; q < v->n; q++) g->order[q] = (int)v->kid[q]->num; }   /* codes to shoot in this sequence */
            g->match_sample = (v = json_get(o, "match_sample")) && v->num != 0; g->multihit = (v = json_get(o, "multihit")) && v->num != 0; if ((v = json_get(o, "lead"))) { g->lead = v->num; g->has_lead = 1; }
            const Json *kk = json_get(o, "keys"); if (kk && kk->t == J_ARR) for (int q = 0; q < kk->n; q++) { const Json *r = kk->kid[q]; if (r->t != J_ARR || r->n < 5) continue;
                BKey *k = keys_get(&g->keys, &g->nkeys, &g->cap, (int)r->kid[0]->num, (int)r->kid[1]->num, 1); k->n = r->kid[2]->num; k->hits = r->kid[3]->num; k->pen = r->kid[4]->num; } } }
    const Json *sc = json_get(j, "scenes");
    if (sc && sc->t == J_OBJ) {
        b->sc = calloc((size_t)(sc->n ? sc->n : 1), sizeof *b->sc);
        for (int i = 0; i < sc->n; i++) { BScene *s = &b->sc[b->nsc++]; snprintf(s->name, sizeof s->name, "%s", sc->key[i]); const Json *o = sc->kid[i], *v;
            s->only = (v = json_get(o, "only")) && v->num != 0; s->np = sel_parse(json_get(o, "prefer"), &s->prefer); s->na = sel_parse(json_get(o, "avoid"), &s->avoid); }
    }
    json_free(j); bot_env(b); return b;
}
void nb1_bot_free(Nb1Bot *b) { if (!b) return; for (int i = 0; i < b->nstages; i++) { free(b->stages[i].keys); free(b->stages[i].order); if (b->stages[i].has_sc) { free(b->stages[i].sc.prefer); free(b->stages[i].sc.avoid); } } free(b->stages); for (int i = 0; i < b->nsc; i++) { free(b->sc[i].prefer); free(b->sc[i].avoid); } free(b->sc); free(b->keys); free(b); }
void nb1_bot_set_scene(Nb1Bot *b, const char *name) {
    b->cur = NULL; if (!name) return;
    for (int i = 0; i < b->nsc; i++) if (!strcmp(b->sc[i].name, name)) { b->cur = &b->sc[i]; return; }
}
const char *nb1_bot_status(const Nb1Bot *b) { return b->status; }
void nb1_bot_set_mode(Nb1Bot *b, int mode) { b->want_mode = mode; }
void nb1_bot_set_stage(Nb1Bot *b, const char *id) { snprintf(b->stage_override, sizeof b->stage_override, "%s", id ? id : ""); }
void nb1_bot_set_seed(Nb1Bot *b, unsigned seed) { b->rng = seed * 2654435761u + 1; }
int nb1_bot_key(const Nb1Bot *b, int i, int *code, int *col, double *n, double *hits, double *pen) {
    if (i < 0 || i >= b->nkeys) return 0; *code = b->keys[i].code; *col = b->keys[i].col; *n = b->keys[i].n; *hits = b->keys[i].hits; *pen = b->keys[i].pen; return 1;
}
int nb1_bot_save_stats(const Nb1Bot *b, const char *path) {
    FILE *f = fopen(path, "w"); if (!f) return -1; fprintf(f, "[");
    for (int i = 0; i < b->nkeys; i++) fprintf(f, "%s[%d,%d,%.0f,%.0f,%.0f]", i ? "," : "", b->keys[i].code, b->keys[i].col, b->keys[i].n, b->keys[i].hits, b->keys[i].pen);
    fprintf(f, "]\n"); fclose(f);
    char p2[1100]; snprintf(p2, sizeof p2, "%s.stages", path); f = fopen(p2, "w"); if (!f) return -1; fprintf(f, "{");              /* per stage statistics: {id: {"keys": [[...]]}} */
    int first = 1; for (int i = 0; i < b->nstages; i++) { const BStage *g = &b->stages[i]; if (!g->nkeys) continue; fprintf(f, "%s\"%s\":{\"keys\":[", first ? "" : ",", g->id); first = 0;
        for (int q = 0; q < g->nkeys; q++) fprintf(f, "%s[%d,%d,%.0f,%.0f,%.0f]", q ? "," : "", g->keys[q].code, g->keys[q].col, g->keys[q].n, g->keys[q].hits, g->keys[q].pen); fprintf(f, "]}"); }
    fprintf(f, "}\n"); fclose(f); return 0;
}

static unsigned g_frame = 0;
static int g_order_min = 1 << 30;                                    /* lowest position in the stage's "order" among the targets visible now */
static int g_sample_code = -1, g_sample_keep = -1; static unsigned g_sample_f = 0;                                       /* the code of the SAMPLE target in a "match the sample" stage */
static double weight_of(Nb1Bot *b, const Cand *c) {
    double w = b->d.unknown; BKey *k = key_get(b, c->code, c->col, 0), *sk = b->stage ? keys_get(&b->stage->keys, &b->stage->nkeys, &b->stage->cap, c->code, c->col, 0) : NULL;
    if (sk && sk->n >= 3) k = sk;                                                  /* what this very stage has shown beats the general statistics */
    double gn = 0, gh = 0, gp = 0;                                                  /* the sprite family (the 16 codes around: animation frames of one creature): used when this exact sprite has little data */
    if (!(k && k->n >= 3)) { for (int i = 0; i < b->nkeys; i++) if ((b->keys[i].code >> 4) == (c->code >> 4) && b->keys[i].col == c->col) { gn += b->keys[i].n; gh += b->keys[i].hits; gp += b->keys[i].pen; }
                             if (b->stage) for (int i = 0; i < b->stage->nkeys; i++) if ((b->stage->keys[i].code >> 4) == (c->code >> 4) && b->stage->keys[i].col == c->col) { gn += 2 * b->stage->keys[i].n; gh += 2 * b->stage->keys[i].hits; gp += 2 * b->stage->keys[i].pen; } }
    int proven = 0;
    if (k && k->n >= 3) { w = (k->hits + 0.3) / (k->n + 1.0); if (w < 0.04) w = 0.04; proven = k->hits >= 3 && k->hits > 0.5 * k->n; }
    else if (k && k->n > 0) { w = (k->hits + 0.3 + gh) / (k->n + 1.0 + gn); if (w < 0.04) w = 0.04; }
    else if (gn >= 3) { w = (gh + 0.3) / (gn + 1.0); if (w < 0.04) w = 0.04; proven = gh >= 6 && gh > 0.5 * gn; }
    if (k && k->pen >= 1 && k->pen >= k->hits) return 0;
    if (!k && gn >= 3 && gp >= 2 && gp >= gh) return 0;
    { int miss = 0, hit = 0; for (int i = 0; i < b->nhist; i++) if (g_frame - b->hist[i].f < 400 && abs(b->hist[i].x - c->cx) < 16 && abs(b->hist[i].y - c->cy) < 16 && b->hist[i].code == c->code) { if (b->hist[i].hit) hit++; else miss++; }
      if (miss >= 2 && hit == 0) w *= 0.03; else if (miss >= 3 && miss > 2 * hit) w *= 0.1; }                                      /* a spot that keeps missing: nothing there to hit */
    if (c->still > 45 && !proven) w *= 0.12;                                          /* scenery: it has not moved for a while and nothing says shooting it scores */                          /* shooting it cost a life more often than it scored */
    if (b->stage && b->stage->has_sc) {                                          /* the stage's own rules (written from its rule screen) */
        const BScene *sc = &b->stage->sc;
        for (int i = 0; i < sc->na; i++) if (sel_match(&sc->avoid[i], c)) return 0;
        int pref = 0; for (int i = 0; i < sc->np; i++) if (sel_match(&sc->prefer[i], c)) { if (sc->prefer[i].weight > w || !pref) w = sc->prefer[i].weight; pref = 1; }
        if (sc->only && !pref) w *= 0.02;
    }
    if (b->stage && b->stage->norder) {
        int idx = -1; for (int i = 0; i < b->stage->norder; i++) if (b->stage->order[i] == c->code) { idx = i; break; }
        if (idx >= 0) w = idx == g_order_min ? 12.0 : 0.01; else w *= 0.05;
    }
    if (b->stage && b->stage->match_sample) { if (g_sample_code >= 0 && c->code != g_sample_code) w *= 0.02; if (g_sample_code >= 0 && c->code == g_sample_code && c->cy < 62 && abs(c->cx - 145) < 30) return 0; }      /* never the sample picture itself */
    if (b->cur) {
        for (int i = 0; i < b->cur->na; i++) if (sel_match(&b->cur->avoid[i], c)) return 0;
        int pref = 0; for (int i = 0; i < b->cur->np; i++) if (sel_match(&b->cur->prefer[i], c)) { if (b->cur->prefer[i].weight > w || !pref) w = b->cur->prefer[i].weight; pref = 1; }
        if (b->cur->only && !pref) w *= 0.03;
    }
    return w;
}

static int is_hud_code(int code) { return (code >= 3799 && code <= 3860) || (code >= 6315 && code <= 6334) || code == 2289 || code == 2401 || code == 2415 || code == 2528; }

/* credits and start: when the game is not being played */
static void credits(Nb1Bot *b, const Nb1GameInfo *g, unsigned frame, int *coin, int *start) {
    if (g && g->valid && g->mode != 2) { if (frame - b->last_coin > 400) { b->last_coin = frame; } if (frame - b->last_coin < 8) *coin = 1;
                                         if (frame - b->last_start > 240) { b->last_start = frame; } if (frame - b->last_start < 8 && frame - b->last_coin > 120) *start = 1; }
}

void nb1_bot_step(Nb1Bot *b, const Nb1State *st, const Nb1GameInfo *g, unsigned frame, double *sx, double *sy, int *trig, int *coin, int *start) {
    if (b->stage_override[0] || (g && g->valid)) { char id[160]; if (b->stage_override[0]) snprintf(id, sizeof id, "%s", b->stage_override); else snprintf(id, sizeof id, "d%d_n%d_c%d", g->diff, g->stage, g->card); if (!b->stage || strcmp(b->stage->id, id)) {
        b->stage = NULL; for (int i = 0; i < b->nstages; i++) if (!strcmp(b->stages[i].id, id)) b->stage = &b->stages[i];
        if (!b->stage) { b->stages = realloc(b->stages, (size_t)(b->nstages + 1) * sizeof *b->stages); BStage *ns = &b->stages[b->nstages++]; memset(ns, 0, sizeof *ns); snprintf(ns->id, sizeof ns->id, "%s", id); b->stage = ns; } } }
    /* the mode screen (after Start): shoot the wanted button (1 training, 2 beginner, 3 expert, 4 very hard), and nothing else until the game has taken it (stage counter leaves 0) */
    if (b->want_mode > 0 && g && g->valid && g->stage == 0 && b->last_start && frame - b->last_start < 900) {
        static const int MP[4][2] = { { 48, 110 }, { 144, 100 }, { 240, 100 }, { 144, 182 } }; int m = (b->want_mode - 1) & 3; unsigned d2 = b->have_aim && frame > b->last_frame ? frame - b->last_frame : 2; if (d2 > 30) d2 = 30; b->last_frame = frame;
        if (!b->have_aim) { b->ax = 144; b->ay = 112; b->have_aim = 1; }
        double dx = MP[m][0] - b->ax, dy = MP[m][1] - b->ay, dist = sqrt(dx * dx + dy * dy), step = b->d.speed * d2; if (dist > step) { dx *= step / dist; dy *= step / dist; } b->ax += dx; b->ay += dy;
        *sx = b->ax; *sy = b->ay; *trig = dist < 6 && frame - b->last_start > 200 && (frame - b->last_start) % 40 < 8; b->trig_left = 0; snprintf(b->status, sizeof b->status, "auto: choosing mode %d", b->want_mode);
        *coin = *start = 0; credits(b, g, frame, coin, start); return;
    }
    static Nb1Entry ents[256]; int ne = nb1_sprite_entries(st, ents); Cand cs[256]; int nc = 0, hit_visible = 0;
    g_order_min = 1 << 30; g_sample_code = -1; g_frame = frame;
    if (b->stage && b->stage->norder) for (int i = 0; i < ne; i++) { const Nb1Entry *e = &ents[i]; if (!e->ntiles) continue; int t0 = e->tiles[0].t, code = (((t0 & 0x7ff) | (st->bank[(t0 >> 11) & 7] << 11)) + e->offset) & 0x7fff;
        for (int q = 0; q < b->stage->norder; q++) if (b->stage->order[q] == code && q < g_order_min) g_order_min = q; }
    if (b->stage && b->stage->match_sample) for (int i = 0; i < ne; i++) {      /* "find the matching target": the SAMPLE animal is the 48x48 sprite at the top middle (its frame is code 14483); the targets are the same sprite frames, so a match has the same code */
        const Nb1Entry *e = &ents[i]; if (!e->ntiles) continue; int x0 = e->tiles[0].x, y0 = e->tiles[0].y, x1 = x0, y1 = y0; for (int k = 0; k < e->ntiles; k++) { const Nb1Tile *t = &e->tiles[k]; if (t->x < x0) x0 = t->x; if (t->y < y0) y0 = t->y; if (t->x + t->w > x1) x1 = t->x + t->w; if (t->y + t->h > y1) y1 = t->y + t->h; }
        int t0 = e->tiles[0].t, code = (((t0 & 0x7ff) | (st->bank[(t0 >> 11) & 7] << 11)) + e->offset) & 0x7fff;
        if (x0 >= 114 && x0 <= 128 && y0 >= 2 && y0 <= 14 && x1 - x0 >= 42 && x1 - x0 <= 54 && y1 - y0 >= 42 && y1 - y0 <= 54 && code != 14483 && code != 14471) { g_sample_code = code; g_sample_keep = code; g_sample_f = frame; } }
    if (b->stage && b->stage->match_sample && g_sample_code < 0 && g_sample_keep >= 0 && frame - g_sample_f < 150) g_sample_code = g_sample_keep;      /* the sample sprite is missing on some frames (it animates / changes between rounds): keep the last one for a couple of seconds */
    unsigned dfr = b->have_aim && frame > b->last_frame ? frame - b->last_frame : 2; if (dfr > 30) dfr = 30; b->last_frame = frame;
    /* "type the same 3-letter word": the word is three dotted 2x2-tile letter sprites near the top (tile numbers 3651..3738, alphabetical: A 3651 ... Y 3734; typed letters turn solid = tile - 96);
       shoot the matching key of the on-screen keyboard, in any order (a wrong key costs nothing).  Letter table and key positions: research repo game/stages/type-the-word/letters.json. */
    {
        static const int LT[26] = { 3651, 3655, 3658, 3662, 3666, 3668, 3670, 3674, 3678, 3680, 3682, 3686, 3688, 3692, 3696, 3700, 3704, 3708, 3712, 3716, 3718, 3722, 3726, 3730, 3734, 3738 };     /* A..Z (Q, S, Z are inferred, not seen) */
        static unsigned type_wait = 0;
        int letter = -1, lx = 0;
        for (int i = 0; i < ne; i++) { const Nb1Entry *e = &ents[i]; if (!e->ntiles || e->ncols != 2 || e->nrows != 2 || e->tiles[0].y < 20 || e->tiles[0].y > 44) continue;
            int t = e->tiles[0].t; for (int k = 0; k < 26; k++) if (LT[k] == t && (letter < 0 || e->tiles[0].x < lx)) { letter = k; lx = e->tiles[0].x; } }
        if (letter >= 0) {
            static const char *ROWS[3] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" }; static const int RX[3] = { 36, 46, 60 }, RY[3] = { 99, 124, 147 };
            double kx = 144, ky = 112; char ch = 'A' + letter;
            for (int r = 0; r < 3; r++) for (int q = 0; ROWS[r][q]; q++) if (ROWS[r][q] == ch) { kx = RX[r] + 24 * q; ky = RY[r]; }
            if (!b->have_aim) { b->ax = 144; b->ay = 112; b->have_aim = 1; }
            double dx = kx - b->ax, dy = ky - b->ay, dist = sqrt(dx * dx + dy * dy), step = b->d.speed * dfr; if (dist > step) { dx *= step / dist; dy *= step / dist; } b->ax += dx; b->ay += dy;
            double ex = kx - b->ax, ey = ky - b->ay;
            if (b->trig_left > 0) b->trig_left -= (int)dfr;
            else if (frame >= type_wait && ex * ex + ey * ey < 3.0 * 3.0) { b->trig_left = (int)b->d.hold; type_wait = frame + 18; b->nshots++; }
            b->have_target = 0; b->nprev = 0;
            snprintf(b->status, sizeof b->status, "auto: typing %c", ch);
            *sx = b->ax; *sy = b->ay; *trig = b->trig_left > 0; *coin = *start = 0; credits(b, g, frame, coin, start); return;
        }
    }
    for (int i = 0; i < ne; i++) {
        const Nb1Entry *e = &ents[i]; if (!e->ntiles) continue;
        int x0 = e->tiles[0].x, y0 = e->tiles[0].y, x1 = x0, y1 = y0; for (int k = 0; k < e->ntiles; k++) { const Nb1Tile *t = &e->tiles[k]; if (t->x < x0) x0 = t->x; if (t->y < y0) y0 = t->y; if (t->x + t->w > x1) x1 = t->x + t->w; if (t->y + t->h > y1) y1 = t->y + t->h; }
        int t0 = e->tiles[0].t, code = (((t0 & 0x7ff) | (st->bank[(t0 >> 11) & 7] << 11)) + e->offset) & 0x7fff;
        if (code >= 6315 && code <= 6334) hit_visible = 1;
        Cand c = { code, e->color, e->pri, (x0 + x1) / 2, (y0 + y1) / 2, x1 - x0, y1 - y0, 0 };
        if (is_hud_code(code) || c.pri >= 6 || c.w < b->d.min_size || c.h < b->d.min_size || c.w > b->d.max_size || c.h > b->d.max_size || c.cx < 3 || c.cx > 284 || c.cy < 3 || c.cy > 220) continue;
        c.still = 0; for (int q = 0; q < b->nprev; q++) if (b->prev[q].code == c.code && b->prev[q].col == c.col && abs(b->prev[q].cx - c.cx) <= 1 && abs(b->prev[q].cy - c.cy) <= 1) { c.still = b->prev[q].still + (int)dfr; break; }
        c.wt = weight_of(b, &c); if (c.wt > 0) cs[nc++] = c;
    }
    if (getenv("NB1_BOT_DEBUG") && (frame / 2) % 30 == 0) { fprintf(stderr, "bot f%u stage '%s' aim %.0f,%.0f sample %d cands:", frame, b->stage ? b->stage->text[0] ? b->stage->text : b->stage->id : "-", b->ax, b->ay, g_sample_code); for (int i = 0; i < nc; i++) fprintf(stderr, " [%d:%d p%d %dx%d @%d,%d w%.2f]", cs[i].code, cs[i].col, cs[i].pri, cs[i].w, cs[i].h, cs[i].cx, cs[i].cy, cs[i].wt); fprintf(stderr, "\n"); }
    /* learning: every shot waits (up to 45 frames) for the game's verdict: the score word rising (a hit) or a life lost (a penalty). A rise is credited to one shot only. */
    if (g && g->valid && g->lives > b->lives_claimed) b->lives_claimed = 99;                  /* lives went up: a new game */
    for (int i = 0; i < b->nq; i++) {
        if (b->q[i].done) continue; unsigned age = frame - b->q[i].f; int hit = 0, pen = 0;
        if (g && g->valid) { int base = b->q[i].sscore > b->score_claimed ? b->q[i].sscore : b->score_claimed; hit = g->score - base >= 10 && g->score - base < 5000; pen = g->lives < b->q[i].slives && g->lives < b->lives_claimed; }
        else hit = hit_visible && !b->last_hit_visible && age <= 20;
        if (hit && age > 16) hit = 0;                                                   /* a hit shows in the score within a few frames: a later rise is some other shot's */
        if (!pen && !hit && age <= 40) continue;
        BKey *k = key_get(b, b->q[i].code, b->q[i].col, 1), *sk = skey_get(b, b->q[i].code, b->q[i].col, 1);
        k->n++; if (sk) sk->n++;
        if (pen) { k->pen++; if (sk) sk->pen++; b->npen++; if (g) b->lives_claimed = g->lives; }
        else if (hit) { k->hits++; if (sk) sk->hits++; b->nhits++; if (g) b->score_claimed = g->score; }
        if (getenv("NB1_BOT_DEBUG")) fprintf(stderr, "shot f%u %s code %d:%d at %d,%d v %.1f,%.1f err %.1f  %s\n", b->q[i].f, b->stage ? b->stage->id : "-", b->q[i].code, b->q[i].col, b->q[i].x, b->q[i].y, b->q[i].vx, b->q[i].vy, b->q[i].err, pen ? "PENALTY" : hit ? "hit" : "miss");
        b->hist[b->hist_head % 32].x = b->q[i].x; b->hist[b->hist_head % 32].y = b->q[i].y; b->hist[b->hist_head % 32].code = b->q[i].code; b->hist[b->hist_head % 32].hit = hit; b->hist[b->hist_head % 32].f = b->q[i].f; b->hist_head++; if (b->nhist < 32) b->nhist++;
        b->q[i].done = 1;
    }
    { int w = 0; for (int i = 0; i < b->nq; i++) if (!b->q[i].done) b->q[w++] = b->q[i]; b->nq = w; }
    b->last_hit_visible = hit_visible; if (g && g->valid) { b->last_score = g->score; b->last_lives = g->lives; }
    for (int i = 0; i < nc; i++) {                                                /* one target drawn as several sprite entries (head and body): merge entries that touch */
        if (cs[i].wt < 0.25) continue;
        for (int j = i + 1; j < nc; j++) { if (cs[j].wt < 0.25) continue;
            int ax0 = cs[i].cx - cs[i].w / 2 - 4, ax1 = cs[i].cx + cs[i].w / 2 + 4, ay0 = cs[i].cy - cs[i].h / 2 - 4, ay1 = cs[i].cy + cs[i].h / 2 + 4, bx0 = cs[j].cx - cs[j].w / 2, bx1 = cs[j].cx + cs[j].w / 2, by0 = cs[j].cy - cs[j].h / 2, by1 = cs[j].cy + cs[j].h / 2;
            if (ax1 < bx0 || bx1 < ax0 || ay1 < by0 || by1 < ay0) continue;
            int ux0 = (ax0 + 4 < bx0 ? ax0 + 4 : bx0), ux1 = (ax1 - 4 > bx1 ? ax1 - 4 : bx1), uy0 = (ay0 + 4 < by0 ? ay0 + 4 : by0), uy1 = (ay1 - 4 > by1 ? ay1 - 4 : by1);
            if (ux1 - ux0 > 96 || uy1 - uy0 > 112) continue;
            if (cs[j].wt > cs[i].wt) { cs[i].code = cs[j].code; cs[i].col = cs[j].col; cs[i].wt = cs[j].wt; cs[i].pri = cs[j].pri; }
            cs[i].cx = (ux0 + ux1) / 2; cs[i].cy = (uy0 + uy1) / 2; cs[i].w = ux1 - ux0; cs[i].h = uy1 - uy0; cs[j] = cs[--nc]; j = i; }
    }
    /* choose the target: keep the current one while it is still there and has not had its chances */
    Cand *pick = NULL; double best = -1; int pick_supp = 0;
    for (int i = 0; i < nc; i++) {
        Cand *c = &cs[i]; double dx = c->cx - b->ax, dy = c->cy - b->ay, dist = sqrt(dx * dx + dy * dy), s = c->wt / (1.0 + dist / 90.0); int supp = 0;
        if (b->have_target && c->code == b->cur_code && c->col == b->cur_col && abs(c->cx - b->cur_cx) < 40 && abs(c->cy - b->cur_cy) < 40) s *= (b->target_shots >= 3 && !(b->stage && b->stage->multihit) ? 0.1 : 2.5);
        if (!(b->stage && b->stage->multihit)) for (int q = 0; q < b->nrs; q++) if (frame - b->rs[q].f < 50 && abs(c->cx - b->rs[q].x) < 26 && abs(c->cy - b->rs[q].y) < 26) { s *= 0.04; supp = 1; break; }      /* just shot there: it is falling or gone */
        { unsigned h = (b->rng ^ (unsigned)(c->code * 40503 + c->col * 977)) * 2654435761u; h ^= h >> 15; h *= 2246822519u; s *= 0.75 + 0.5 * ((h >> 8) & 0xff) / 255.0; }     /* a fixed per-run factor for each sprite type, so runs differ (which stage card they pick) but a pick never flips back and forth */
        if (s > best) { best = s; pick = c; pick_supp = supp; }
    }
    if (pick) {
        if (!b->have_target || pick->code != b->cur_code || pick->col != b->cur_col) { b->target_shots = 0; b->target_since = frame; }
        b->have_target = 1; b->cur_code = pick->code; b->cur_col = pick->col; b->cur_cx = pick->cx; b->cur_cy = pick->cy;
        double vx = 0, vy = 0;                                                 /* a moving target: lead it by its speed over the last state */
        for (int i = 0; i < b->nprev; i++) if (b->prev[i].code == pick->code && abs(b->prev[i].cx - pick->cx) < 30 && abs(b->prev[i].cy - pick->cy) < 30) { vx = (pick->cx - b->prev[i].cx) / (double)dfr; vy = (pick->cy - b->prev[i].cy) / (double)dfr; break; }
        double ld = b->stage && b->stage->has_lead ? b->stage->lead : b->d.lead;      /* a stage can set its own lead (fast flyers such as clay targets need less) */
        double tx = pick->cx + vx * ld, ty = pick->cy + vy * ld;
        if (!b->have_aim) { b->ax = tx; b->ay = ty; b->have_aim = 1; }
        double dx = tx - b->ax, dy = ty - b->ay, dist = sqrt(dx * dx + dy * dy), step = b->d.speed * dfr; if (dist > step) { dx *= step / dist; dy *= step / dist; }
        b->ax += dx; b->ay += dy;
        double ex = tx - b->ax, ey = ty - b->ay;
        if (b->trig_left > 0) { b->trig_left -= (int)dfr; }
        else if (b->gap_left > 0) { b->gap_left -= (int)dfr; }
        else if (ex * ex + ey * ey < 5.0 * 5.0 && st && pick->wt >= b->d.fire_min && !pick_supp) { b->trig_left = (int)b->d.hold; b->gap_left = (int)b->d.gap; b->shot_code = pick->code; b->shot_col = pick->col; b->shot_frame = frame; if (b->nq < 8) { b->q[b->nq].f = frame; b->q[b->nq].code = pick->code; b->q[b->nq].col = pick->col; b->q[b->nq].x = pick->cx; b->q[b->nq].y = pick->cy; b->q[b->nq].vx = vx; b->q[b->nq].vy = vy; b->q[b->nq].err = sqrt(ex * ex + ey * ey); b->q[b->nq].sscore = g && g->valid ? g->score : 1 << 30; b->q[b->nq].slives = g && g->valid ? g->lives : 0;      /* no game numbers yet: this shot can be judged neither a hit nor a penalty */ b->q[b->nq].done = 0; b->nq++; } for (int q = 7; q > 0; q--) b->rs[q] = b->rs[q - 1]; b->rs[0].x = pick->cx; b->rs[0].y = pick->cy; b->rs[0].f = frame; if (b->nrs < 8) b->nrs++; b->shot_pending = 1; b->target_shots++; b->nshots++;
                                                         if (g && g->valid) { b->last_score = g->score; b->last_lives = g->lives; b->shot_score = g->score; b->shot_lives = g->lives; } }
        snprintf(b->status, sizeof b->status, "auto: target code %d col %d (w %.2f)  shots %u hits %u penalties %u", pick->code, pick->col, pick->wt, b->nshots, b->nhits, b->npen);
    } else {
        b->have_target = 0; if (!b->have_aim) { b->ax = 144; b->ay = 112; b->have_aim = 1; }
        if (b->trig_left > 0) b->trig_left -= (int)dfr;
        snprintf(b->status, sizeof b->status, "auto: nothing to shoot   shots %u hits %u penalties %u", b->nshots, b->nhits, b->npen);
    }
    b->nprev = nc < 64 ? nc : 64; memcpy(b->prev, cs, (size_t)b->nprev * sizeof *cs);
    *sx = b->ax; *sy = b->ay; *trig = b->trig_left > 0;
    *coin = *start = 0; credits(b, g, frame, coin, start);
}
