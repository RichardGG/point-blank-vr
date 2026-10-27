#pragma GCC diagnostic ignored "-Wmisleading-indentation"
/* The game-flow tracker: keeps our own picture of where the game is (attract, choosing a stage card, the rule screen, playing, judgement) and which stage it is.
 * The stage is identified by the rule banner that opens it ("SHOOT 28 NINJAS OR MORE."): drawn with a fixed font on a fixed stone texture, it matches a reference strip
 * almost exactly (research/data/flow/banners.*). The stage counter in RAM says when a new stage begins; the banner says which one it is; the depth table that the art
 * selector chose most often while playing is then held for the whole stage. See nb1.h. */
#include "nb1.h"
#include "nb1_json.h"
#include "nb1_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SW 140
#define SH 16
typedef struct { char text[160], goal[200], dont[200], world[64]; uint8_t strip[SW * SH * 3]; } Banner;
struct Nb1Flow {
    Banner *b; int nb; double thr;
    int phase, slot, type, calls, have_unknown, last_diff; unsigned phase_frame, slot_frame, play_frames;
    uint8_t unknown[SW * SH * 3], pend[SW * SH * 3]; int pend_ok, sent_ok; uint8_t sent[SW * SH * 3]; char unknown_name[48];
    char names[16][64]; int counts[16]; int nnames; char locked[64];
};

