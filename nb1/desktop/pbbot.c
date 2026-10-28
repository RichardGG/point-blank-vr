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
int main(int argc, char **argv) {
    int once = 0; const char *movie_dir = NULL; int movie_every = 20; const char *state_dir = NULL; const char *flow_dir = NULL, *flow_log = NULL, *unk_dir = NULL; unsigned seed = 1; const char *stage_log = NULL, *capdir = NULL; int want_mode = 0; const char *ipc = NULL, *rules = NULL, *tables = NULL, *roms_dir = NULL, *stats = NULL; double seconds = 0;
    for (int i = 1; i < argc; i++) { const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--ipc") && v) { ipc = v; i++; } else if (!strcmp(a, "--rules") && v) { rules = v; i++; } else if (!strcmp(a, "--tables") && v) { tables = v; i++; }
        else if (!strcmp(a, "--roms") && v) { roms_dir = v; i++; } else if (!strcmp(a, "--stats-out") && v) { stats = v; i++; } else if (!strcmp(a, "--seconds") && v) { seconds = atof(v); i++; }
        else if (!strcmp(a, "--once")) once = 1; else if (!strcmp(a, "--movie") && v) { movie_dir = v; i++; if (i + 1 < argc && argv[i + 1][0] != '-') movie_every = atoi(argv[++i]); } else if (!strcmp(a, "--save-states") && v) { state_dir = v; i++; } else if (!strcmp(a, "--flow") && v) { flow_dir = v; i++; } else if (!strcmp(a, "--flow-log") && v) { flow_log = v; i++; } else if (!strcmp(a, "--unknown-dir") && v) { unk_dir = v; i++; } else if (!strcmp(a, "--seed") && v) { seed = (unsigned)atoi(v); i++; } else if (!strcmp(a, "--stage-log") && v) { stage_log = v; i++; } else if (!strcmp(a, "--capture") && v) { capdir = v; i++; }
        else if (!strcmp(a, "--mode") && v) { want_mode = !strcmp(v, "training") ? 1 : !strcmp(v, "beginner") ? 2 : !strcmp(v, "expert") ? 3 : !strcmp(v, "veryhard") ? 4 : 0; i++; } }
    if (!ipc) { fprintf(stderr, "usage: pbbot --ipc DIR [--rules bot_rules.json] [--tables scene_tables.json --roms DIR] [--stats-out FILE] [--seconds N]\n"); return 2; }
    Nb1Bot *bot = nb1_bot_new(rules); nb1_bot_set_mode(bot, want_mode); nb1_bot_set_seed(bot, seed); Nb1Roms roms; int have_roms = roms_dir && nb1_roms_load(&roms, roms_dir) == 0;
    Nb1TableSet *ts = tables ? nb1_tableset_load(tables) : NULL; Nb1Selector *sel = NULL; if (ts) { if (have_roms) nb1_tableset_set_roms(ts, &roms); sel = nb1_selector_new(ts, 3); }
    char latest[1100], aim[1100], aim_tmp[1110], game[1100]; snprintf(latest, sizeof latest, "%s/latest.txt", ipc); snprintf(aim, sizeof aim, "%s/aim.txt", ipc); snprintf(aim_tmp, sizeof aim_tmp, "%s.tmp", aim); snprintf(game, sizeof game, "%s/game.txt", ipc);
    Nb1Flow *flow = flow_dir ? nb1_flow_load(flow_dir) : NULL; if (flow_dir && !flow) fprintf(stderr, "pbbot: cannot load the flow data in %s\n", flow_dir); int pick_quad = -1, prev_trig = 0, cur_quad = -1; int pick_id = -1, pick_seq = 0, cur_pid = -1, card_wait = 0, card_set = 0, card_still = 0; unsigned card_sig = 0; char pick_all[6000] = "", cur_all[6000] = ""; double pick_ax = 0, pick_ay = 0, cur_ax = 0, cur_ay = 0; char pick_sp[4000] = "", cur_sp[4000] = ""; int res_failed = 0, res_complete = 0, lives_start = -1; char save_req[120] = "-"; char saved_for[220] = ""; unsigned intro_seen = 0; int unk_n = 0; const char *cur_name = NULL; char ident[220] = "", cur_ident[220] = "";
    typedef struct { int code, col; double n, hits, pen; } K; K *snap = NULL; int nsnap = 0; int cur_stage = -99, cur_diff = -99, cur_card = -99; unsigned stage_start = 0, cap_start = 0; int cap_n = 0, stage_scenes = 0; char scenes_seen[400] = "";
    unsigned last = 0; int have = 0, seq = 0; time_t t0 = time(NULL); unsigned long steps = 0; double sx = 144, sy = 112; int trig = 0, coin = 0, start = 0;
    while (seconds >= 0 && (seconds == 0 || difftime(time(NULL), t0) < seconds)) {
        size_t n; unsigned char *b = slurp(latest, &n); char name[256]; unsigned fr = 0;
        if (!b) { SLEEP_MS(5); continue; }
        b = realloc(b, n + 1); b[n] = 0; int ok = sscanf((char *)b, "%255s %u", name, &fr) == 2; free(b);
        if (!ok || (have && fr == last)) { SLEEP_MS(3); continue; }
        char path[1100]; snprintf(path, sizeof path, "%s/%s", ipc, name); b = slurp(path, &n);
        if (!b || n != NB1_STATE_FILE_BYTES + 12 || memcmp(b, "PBS1", 4) || memcmp(b + 4, b + n - 4, 4)) { free(b); SLEEP_MS(2); continue; }
        Nb1State *st = nb1_state_from_bytes(b + 8, n - 12); free(b); last = fr; have = 1; if (!st) continue;
        Nb1GameInfo gi; memset(&gi, 0, sizeof gi); { unsigned gf; int l, s, m, sg, d; unsigned char *gb = slurp(game, &n); if (gb) { gb = realloc(gb, n + 1); gb[n] = 0;
            int cd = 0; if (sscanf((char *)gb, "%u %d %d %d %d %d %d", &gf, &l, &s, &m, &sg, &d, &cd) >= 6) { gi.valid = 1; gi.lives = l; gi.score = s; gi.mode = m; gi.stage = sg; gi.diff = d; gi.card = cd; } free(gb); } }
        if (sel) { const char *nm = NULL; if (nb1_selector_update(sel, st, st, &nm) && nm) { cur_name = nm; nb1_bot_set_scene(bot, nm); if (!strstr(scenes_seen, nm) && strlen(scenes_seen) + strlen(nm) + 3 < sizeof scenes_seen) { strcat(scenes_seen, stage_scenes++ ? "; " : ""); strcat(scenes_seen, nm); } } }
        if (flow && have_roms) { nb1_flow_step(flow, st, &roms, &gi, cur_name, fr); char ftb[200]; const char *ft = NULL; if (nb1_flow_type(flow) >= 0) { snprintf(ftb, sizeof ftb, "%s", nb1_flow_text(flow)); for (char *q = ftb; *q; q++) if (*q == '"' || *q == '\\') *q = '\''; ft = ftb; } nb1_bot_set_stage(bot, ft);
            uint8_t us[140 * 16 * 3]; if (unk_dir && nb1_flow_unknown(flow, us)) { char up[1200]; snprintf(up, sizeof up, "%s/unknown_%03d.ppm", unk_dir, unk_n++); FILE *uf = fopen(up, "wb"); if (uf) { fprintf(uf, "P6\n140 16\n255\n"); fwrite(us, 1, sizeof us, uf); fclose(uf); }
                snprintf(up, sizeof up, "%s/unknown_%03d_full.ppm", unk_dir, unk_n - 1); uint8_t *fr_rgb = malloc(NB1_W * NB1_H * 3); nb1_render_composite(st, st, &roms, fr_rgb); uf = fopen(up, "wb"); if (uf) { fprintf(uf, "P6\n%d %d\n255\n", NB1_W, NB1_H); fwrite(fr_rgb, 1, NB1_W * NB1_H * 3, uf); fclose(uf); } free(fr_rgb); }
            if (flow_log && steps % 3 == 0) { FILE *ff = fopen(flow_log, "a"); if (ff) { fprintf(ff, "%u\t%d\t%d\t%s\t%s\t%s\t%d\n", fr, nb1_flow_phase(flow), nb1_flow_type(flow), cur_name ? cur_name : "", nb1_flow_locked_table(flow) ? nb1_flow_locked_table(flow) : "", nb1_flow_type(flow) >= 0 ? nb1_flow_text(flow) : "", gi.stage); fclose(ff); } } }
        card_wait = 0;
        if (flow && nb1_flow_phase(flow) == NB1_FLOW_CARD_SELECT) {      /* the choosing screen: the four cards fly in and zoom; they can be shot only once they stand still */
            static Nb1Entry pe[256]; int n = nb1_sprite_entries(st, pe), big = 0, o = 0; unsigned sig = 2166136261u; char tmp[600]; static char allsp[6000]; int oa = 0; allsp[0] = 0; tmp[0] = 0;
            for (int i = 0; i < n; i++) if (pe[i].ntiles) { int x0 = pe[i].tiles[0].x, y0 = pe[i].tiles[0].y, x1 = x0, y1 = y0; for (int k = 0; k < pe[i].ntiles; k++) { const Nb1Tile *tl = &pe[i].tiles[k]; if (tl->x < x0) x0 = tl->x; if (tl->y < y0) y0 = tl->y; if (tl->x + tl->w > x1) x1 = tl->x + tl->w; if (tl->y + tl->h > y1) y1 = tl->y + tl->h; }
                { int t0 = pe[i].tiles[0].t, code = (((t0 & 0x7ff) | (st->bank[(t0 >> 11) & 7] << 11)) + pe[i].offset) & 0x7fff; oa += snprintf(allsp + oa, sizeof allsp - oa - 1, "%s[%d,%d,%d,%d,%d,%d,%d]", oa ? "," : "", code, x0, y0, x1, y1, pe[i].ntiles, pe[i].palette); }
                if (pe[i].ntiles == 59 || pe[i].ntiles == 63) sig = (sig ^ (unsigned)(pe[i].ntiles * 31 + x0 * 7 + y0 * 131)) * 16777619u;      /* the flags of played cards drop in: wait for them too */
                if (x1 - x0 >= 80 && x1 - x0 < 250 && y1 - y0 >= 60 && big < 8) { big++; sig = (sig ^ (unsigned)(x0 * 7 + y0 * 131 + x1 * 17 + y1 * 257)) * 16777619u; o += snprintf(tmp + o, sizeof tmp - o - 1, "%s[%d,%d,%d,%d]", o ? "," : "", x0, y0, x1, y1); } }
            if (big >= 3 && sig == card_sig) card_still++; else { card_still = 0; card_sig = sig; }
            int settled = card_still >= 20;
            if (settled) { snprintf(pick_sp, sizeof pick_sp, "%s", tmp); snprintf(pick_all, sizeof pick_all, "%s", allsp); }      /* the flags of the played cards drop in after the cards settle: keep the newest picture of the screen */
            if (settled && !card_set) { card_set = 1; pick_id = pick_seq++; pick_quad = -1;          /* the cards stand still: this is what was on offer */
                if (capdir && have_roms) { uint8_t *rgb = malloc(NB1_W * NB1_H * 3); nb1_render_composite(st, st, &roms, rgb); char pp[1400]; snprintf(pp, sizeof pp, "%s/offer_%03d.ppm", capdir, pick_id); FILE *pf = fopen(pp, "wb"); if (pf) { fprintf(pf, "P6\n%d %d\n255\n", NB1_W, NB1_H); fwrite(rgb, 1, NB1_W * NB1_H * 3, pf); fclose(pf); } free(rgb); } }
            if (!settled) card_wait = 1;
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
            if (cur_ident[0]) { FILE *lf = fopen(stage_log, "a"); if (lf) { fprintf(lf, "{\"text\":\"%s\",\"diff\":%d,\"stage\":%d,\"card\":%d,\"pid\":%d,\"quad\":%d,\"block\":%d,\"aim\":[%.0f,%.0f],\"pick_sprites\":[%s],\"offer\":[%s],\"from_frame\":%u,\"to_frame\":%u,\"scenes\":\"%s\",\"score\":%d,\"lives\":%d,\"lives_start\":%d,\"failed\":%d,\"complete\":%d,\"keys\":[", cur_ident, cur_diff, cur_stage, cur_card, cur_pid, cur_quad, cur_stage > 0 ? (cur_stage - 1) / 4 : -1, cur_ax, cur_ay, cur_sp, cur_all, stage_start, fr, scenes_seen, gi.score, gi.lives, lives_start, res_failed, res_complete);
                int first = 1, c, k; double n, h, p; for (int i = 0; nb1_bot_key(bot, i, &c, &k, &n, &h, &p); i++) { double n0 = 0, h0 = 0, p0 = 0; for (int j = 0; j < nsnap; j++) if (snap[j].code == c && snap[j].col == k) { n0 = snap[j].n; h0 = snap[j].hits; p0 = snap[j].pen; }
                    if (n > n0) { fprintf(lf, "%s[%d,%d,%.0f,%.0f,%.0f]", first ? "" : ",", c, k, n - n0, h - h0, p - p0); first = 0; } }
                fprintf(lf, "]}\n"); fclose(lf); if (once && strncmp(cur_ident, "slot", 4) && fr - stage_start > 300) seconds = -1; } }
            free(snap); nsnap = 0; { int c, k, i; double n, h, p; for (i = 0; nb1_bot_key(bot, i, &c, &k, &n, &h, &p); i++); snap = malloc((size_t)(i ? i : 1) * sizeof *snap); for (int j = 0; j < i; j++) { nb1_bot_key(bot, j, &c, &k, &n, &h, &p); snap[j] = (K){ c, k, n, h, p }; } nsnap = i; }
            res_failed = res_complete = 0; lives_start = gi.lives; cur_quad = pick_quad; cur_pid = pick_id; snprintf(cur_sp, sizeof cur_sp, "%s", pick_sp); snprintf(cur_all, sizeof cur_all, "%s", pick_all); cur_ax = pick_ax; cur_ay = pick_ay; pick_id = -1; pick_quad = -1; pick_sp[0] = 0; card_set = 0; cur_stage = gi.stage; cur_diff = gi.diff; cur_card = gi.card; snprintf(cur_ident, sizeof cur_ident, "%s", ident); stage_start = fr; scenes_seen[0] = 0; stage_scenes = 0; cap_start = fr; cap_n = 0;
        }
        if (capdir && have_roms && cap_n < 10 && fr >= cap_start + (unsigned)cap_n * 24) {                       /* pictures of the first seconds of the stage: the rules screen is among them */
            uint8_t *rgb = malloc(NB1_W * NB1_H * 3); nb1_render_composite(st, st, &roms, rgb); char pp[1200]; snprintf(pp, sizeof pp, "%s/d%d_n%d_c%d_%02d.ppm", capdir, cur_diff, cur_stage, cur_card, cap_n);
            FILE *pf = fopen(pp, "wb"); if (pf) { fprintf(pf, "P6\n%d %d\n255\n", NB1_W, NB1_H); fwrite(rgb, 1, NB1_W * NB1_H * 3, pf); fclose(pf); } free(rgb); cap_n++; }
        nb1_bot_step(bot, st, &gi, fr, &sx, &sy, &trig, &coin, &start); if (card_wait) trig = 0; Nb1State *st0 = st; steps++;
        if (movie_dir && have_roms && steps % (unsigned long)(movie_every / 2 > 0 ? movie_every / 2 : 1) == 0) {                 /* every Nth published state: the picture with the gun's aim marked (red cross; yellow while the trigger is held) */
            static int mn; uint8_t *rgb = malloc(NB1_W * NB1_H * 3); nb1_render_composite(st0, st0, &roms, rgb); int ax = (int)(sx + 0.5), ay = (int)(sy + 0.5);
            for (int d = -6; d <= 6; d++) { int xs[2][2] = { { ax + d, ay }, { ax, ay + d } }; for (int q = 0; q < 2; q++) { int x = xs[q][0], y = xs[q][1]; if (x >= 0 && x < NB1_W && y >= 0 && y < NB1_H) { uint8_t *px = rgb + (y * NB1_W + x) * 3; px[0] = 255; px[1] = trig ? 255 : 0; px[2] = 0; } } }
            char mp[1300]; snprintf(mp, sizeof mp, "%s/m_%05d.ppm", movie_dir, mn++); FILE *mf = fopen(mp, "wb"); if (mf) { fprintf(mf, "P6\n%d %d\n255\n", NB1_W, NB1_H); fwrite(rgb, 1, NB1_W * NB1_H * 3, mf); fclose(mf); } free(rgb); }
        FILE *f = fopen(aim_tmp, "w"); if (f) { fprintf(f, "%d %.2f %.2f %d %d %d 0 - %s\n", ++seq, sx, sy, trig, coin, start, save_req); fclose(f); remove(aim); rename(aim_tmp, aim); }
        free(st0);
        if (steps % 120 == 0) { fprintf(stderr, "%s\n", nb1_bot_status(bot)); if (stats) nb1_bot_save_stats(bot, stats); }
    }
    if (stats) nb1_bot_save_stats(bot, stats); fprintf(stderr, "%s\n", nb1_bot_status(bot)); return 0;
}
