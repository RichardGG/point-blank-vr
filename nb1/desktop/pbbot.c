/* pbbot: the auto-player on its own, no window. Plays the MAME session of tools/play/pbplay.lua: reads the sprite state it publishes (--ipc DIR), writes aim.txt.
 *   pbbot --ipc DIR [--rules bot_rules.json] [--tables scene_tables.json --roms DIR] [--stats-out FILE] [--seconds N]
 *         [--mode training|beginner|expert|veryhard] [--stage-log FILE.jsonl --capture DIR] [--flow research/data/flow] [--flow-log FILE] [--unknown-dir DIR]   (per stage: what it learned, and pictures of the screens that open the stage)
 * Without pbbot, pbvr and pbdesk have the same player built in (--auto). */
#include "../nb1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(n) Sleep(n)
#else
#include <unistd.h>
#define SLEEP_MS(n) usleep((n) * 1000)
#endif
static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(len > 0 ? (size_t)len : 1); if (fread(b, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(b); return NULL; } fclose(f); *n = (size_t)len; return b;
}

/* ---- the choosing screen: the four cards, which stage each one starts (learned while playing), and which one to shoot */
static const int CBOX[4][4] = { { 26, 24, 138, 104 }, { 153, 24, 265, 104 }, { 26, 109, 138, 189 }, { 153, 109, 265, 189 } };     /* x0 y0 x1 y1 of the cards once they have zoomed in: top-left, top-right, bottom-left, bottom-right */
typedef struct { char key[16], text[220]; int n, comp, fail; } CardRec;
static CardRec ct[800]; static int nct;
static CardRec *ct_find(const char *key, const char *text, int create) {
    for (int i = 0; i < nct; i++) if (!strcmp(ct[i].key, key) && !strcmp(ct[i].text, text)) return &ct[i];
    if (!create || nct >= 800) return NULL; CardRec *r = &ct[nct++]; memset(r, 0, sizeof *r); snprintf(r->key, sizeof r->key, "%s", key); snprintf(r->text, sizeof r->text, "%s", text); return r; }
static void ct_load(const char *path) { FILE *f = fopen(path, "r"); if (!f) return; char l[400]; while (fgets(l, sizeof l, f)) { char *k = strtok(l, "\t"), *t = strtok(NULL, "\t"), *n = strtok(NULL, "\t"), *c = strtok(NULL, "\t"), *x = strtok(NULL, "\t\n"); if (!k || !t || !x) continue; CardRec *r = ct_find(k, t, 1); if (r) { r->n = atoi(n); r->comp = atoi(c); r->fail = atoi(x); } } fclose(f); }
static void ct_save(const char *path) { FILE *f = fopen(path, "w"); if (!f) return; for (int i = 0; i < nct; i++) fprintf(f, "%s\t%s\t%d\t%d\t%d\n", ct[i].key, ct[i].text, ct[i].n, ct[i].comp, ct[i].fail); fclose(f); }
/* how much the bot wants this card: the stage it is known to start (and how often that stage was completed), or a mild curiosity for a card it has not played */
static double ct_score(const char *key, const char *want) {
    double known = 0, comp = 0, fail = 0, wanted = 0; for (int i = 0; i < nct; i++) if (!strcmp(ct[i].key, key)) { known += ct[i].n; comp += ct[i].comp; fail += ct[i].fail; if (want && !strcmp(ct[i].text, want)) wanted += ct[i].n; }
    if (want) return wanted > 0 ? 2.0 + wanted / known : known > 0 ? 0.1 : 0.7;       /* a wanted stage first, then cards not yet known, never a card known to be something else */
    return known > 0 ? (comp + 1) / (comp + fail + 2) : 0.7; }