static uint8_t *slurp_f(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(len ? (size_t)len : 1); if (fread(b, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(b); return NULL; } fclose(f); *n = (size_t)len; return b;
}
Nb1Flow *nb1_flow_load(const char *dir) {
    char p[1100]; size_t n, m; snprintf(p, sizeof p, "%s/banners.json", dir); uint8_t *jb = slurp_f(p, &n); snprintf(p, sizeof p, "%s/banners.bin", dir); uint8_t *bin = slurp_f(p, &m);
    if (!jb || !bin) { free(jb); free(bin); return NULL; }
    char *txt = malloc(n + 1); memcpy(txt, jb, n); txt[n] = 0; free(jb); Json *j = json_parse(txt); free(txt); const Json *bj = j ? json_get(j, "banners") : NULL;
    if (!bj || bj->t != J_ARR || m < (size_t)bj->n * SW * SH * 3) { json_free(j); free(bin); return NULL; }
    Nb1Flow *f = calloc(1, sizeof *f); f->nb = bj->n; f->b = calloc((size_t)(f->nb ? f->nb : 1), sizeof *f->b); f->thr = json_get(j, "threshold") ? json_get(j, "threshold")->num : 3.0; f->type = -1; f->phase = NB1_FLOW_ATTRACT; f->slot = -1;
    for (int i = 0; i < f->nb; i++) { const Json *o = bj->kid[i], *v; Banner *b = &f->b[i];
        if ((v = json_get(o, "text")) && v->t == J_STR) snprintf(b->text, sizeof b->text, "%s", v->str); if ((v = json_get(o, "goal")) && v->t == J_STR) snprintf(b->goal, sizeof b->goal, "%s", v->str);
        if ((v = json_get(o, "dont_shoot")) && v->t == J_STR) snprintf(b->dont, sizeof b->dont, "%s", v->str); if ((v = json_get(o, "world")) && v->t == J_STR) snprintf(b->world, sizeof b->world, "%s", v->str);
        memcpy(b->strip, bin + (size_t)i * SW * SH * 3, SW * SH * 3); }
    json_free(j); free(bin); return f;
}
void nb1_flow_free(Nb1Flow *f) { if (f) { free(f->b); free(f); } }
static int is_ui(const char *name) { return !name || !name[0] || strstr(name, "ui_") || !strncmp(name, "flat UI", 7); }
static void strip_of(const uint8_t *rgb, uint8_t *out) {                                   /* arcade x 4..284, y 6..38, halved */
    for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) for (int c = 0; c < 3; c++) {
        int s = 0; for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) s += rgb[((6 + 2 * y + dy) * NB1_W + 4 + 2 * x + dx) * 3 + c]; out[(y * SW + x) * 3 + c] = (uint8_t)((s + 2) / 4); }
}
static double strip_mae(const uint8_t *a, const uint8_t *b) { long s = 0; for (int i = 0; i < SW * SH * 3; i++) s += abs(a[i] - b[i]); return (double)s / (SW * SH * 3); }
static int banner_present(const uint8_t *rgb) {                                              /* the grey stone banner: mostly grey pixels in the strip */
    int n = 0, t = 0; for (int y = 8; y < 30; y += 2) for (int x = 30; x < 260; x += 2) { const uint8_t *p = rgb + (y * NB1_W + x) * 3; int r = p[0], g = p[1], b = p[2]; t++; if (abs(r - g) < 14 && abs(g - b) < 14 && r > 140 && r < 235) n++; }
    return n * 100 > t * 45;
}
void nb1_flow_step(Nb1Flow *f, const Nb1State *st, const Nb1Roms *roms, const Nb1GameInfo *g, const char *table_name, unsigned frame) {
    if (!f) return; f->calls++;
    if (g && g->valid) {
        if (g->stage != f->slot) { f->slot = g->stage; f->slot_frame = frame; f->phase = g->stage == 0 ? NB1_FLOW_ATTRACT : NB1_FLOW_CARD_SELECT; f->phase_frame = frame; f->type = -1; f->play_frames = 0; f->nnames = 0; f->locked[0] = 0; f->have_unknown = 0; }
        else if (g->stage == 0) f->phase = NB1_FLOW_ATTRACT;
    }
    int look = roms && st && (f->phase == NB1_FLOW_CARD_SELECT || f->phase == NB1_FLOW_INTRO || f->phase == NB1_FLOW_ATTRACT) && f->calls % 3 == 0;
    if (look) {
        uint8_t *rgb = malloc(NB1_W * NB1_H * 3); nb1_render_composite(st, st, roms, rgb);
        if (banner_present(rgb)) {
            uint8_t cur[SW * SH * 3]; strip_of(rgb, cur); int best = -1; double bm = 1e9;
            for (int i = 0; i < f->nb; i++) { double m = strip_mae(cur, f->b[i].strip); if (m < bm) { bm = m; best = i; } }
            if (f->phase != NB1_FLOW_INTRO) { f->phase = NB1_FLOW_INTRO; f->phase_frame = frame; }
            if (best >= 0 && bm <= f->thr) f->type = best;
            else if (f->type == -1 || f->type == -2) {                                                                              /* a banner we have no picture of: after two equal looks in a row (not the banner sliding in) */
                if (f->pend_ok && strip_mae(cur, f->pend) < 0.5 && !f->have_unknown && (!f->sent_ok || strip_mae(cur, f->sent) > 1.0)) { f->type = -2; memcpy(f->unknown, cur, sizeof cur); f->have_unknown = 1; memcpy(f->sent, cur, sizeof cur); f->sent_ok = 1; }
                memcpy(f->pend, cur, sizeof cur); f->pend_ok = 1; }
        }
        free(rgb);
    }
    if (f->phase == NB1_FLOW_INTRO && f->type != -1 && !is_ui(table_name) && frame - f->phase_frame > 20) { f->phase = NB1_FLOW_PLAY; f->phase_frame = frame; }
    if (f->phase == NB1_FLOW_PLAY) {
        f->play_frames += 2;
        if (table_name && !is_ui(table_name)) {                                                   /* which world table does the art selector choose while this stage is played? */
            int k = 0; for (; k < f->nnames; k++) if (!strcmp(f->names[k], table_name)) break;
            if (k == f->nnames && f->nnames < 16) { snprintf(f->names[k], sizeof f->names[k], "%s", table_name); f->counts[k] = 0; f->nnames++; }
            if (k < f->nnames) { f->counts[k]++; int bi = 0; for (int q = 1; q < f->nnames; q++) if (f->counts[q] > f->counts[bi]) bi = q; if (f->counts[bi] >= 12) snprintf(f->locked, sizeof f->locked, "%s", f->names[bi]); }
        } else if (table_name && strstr(table_name, "ui_judgement") && f->play_frames > 400) { f->phase = NB1_FLOW_JUDGEMENT; f->phase_frame = frame; }
    }
}
int nb1_flow_phase(const Nb1Flow *f) { return f ? f->phase : NB1_FLOW_ATTRACT; }
int nb1_flow_type(const Nb1Flow *f) { return f ? f->type : -1; }
const char *nb1_flow_text(const Nb1Flow *f) { return f && f->type >= 0 ? f->b[f->type].text : f && f->type == -2 ? "(unknown stage rule)" : ""; }
const char *nb1_flow_goal(const Nb1Flow *f) { return f && f->type >= 0 ? f->b[f->type].goal : ""; }
const char *nb1_flow_world(const Nb1Flow *f) { return f && f->type >= 0 && f->b[f->type].world[0] ? f->b[f->type].world : NULL; }   /* the depth table this stage was played with before */
const char *nb1_flow_locked_table(const Nb1Flow *f) { return f && f->phase >= NB1_FLOW_PLAY && f->locked[0] ? f->locked : NULL; }
int nb1_flow_unknown(Nb1Flow *f, uint8_t *strip /* 140 x 16 RGB */) { if (!f || !f->have_unknown) return 0; memcpy(strip, f->unknown, sizeof f->unknown); f->have_unknown = 0; return 1; }
