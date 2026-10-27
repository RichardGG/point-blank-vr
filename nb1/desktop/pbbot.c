/* pbbot: the auto-player on its own, no window. Plays the MAME session of tools/play/pbplay.lua: reads the sprite state it publishes (--ipc DIR), writes aim.txt.
 *   pbbot --ipc DIR [--rules bot_rules.json] [--tables scene_tables.json --roms DIR] [--stats-out FILE] [--seconds N]
 *         [--mode training|beginner|expert|veryhard] [--stage-log FILE.jsonl --capture DIR]   (per stage: what it learned, and pictures of the screens that open the stage)
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
    unsigned seed = 1; const char *stage_log = NULL, *capdir = NULL; int want_mode = 0; const char *ipc = NULL, *rules = NULL, *tables = NULL, *roms_dir = NULL, *stats = NULL; double seconds = 0;
    for (int i = 1; i < argc; i++) { const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--ipc") && v) { ipc = v; i++; } else if (!strcmp(a, "--rules") && v) { rules = v; i++; } else if (!strcmp(a, "--tables") && v) { tables = v; i++; }
        else if (!strcmp(a, "--roms") && v) { roms_dir = v; i++; } else if (!strcmp(a, "--stats-out") && v) { stats = v; i++; } else if (!strcmp(a, "--seconds") && v) { seconds = atof(v); i++; }
        else if (!strcmp(a, "--seed") && v) { seed = (unsigned)atoi(v); i++; } else if (!strcmp(a, "--stage-log") && v) { stage_log = v; i++; } else if (!strcmp(a, "--capture") && v) { capdir = v; i++; }
        else if (!strcmp(a, "--mode") && v) { want_mode = !strcmp(v, "training") ? 1 : !strcmp(v, "beginner") ? 2 : !strcmp(v, "expert") ? 3 : !strcmp(v, "veryhard") ? 4 : 0; i++; } }
    if (!ipc) { fprintf(stderr, "usage: pbbot --ipc DIR [--rules bot_rules.json] [--tables scene_tables.json --roms DIR] [--stats-out FILE] [--seconds N]\n"); return 2; }
    Nb1Bot *bot = nb1_bot_new(rules); nb1_bot_set_mode(bot, want_mode); nb1_bot_set_seed(bot, seed); Nb1Roms roms; int have_roms = roms_dir && nb1_roms_load(&roms, roms_dir) == 0;
    Nb1TableSet *ts = tables ? nb1_tableset_load(tables) : NULL; Nb1Selector *sel = NULL; if (ts) { if (have_roms) nb1_tableset_set_roms(ts, &roms); sel = nb1_selector_new(ts, 3); }
    char latest[1100], aim[1100], aim_tmp[1110], game[1100]; snprintf(latest, sizeof latest, "%s/latest.txt", ipc); snprintf(aim, sizeof aim, "%s/aim.txt", ipc); snprintf(aim_tmp, sizeof aim_tmp, "%s.tmp", aim); snprintf(game, sizeof game, "%s/game.txt", ipc);
    typedef struct { int code, col; double n, hits, pen; } K; K *snap = NULL; int nsnap = 0; int cur_stage = -99, cur_diff = -99, cur_card = -99; unsigned stage_start = 0, cap_start = 0; int cap_n = 0, stage_scenes = 0; char scenes_seen[400] = "";
    unsigned last = 0; int have = 0, seq = 0; time_t t0 = time(NULL); unsigned long steps = 0; double sx = 144, sy = 112; int trig = 0, coin = 0, start = 0;
    while (seconds <= 0 || difftime(time(NULL), t0) < seconds) {
        size_t n; unsigned char *b = slurp(latest, &n); char name[256]; unsigned fr = 0;
        if (!b) { SLEEP_MS(5); continue; }
        b = realloc(b, n + 1); b[n] = 0; int ok = sscanf((char *)b, "%255s %u", name, &fr) == 2; free(b);
        if (!ok || (have && fr == last)) { SLEEP_MS(3); continue; }
        char path[1100]; snprintf(path, sizeof path, "%s/%s", ipc, name); b = slurp(path, &n);
        if (!b || n != NB1_STATE_FILE_BYTES + 12 || memcmp(b, "PBS1", 4) || memcmp(b + 4, b + n - 4, 4)) { free(b); SLEEP_MS(2); continue; }
        Nb1State *st = nb1_state_from_bytes(b + 8, n - 12); free(b); last = fr; have = 1; if (!st) continue;
        Nb1GameInfo gi; memset(&gi, 0, sizeof gi); { unsigned gf; int l, s, m, sg, d; unsigned char *gb = slurp(game, &n); if (gb) { gb = realloc(gb, n + 1); gb[n] = 0;
            int cd = 0; if (sscanf((char *)gb, "%u %d %d %d %d %d %d", &gf, &l, &s, &m, &sg, &d, &cd) >= 6) { gi.valid = 1; gi.lives = l; gi.score = s; gi.mode = m; gi.stage = sg; gi.diff = d; gi.card = cd; } free(gb); } }
        if (sel) { const char *nm = NULL; if (nb1_selector_update(sel, st, st, &nm) && nm) { nb1_bot_set_scene(bot, nm); if (!strstr(scenes_seen, nm) && strlen(scenes_seen) + strlen(nm) + 3 < sizeof scenes_seen) { strcat(scenes_seen, stage_scenes++ ? "; " : ""); strcat(scenes_seen, nm); } } }
        if (gi.valid && stage_log && (gi.stage != cur_stage || gi.diff != cur_diff || gi.card != cur_card)) {        /* a new stage, difficulty or card pick: close the last record (what the bot learned meanwhile) and start a capture */
            if (cur_stage != -99) { FILE *lf = fopen(stage_log, "a"); if (lf) { fprintf(lf, "{\"diff\":%d,\"stage\":%d,\"card\":%d,\"from_frame\":%u,\"to_frame\":%u,\"scenes\":\"%s\",\"score\":%d,\"lives\":%d,\"keys\":[", cur_diff, cur_stage, cur_card, stage_start, fr, scenes_seen, gi.score, gi.lives);
                int first = 1, c, k; double n, h, p; for (int i = 0; nb1_bot_key(bot, i, &c, &k, &n, &h, &p); i++) { double n0 = 0, h0 = 0, p0 = 0; for (int j = 0; j < nsnap; j++) if (snap[j].code == c && snap[j].col == k) { n0 = snap[j].n; h0 = snap[j].hits; p0 = snap[j].pen; }
                    if (n > n0) { fprintf(lf, "%s[%d,%d,%.0f,%.0f,%.0f]", first ? "" : ",", c, k, n - n0, h - h0, p - p0); first = 0; } }
                fprintf(lf, "]}\n"); fclose(lf); } }
            free(snap); nsnap = 0; { int c, k, i; double n, h, p; for (i = 0; nb1_bot_key(bot, i, &c, &k, &n, &h, &p); i++); snap = malloc((size_t)(i ? i : 1) * sizeof *snap); for (int j = 0; j < i; j++) { nb1_bot_key(bot, j, &c, &k, &n, &h, &p); snap[j] = (K){ c, k, n, h, p }; } nsnap = i; }
            cur_stage = gi.stage; cur_diff = gi.diff; cur_card = gi.card; stage_start = fr; scenes_seen[0] = 0; stage_scenes = 0; cap_start = fr; cap_n = 0;
        }
        if (capdir && have_roms && cap_n < 18 && fr >= cap_start + (unsigned)cap_n * 24) {                       /* pictures of the first seconds of the stage: the rules screen is among them */
            uint8_t *rgb = malloc(NB1_W * NB1_H * 3); nb1_render_composite(st, st, &roms, rgb); char pp[1200]; snprintf(pp, sizeof pp, "%s/d%d_n%d_c%d_%02d.ppm", capdir, cur_diff, cur_stage, cur_card, cap_n);
            FILE *pf = fopen(pp, "wb"); if (pf) { fprintf(pf, "P6\n%d %d\n255\n", NB1_W, NB1_H); fwrite(rgb, 1, NB1_W * NB1_H * 3, pf); fclose(pf); } free(rgb); cap_n++; }
        nb1_bot_step(bot, st, &gi, fr, &sx, &sy, &trig, &coin, &start); free(st); steps++;
        FILE *f = fopen(aim_tmp, "w"); if (f) { fprintf(f, "%d %.2f %.2f %d %d %d 0\n", ++seq, sx, sy, trig, coin, start); fclose(f); remove(aim); rename(aim_tmp, aim); }
        if (steps % 120 == 0) { fprintf(stderr, "%s\n", nb1_bot_status(bot)); if (stats) nb1_bot_save_stats(bot, stats); }
    }
    if (stats) nb1_bot_save_stats(bot, stats); fprintf(stderr, "%s\n", nb1_bot_status(bot)); return 0;
}