static void entry_box(const Nb1Entry *e, int *x0, int *y0, int *x1, int *y1) { *x0 = e->tiles[0].x; *y0 = e->tiles[0].y; *x1 = *x0; *y1 = *y0; for (int k = 0; k < e->ntiles; k++) { const Nb1Tile *tl = &e->tiles[k]; if (tl->x < *x0) *x0 = tl->x; if (tl->y < *y0) *y0 = tl->y; if (tl->x + tl->w > *x1) *x1 = tl->x + tl->w; if (tl->y + tl->h > *y1) *y1 = tl->y + tl->h; } }
static unsigned char pbs_bytes[NB1_STATE_FILE_BYTES];      /* the raw bytes of the state being handled, for --capture */
static char scene_req[200];       /* name of the MAME save state asked for at the latest capture (pbplay.lua saves it when this changes) */
static void scene_save(const char *dir, int *seq, unsigned fr, int phase, const char *text, unsigned fp, const Nb1GameInfo *gi, const char *tag, const char *extra) {
    if (strcmp(tag, "bg") || gi->stage == 0) { const char *b = strrchr(dir, '/'); b = b ? b + 1 : dir; snprintf(scene_req, sizeof scene_req, "cap_%s_%d", b, *seq); }      /* every tagged screen and the boot screens get a MAME state next to the RAM snapshot */
    char p[1400]; snprintf(p, sizeof p, "%s/scene_%04d.state", dir, *seq); FILE *f = fopen(p, "wb"); if (f) { fwrite(pbs_bytes, 1, NB1_STATE_FILE_BYTES, f); fclose(f); }
    snprintf(p, sizeof p, "%s/scene_log.tsv", dir); FILE *l = fopen(p, "a"); if (l) { fprintf(l, "%d\t%u\t%d\t%s\t%08x\t%d\t%d\t%d\t%s\t%s\n", *seq, fr, phase, text, fp, gi->stage, gi->mode, gi->diff, tag, extra); fclose(l); }
    (*seq)++;
}
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
/* read the four cards of a settled choosing screen: a key for the picture of each, and whether it carries the flag of a played stage */
static void read_cards(const Nb1State *st, const Nb1Entry *pe, int n, char key[4][16], int flagged[4]) {
    for (int q = 0; q < 4; q++) {
        int codes[16], nc = 0; flagged[q] = 0;
        for (int i = 0; i < n; i++) if (pe[i].ntiles) { int x0, y0, x1, y1; entry_box(&pe[i], &x0, &y0, &x1, &y1); int nt = pe[i].ntiles;
            int t0 = pe[i].tiles[0].t, code = (((t0 & 0x7ff) | (st->bank[(t0 >> 11) & 7] << 11)) + pe[i].offset) & 0x7fff;
            if (nt == 10) { if (x1 - x0 >= 44 && x1 - x0 <= 52 && y1 - y0 >= 60 && y1 - y0 <= 68 && abs((x0 + x1) / 2 - (CBOX[q][0] + CBOX[q][2]) / 2) <= 15 && abs((y0 + y1) / 2 - (CBOX[q][1] + CBOX[q][3]) / 2) <= 15) flagged[q] = 1; continue; }      /* the flag of a played card: a 10-tile 48x64 sprite on the middle of the card (3960 the white flag with the X, 3915 the red flag) */
            if (nt == 20 || nt == 43 || nt == 59 || nt == 63) continue;
            if (abs(x0 - CBOX[q][0]) <= 3 && abs(y0 - CBOX[q][1]) <= 3 && abs(x1 - CBOX[q][2]) <= 3 && abs(y1 - CBOX[q][3]) <= 3 && nc < 16) { int dup = 0; for (int k = 0; k < nc; k++) dup |= codes[k] == code; if (!dup) codes[nc++] = code; } }
        qsort(codes, (size_t)nc, sizeof(int), cmp_int); unsigned h = 2166136261u; for (int k = 0; k < nc; k++) h = (h ^ (unsigned)codes[k]) * 16777619u; snprintf(key[q], 16, nc ? "%08x" : "-", h); }
}
int main(int argc, char **argv) {
    int once = 0; const char *movie_dir = NULL; int movie_every = 20; const char *state_dir = NULL; const char *flow_dir = NULL, *flow_log = NULL, *unk_dir = NULL; unsigned seed = 1; const char *cards_file = NULL, *want_text = NULL, *scene_dir = NULL; const char *stage_log = NULL, *capdir = NULL; int want_mode = 0; const char *ipc = NULL, *rules = NULL, *tables = NULL, *roms_dir = NULL, *stats = NULL; double seconds = 0;
    for (int i = 1; i < argc; i++) { const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--ipc") && v) { ipc = v; i++; } else if (!strcmp(a, "--rules") && v) { rules = v; i++; } else if (!strcmp(a, "--tables") && v) { tables = v; i++; }
        else if (!strcmp(a, "--roms") && v) { roms_dir = v; i++; } else if (!strcmp(a, "--stats-out") && v) { stats = v; i++; } else if (!strcmp(a, "--seconds") && v) { seconds = atof(v); i++; }
        else if (!strcmp(a, "--once")) once = 1; else if (!strcmp(a, "--movie") && v) { movie_dir = v; i++; if (i + 1 < argc && argv[i + 1][0] != '-') movie_every = atoi(argv[++i]); } else if (!strcmp(a, "--save-states") && v) { state_dir = v; i++; } else if (!strcmp(a, "--flow") && v) { flow_dir = v; i++; } else if (!strcmp(a, "--flow-log") && v) { flow_log = v; i++; } else if (!strcmp(a, "--unknown-dir") && v) { unk_dir = v; i++; } else if (!strcmp(a, "--seed") && v) { seed = (unsigned)atoi(v); i++; } else if (!strcmp(a, "--stage-log") && v) { stage_log = v; i++; } else if (!strcmp(a, "--capture") && v) { capdir = v; i++; }
        else if (!strcmp(a, "--scene-dir") && v) { scene_dir = v; i++; } else if (!strcmp(a, "--cards") && v) { cards_file = v; i++; } else if (!strcmp(a, "--want") && v) { want_text = v; i++; }
        else if (!strcmp(a, "--mode") && v) { want_mode = !strcmp(v, "training") ? 1 : !strcmp(v, "beginner") ? 2 : !strcmp(v, "expert") ? 3 : !strcmp(v, "veryhard") ? 4 : 0; i++; } }
    if (!ipc) { fprintf(stderr, "usage: pbbot --ipc DIR [--rules bot_rules.json] [--tables scene_tables.json --roms DIR] [--stats-out FILE] [--seconds N] [--cards FILE] [--want SENTENCE]\n"); return 2; }
    if (cards_file) ct_load(cards_file);
    Nb1Bot *bot = nb1_bot_new(rules); nb1_bot_set_mode(bot, want_mode); nb1_bot_set_seed(bot, seed); Nb1Roms roms; int have_roms = roms_dir && nb1_roms_load(&roms, roms_dir) == 0;
    Nb1TableSet *ts = tables ? nb1_tableset_load(tables) : NULL; Nb1Selector *sel = NULL; if (ts) { if (have_roms) nb1_tableset_set_roms(ts, &roms); sel = nb1_selector_new(ts, 3); }
    char latest[1100], aim[1100], aim_tmp[1110], game[1100]; snprintf(latest, sizeof latest, "%s/latest.txt", ipc); snprintf(aim, sizeof aim, "%s/aim.txt", ipc); snprintf(aim_tmp, sizeof aim_tmp, "%s.tmp", aim); snprintf(game, sizeof game, "%s/game.txt", ipc);
    Nb1Flow *flow = flow_dir ? nb1_flow_load(flow_dir) : NULL; if (flow_dir && !flow) fprintf(stderr, "pbbot: cannot load the flow data in %s\n", flow_dir); int pick_quad = -1, prev_trig = 0, cur_quad = -1; int scene_phase = -1, scene_tagged = 0; unsigned scene_phase_fr = 0; unsigned scene_fp = 0, scene_cand = 0, scene_last_fr = 0; int scene_still = 0, scene_seq = 0; int card_goal = -1; unsigned card_steps = 0; char next_key[16] = "", goal_key[16] = "", pend_key[16] = "", cur_ckey[16] = ""; int pick_id = -1, pick_seq = 0, cur_pid = -1, card_wait = 0, card_set = 0, card_still = 0; unsigned card_sig = 0; char pick_all[6000] = "", cur_all[6000] = ""; double pick_ax = 0, pick_ay = 0, cur_ax = 0, cur_ay = 0; char pick_sp[4000] = "", cur_sp[4000] = ""; int res_failed = 0, res_complete = 0, lives_start = -1; char save_req[120] = "-"; char saved_for[220] = ""; unsigned intro_seen = 0; int unk_n = 0; const char *cur_name = NULL; char ident[220] = "", cur_ident[220] = "";
    typedef struct { int code, col; double n, hits, pen; } K; K *snap = NULL; int nsnap = 0; int cur_stage = -99, cur_diff = -99, cur_card = -99; unsigned stage_start = 0, cap_start = 0; int cap_n = 0, stage_scenes = 0; char scenes_seen[400] = "";
    unsigned last = 0; int have = 0, seq = 0; time_t t0 = time(NULL); unsigned long steps = 0; double sx = 144, sy = 112; int trig = 0, coin = 0, start = 0;
    while (seconds >= 0 && (seconds == 0 || difftime(time(NULL), t0) < seconds)) {
        size_t n; unsigned char *b = slurp(latest, &n); char name[256]; unsigned fr = 0;
        if (!b) { SLEEP_MS(5); continue; }
        b = realloc(b, n + 1); b[n] = 0; int ok = sscanf((char *)b, "%255s %u", name, &fr) == 2; free(b);
        if (!ok || (have && fr == last)) { SLEEP_MS(3); continue; }
        char path[1100]; snprintf(path, sizeof path, "%s/%s", ipc, name); b = slurp(path, &n);
        if (!b || n != NB1_STATE_FILE_BYTES + 12 || memcmp(b, "PBS1", 4) || memcmp(b + 4, b + n - 4, 4)) { free(b); SLEEP_MS(2); continue; }
        memcpy(pbs_bytes, b + 8, NB1_STATE_FILE_BYTES); Nb1State *st = nb1_state_from_bytes(b + 8, n - 12); free(b); last = fr; have = 1; if (!st) continue;
        Nb1GameInfo gi; memset(&gi, 0, sizeof gi); { unsigned gf; int l, s, m, sg, d; unsigned char *gb = slurp(game, &n); if (gb) { gb = realloc(gb, n + 1); gb[n] = 0;
            int cd = 0; if (sscanf((char *)gb, "%u %d %d %d %d %d %d", &gf, &l, &s, &m, &sg, &d, &cd) >= 6) { gi.valid = 1; gi.lives = l; gi.score = s; gi.mode = m; gi.stage = sg; gi.diff = d; gi.card = cd; } free(gb); } }
        if (sel) { const char *nm = NULL; if (nb1_selector_update(sel, st, st, &nm) && nm) { cur_name = nm; nb1_bot_set_scene(bot, nm); if (!strstr(scenes_seen, nm) && strlen(scenes_seen) + strlen(nm) + 3 < sizeof scenes_seen) { strcat(scenes_seen, stage_scenes++ ? "; " : ""); strcat(scenes_seen, nm); } } }
        if (flow && have_roms) { nb1_flow_step(flow, st, &roms, &gi, cur_name, fr); char ftb[200]; const char *ft = NULL; if (nb1_flow_type(flow) >= 0) { snprintf(ftb, sizeof ftb, "%s", nb1_flow_text(flow)); for (char *q = ftb; *q; q++) if (*q == '"' || *q == '\\') *q = '\''; ft = ftb; } nb1_bot_set_stage(bot, ft);
            uint8_t us[140 * 16 * 3]; if (unk_dir && nb1_flow_unknown(flow, us)) { char up[1200]; snprintf(up, sizeof up, "%s/unknown_%03d.ppm", unk_dir, unk_n++); FILE *uf = fopen(up, "wb"); if (uf) { fprintf(uf, "P6\n140 16\n255\n"); fwrite(us, 1, sizeof us, uf); fclose(uf); }
                snprintf(up, sizeof up, "%s/unknown_%03d_full.ppm", unk_dir, unk_n - 1); uint8_t *fr_rgb = malloc(NB1_W * NB1_H * 3); nb1_render_composite(st, st, &roms, fr_rgb); uf = fopen(up, "wb"); if (uf) { fprintf(uf, "P6\n%d %d\n255\n", NB1_W, NB1_H); fwrite(fr_rgb, 1, NB1_W * NB1_H * 3, uf); fclose(uf); } free(fr_rgb); }
            if (flow_log && steps % 3 == 0) { FILE *ff = fopen(flow_log, "a"); if (ff) { fprintf(ff, "%u\t%d\t%d\t%s\t%s\t%s\t%d\n", fr, nb1_flow_phase(flow), nb1_flow_type(flow), cur_name ? cur_name : "", nb1_flow_locked_table(flow) ? nb1_flow_locked_table(flow) : "", nb1_flow_type(flow) >= 0 ? nb1_flow_text(flow) : "", gi.stage); fclose(ff); } } }
        if (scene_dir) {        /* scene capture: a raw state whenever the background set-up (sprite banks + tilemap control + flow phase) changes and holds, every 10 s, and at fixed moments of each screen (tagged) */
            unsigned fp = 2166136261u; for (int k = 0; k < 8; k++) fp = (fp ^ st->bank[k]) * 16777619u; for (int k = 0x10; k < 0x14; k++) fp = (fp ^ st->ctl[k]) * 16777619u; fp = (fp ^ (unsigned)(flow ? nb1_flow_phase(flow) : 9)) * 16777619u;
            int ph = flow ? nb1_flow_phase(flow) : -1; if (ph != scene_phase) { scene_phase = ph; scene_phase_fr = fr; scene_tagged = 0; }
            const char *ttxt = flow && nb1_flow_type(flow) >= 0 ? nb1_flow_text(flow) : "";
            const char *tag = NULL; unsigned since = fr - scene_phase_fr;
            if (ph == NB1_FLOW_INTRO && since >= 90 && !(scene_tagged & 1)) { scene_tagged |= 1; tag = "rules"; }              /* the rules screen with its text drawn */
            else if (ph == NB1_FLOW_JUDGEMENT && since >= 60 && !(scene_tagged & 2)) { scene_tagged |= 2; tag = "result"; }      /* the result panel */
            else if (ph == NB1_FLOW_ATTRACT && since >= 120 && !(scene_tagged & 4)) { scene_tagged |= 4; tag = "attract"; }
            else if (ph == NB1_FLOW_PLAY && since >= 240 && !(scene_tagged & 8)) { scene_tagged |= 8; tag = "play"; }
            if (fp == scene_cand) scene_still++; else { scene_cand = fp; scene_still = 0; }
            if (!tag && scene_still >= 10 && (fp != scene_fp || fr - scene_last_fr >= 600)) { tag = "bg"; scene_fp = fp; scene_last_fr = fr; scene_still = 0; }
            if (tag) scene_save(scene_dir, &scene_seq, fr, ph, ttxt, fp, &gi, tag, "");
        }
        card_wait = 0;
        if (flow && nb1_flow_phase(flow) == NB1_FLOW_CARD_SELECT) {      /* the choosing screen: the four cards fly in and zoom; they can be shot only once they stand still */
            static Nb1Entry pe[256]; int n = nb1_sprite_entries(st, pe), big = 0, o = 0; unsigned sig = 2166136261u; char tmp[600]; static char allsp[6000]; int oa = 0; allsp[0] = 0; tmp[0] = 0;
            for (int i = 0; i < n; i++) if (pe[i].ntiles) { int x0 = pe[i].tiles[0].x, y0 = pe[i].tiles[0].y, x1 = x0, y1 = y0; for (int k = 0; k < pe[i].ntiles; k++) { const Nb1Tile *tl = &pe[i].tiles[k]; if (tl->x < x0) x0 = tl->x; if (tl->y < y0) y0 = tl->y; if (tl->x + tl->w > x1) x1 = tl->x + tl->w; if (tl->y + tl->h > y1) y1 = tl->y + tl->h; }
                { int t0 = pe[i].tiles[0].t, code = (((t0 & 0x7ff) | (st->bank[(t0 >> 11) & 7] << 11)) + pe[i].offset) & 0x7fff; oa += snprintf(allsp + oa, sizeof allsp - oa - 1, "%s[%d,%d,%d,%d,%d,%d,%d]", oa ? "," : "", code, x0, y0, x1, y1, pe[i].ntiles, pe[i].palette); }
                if (x1 - x0 >= 80 && x1 - x0 < 250 && y1 - y0 >= 60 && big < 8) { big++; sig = (sig ^ (unsigned)(x0 * 7 + y0 * 131 + x1 * 17 + y1 * 257)) * 16777619u; o += snprintf(tmp + o, sizeof tmp - o - 1, "%s[%d,%d,%d,%d]", o ? "," : "", x0, y0, x1, y1); } }
            if (big >= 3 && sig == card_sig) card_still++; else { card_still = 0; card_sig = sig; }
            int settled = card_still >= 45;      /* the cards stand still, and the flags of the played ones have had time to drop in and wave */ if (getenv("NB1_CARD_DEBUG")) fprintf(stderr, "card fr %u big %d still %d sig %08x\n", fr, big, card_still, sig);
            if (settled) { snprintf(pick_sp, sizeof pick_sp, "%s", tmp); snprintf(pick_all, sizeof pick_all, "%s", allsp); }      /* the flags of the played cards drop in after the cards settle: keep the newest picture of the screen */
            if (settled && !card_set) { card_set = 1; pick_id = pick_seq++; pick_quad = -1;          /* the cards stand still: this is what was on offer */
                if (capdir && have_roms) { uint8_t *rgb = malloc(NB1_W * NB1_H * 3); nb1_render_composite(st, st, &roms, rgb); char pp[1400]; snprintf(pp, sizeof pp, "%s/offer_%03d.ppm", capdir, pick_id); { char sp2[1400]; snprintf(sp2, sizeof sp2, "%s/offer_%03d.state", capdir, pick_id); FILE *sf = fopen(sp2, "wb"); if (sf) { fwrite(pbs_bytes, 1, NB1_STATE_FILE_BYTES, sf); fclose(sf); } } FILE *pf = fopen(pp, "wb"); if (pf) { fprintf(pf, "P6\n%d %d\n255\n", NB1_W, NB1_H); fwrite(rgb, 1, NB1_W * NB1_H * 3, pf); fclose(pf); } free(rgb); } }
            if (settled && card_goal < 0 && card_set) {         /* choose the card to shoot: the best one that has not been played */
                char ck[4][16]; int fl[4], nfl = 0; read_cards(st, pe, n, ck, fl); for (int q = 0; q < 4; q++) nfl += fl[q];
                double best = -1; for (int q = 0; q < 4; q++) { if (!strcmp(ck[q], "-")) continue; double sc = ct_score(ck[q], want_text) - (nfl < 4 && fl[q] ? 100 : 0); if (sc > best) { best = sc; card_goal = q; } }
                if (card_goal >= 0 && scene_dir) { char ex[200]; snprintf(ex, sizeof ex, "%s,%s,%s,%s;%d;%d%d%d%d", ck[0], ck[1], ck[2], ck[3], card_goal, fl[0], fl[1], fl[2], fl[3]); scene_save(scene_dir, &scene_seq, fr, NB1_FLOW_CARD_SELECT, "", 0, &gi, "choose", ex); }
                if (card_goal >= 0) { snprintf(goal_key, sizeof goal_key, "%s", ck[card_goal]); next_key[0] = 0; if (nfl == 2 && !fl[card_goal]) for (int q = 0; q < 4; q++) if (q != card_goal && !fl[q]) snprintf(next_key, sizeof next_key, "%s", ck[q]);      /* two cards left: the other one is what the game picks by itself on the last screen */
                     card_steps = 0; if (getenv("NB1_CARD_DEBUG")) fprintf(stderr, "cards %s%s %s%s %s%s %s%s -> %d\n", ck[0], fl[0] ? "*" : "", ck[1], fl[1] ? "*" : "", ck[2], fl[2] ? "*" : "", ck[3], fl[3] ? "*" : "", card_goal); } }
            if (!settled) card_wait = 1;
            if (trig && !prev_trig && card_set && settled && capdir && have_roms) { uint8_t *rgb = malloc(NB1_W * NB1_H * 3); nb1_render_composite(st, st, &roms, rgb); char pp[1400]; snprintf(pp, sizeof pp, "%s/shot_%03d_%u.ppm", capdir, pick_id, fr); FILE *pf = fopen(pp, "wb"); if (pf) { fprintf(pf, "P6\n%d %d\n255\n", NB1_W, NB1_H); fwrite(rgb, 1, NB1_W * NB1_H * 3, pf); fclose(pf); } free(rgb); }
            if (trig && !prev_trig && card_set && settled) { pick_quad = (sy > 112) * 2 + (sx > 144); pick_ax = sx; pick_ay = sy; }
        } else { card_set = 0; card_still = 0; card_sig = 0; }
        prev_trig = trig;
        { static Nb1Entry ee[256]; int ne = nb1_sprite_entries(st, ee); for (int i = 0; i < ne; i++) if (ee[i].ntiles) { int t0 = ee[i].tiles[0].t, code = (((t0 & 0x7ff) | (st->bank[(t0 >> 11) & 7] << 11)) + ee[i].offset) & 0x7fff; if (code == 8144) res_failed = 1; else if (code == 6642) res_complete = 1; } }
        if (flow && state_dir && nb1_flow_phase(flow) == NB1_FLOW_INTRO && nb1_flow_type(flow) >= 0) {                  /* stage lab: a MAME state at the start of every stage sentence, once */
            const char *tx = nb1_flow_text(flow); if (strcmp(saved_for, tx)) { snprintf(saved_for, sizeof saved_for, "%s", tx); intro_seen = fr; }
            if (fr - intro_seen >= 10 && fr - intro_seen < 40 && !strcmp(save_req, "-")) { char slug[100]; int k = 0; for (const char *c = tx; *c && k < 90; c++) slug[k++] = (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ? *c : (*c >= 'a' && *c <= 'z') ? *c - 32 : '_'; slug[k] = 0;
                char sp[1300]; snprintf(sp, sizeof sp, "%s/ptblank/stg_%s.sta", state_dir, slug); FILE *sf = fopen(sp, "rb"); if (sf) fclose(sf); else snprintf(save_req, sizeof save_req, "stg_%s", slug); } }
        else if (strcmp(save_req, "-") && flow && nb1_flow_phase(flow) != NB1_FLOW_INTRO && fr - intro_seen > 120) snprintf(save_req, sizeof save_req, "-");
        if (flow && capdir && have_roms && nb1_flow_phase(flow) == NB1_FLOW_INTRO && nb1_flow_type(flow) >= 0) {          /* the rule screen of each stage, once or twice: the card beside the banner shows the target and the "Don't shoot" icons */
            static char shot_for[220]; static unsigned shot_frame; const char *tx = nb1_flow_text(flow);
            if (strcmp(shot_for, tx) || fr - shot_frame == 90 || (fr - shot_frame > 90 && 0)) { if (strcmp(shot_for, tx)) { snprintf(shot_for, sizeof shot_for, "%s", tx); shot_frame = fr; } }
            if (fr - shot_frame == 8 || fr - shot_frame == 90 || fr - shot_frame == 91) { char slug[100]; int k = 0; for (const char *c = tx; *c && k < 90; c++) slug[k++] = (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ? *c : (*c >= 'a' && *c <= 'z') ? *c - 32 : '_'; slug[k] = 0;
                uint8_t *rgb = malloc(NB1_W * NB1_H * 3); nb1_render_composite(st, st, &roms, rgb); char pp[1400]; snprintf(pp, sizeof pp, "%s/intro_%s_%c.ppm", capdir, slug, fr - shot_frame == 8 ? 'a' : 'b'); FILE *pf = fopen(pp, "wb"); if (pf) { fprintf(pf, "P6\n%d %d\n255\n", NB1_W, NB1_H); fwrite(rgb, 1, NB1_W * NB1_H * 3, pf); fclose(pf); } free(rgb); } }
        if (flow && nb1_flow_type(flow) >= 0) snprintf(ident, sizeof ident, "%s", nb1_flow_text(flow)); else snprintf(ident, sizeof ident, "slot%d", gi.stage);
        for (char *q = ident; *q; q++) if (*q == '"' || *q == '\\') *q = '\'';                                   /* the ident goes into JSON files */
        if (gi.valid && stage_log && strcmp(ident, cur_ident)) {        /* a new stage, difficulty or card pick: close the last record (what the bot learned meanwhile) and start a capture */
            if (cur_ident[0]) { FILE *lf = fopen(stage_log, "a"); if (lf) { fprintf(lf, "{\"text\":\"%s\",\"diff\":%d,\"stage\":%d,\"card\":%d,\"ckey\":\"%s\",\"pid\":%d,\"quad\":%d,\"block\":%d,\"aim\":[%.0f,%.0f],\"pick_sprites\":[%s],\"offer\":[%s],\"from_frame\":%u,\"to_frame\":%u,\"scenes\":\"%s\",\"score\":%d,\"lives\":%d,\"lives_start\":%d,\"failed\":%d,\"complete\":%d,\"keys\":[", cur_ident, cur_diff, cur_stage, cur_card, cur_ckey, cur_pid, cur_quad, cur_stage > 0 ? (cur_stage - 1) / 4 : -1, cur_ax, cur_ay, cur_sp, cur_all, stage_start, fr, scenes_seen, gi.score, gi.lives, lives_start, res_failed, res_complete);
                int first = 1, c, k; double n, h, p; for (int i = 0; nb1_bot_key(bot, i, &c, &k, &n, &h, &p); i++) { double n0 = 0, h0 = 0, p0 = 0; for (int j = 0; j < nsnap; j++) if (snap[j].code == c && snap[j].col == k) { n0 = snap[j].n; h0 = snap[j].hits; p0 = snap[j].pen; }
                    if (n > n0) { fprintf(lf, "%s[%d,%d,%.0f,%.0f,%.0f]", first ? "" : ",", c, k, n - n0, h - h0, p - p0); first = 0; } }
                fprintf(lf, "]}\n"); fclose(lf); if (once && strncmp(cur_ident, "slot", 4) && fr - stage_start > 300) seconds = -1; } }
            free(snap); nsnap = 0; { int c, k, i; double n, h, p; for (i = 0; nb1_bot_key(bot, i, &c, &k, &n, &h, &p); i++); snap = malloc((size_t)(i ? i : 1) * sizeof *snap); for (int j = 0; j < i; j++) { nb1_bot_key(bot, j, &c, &k, &n, &h, &p); snap[j] = (K){ c, k, n, h, p }; } nsnap = i; }
            if (pend_key[0]) { CardRec *cr = ct_find(pend_key, cur_ident, 1); if (cr) { cr->comp += res_complete; cr->fail += res_failed; if (cards_file) ct_save(cards_file); } pend_key[0] = 0; }      /* what the stage the card started came to */
            cur_ckey[0] = 0; if (card_goal < 0 && next_key[0] && strncmp(ident, "slot", 4) && !strncmp(cur_ident, "slot", 4)) { snprintf(goal_key, sizeof goal_key, "%s", next_key); next_key[0] = 0; card_goal = 4; }
            if (card_goal >= 0 && strncmp(ident, "slot", 4)) { snprintf(cur_ckey, sizeof cur_ckey, "%s", goal_key); snprintf(pend_key, sizeof pend_key, "%s", goal_key); CardRec *cr = ct_find(goal_key, ident, 1); if (cr) { cr->n++; if (cards_file) ct_save(cards_file); } }      /* the card shot, and the stage that followed */
            card_goal = -1; card_steps = 0;
            res_failed = res_complete = 0; lives_start = gi.lives; cur_quad = pick_quad; cur_pid = pick_id; snprintf(cur_sp, sizeof cur_sp, "%s", pick_sp); snprintf(cur_all, sizeof cur_all, "%s", pick_all); cur_ax = pick_ax; cur_ay = pick_ay; pick_id = -1; pick_quad = -1; pick_sp[0] = 0; card_set = 0; cur_stage = gi.stage; cur_diff = gi.diff; cur_card = gi.card; snprintf(cur_ident, sizeof cur_ident, "%s", ident); stage_start = fr; scenes_seen[0] = 0; stage_scenes = 0; cap_start = fr; cap_n = 0;
        }
        if (capdir && have_roms && cap_n < 10 && fr >= cap_start + (unsigned)cap_n * 24) {                       /* pictures of the first seconds of the stage: the rules screen is among them */
            uint8_t *rgb = malloc(NB1_W * NB1_H * 3); nb1_render_composite(st, st, &roms, rgb); char pp[1200]; snprintf(pp, sizeof pp, "%s/d%d_n%d_c%d_%02d.ppm", capdir, cur_diff, cur_stage, cur_card, cap_n);
            FILE *pf = fopen(pp, "wb"); if (pf) { fprintf(pf, "P6\n%d %d\n255\n", NB1_W, NB1_H); fwrite(rgb, 1, NB1_W * NB1_H * 3, pf); fclose(pf); } free(rgb); cap_n++; }
        nb1_bot_step(bot, st, &gi, fr, &sx, &sy, &trig, &coin, &start); if (card_wait) trig = 0; if (card_goal >= 0 && flow && nb1_flow_phase(flow) == NB1_FLOW_CARD_SELECT && !card_wait) { sx = (CBOX[card_goal][0] + CBOX[card_goal][2]) / 2.0; sy = (CBOX[card_goal][1] + CBOX[card_goal][3]) / 2.0 + 6; trig = (card_steps % 24) >= 4 && (card_steps % 24) < 8; card_steps++; }      /* shoot the card it chose, in the middle, a few times if the first shot is missed */ Nb1State *st0 = st; steps++;
        if (movie_dir && have_roms && steps % (unsigned long)(movie_every / 2 > 0 ? movie_every / 2 : 1) == 0) {                 /* every Nth published state: the picture with the gun's aim marked (red cross; yellow while the trigger is held) */
            static int mn; uint8_t *rgb = malloc(NB1_W * NB1_H * 3); nb1_render_composite(st0, st0, &roms, rgb); int ax = (int)(sx + 0.5), ay = (int)(sy + 0.5);
            for (int d = -6; d <= 6; d++) { int xs[2][2] = { { ax + d, ay }, { ax, ay + d } }; for (int q = 0; q < 2; q++) { int x = xs[q][0], y = xs[q][1]; if (x >= 0 && x < NB1_W && y >= 0 && y < NB1_H) { uint8_t *px = rgb + (y * NB1_W + x) * 3; px[0] = 255; px[1] = trig ? 255 : 0; px[2] = 0; } } }
            char mp[1300]; snprintf(mp, sizeof mp, "%s/m_%05d.ppm", movie_dir, mn++); FILE *mf = fopen(mp, "wb"); if (mf) { fprintf(mf, "P6\n%d %d\n255\n", NB1_W, NB1_H); fwrite(rgb, 1, NB1_W * NB1_H * 3, mf); fclose(mf); } free(rgb); }
        FILE *f = fopen(aim_tmp, "w"); if (f) { fprintf(f, "%d %.2f %.2f %d %d %d 0 - %s\n", ++seq, sx, sy, trig, coin, start, scene_req[0] ? scene_req : save_req); fclose(f); remove(aim); rename(aim_tmp, aim); }
        free(st0);
        if (steps % 120 == 0) { fprintf(stderr, "%s\n", nb1_bot_status(bot)); if (stats) nb1_bot_save_stats(bot, stats); }
    }
    if (stats) nb1_bot_save_stats(bot, stats); fprintf(stderr, "%s\n", nb1_bot_status(bot)); return 0;
}
