/* pbbot: the auto-player on its own, no window. Plays the MAME session of tools/play/pbplay.lua: reads the sprite state it publishes (--ipc DIR), writes aim.txt.
 *   pbbot --ipc DIR [--rules bot_rules.json] [--tables scene_tables.json --roms DIR] [--stats-out FILE] [--seconds N]
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
    const char *ipc = NULL, *rules = NULL, *tables = NULL, *roms_dir = NULL, *stats = NULL; double seconds = 0;
    for (int i = 1; i < argc; i++) { const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--ipc") && v) { ipc = v; i++; } else if (!strcmp(a, "--rules") && v) { rules = v; i++; } else if (!strcmp(a, "--tables") && v) { tables = v; i++; }
        else if (!strcmp(a, "--roms") && v) { roms_dir = v; i++; } else if (!strcmp(a, "--stats-out") && v) { stats = v; i++; } else if (!strcmp(a, "--seconds") && v) { seconds = atof(v); i++; } }
    if (!ipc) { fprintf(stderr, "usage: pbbot --ipc DIR [--rules bot_rules.json] [--tables scene_tables.json --roms DIR] [--stats-out FILE] [--seconds N]\n"); return 2; }
    Nb1Bot *bot = nb1_bot_new(rules); Nb1Roms roms; int have_roms = roms_dir && nb1_roms_load(&roms, roms_dir) == 0;
    Nb1TableSet *ts = tables ? nb1_tableset_load(tables) : NULL; Nb1Selector *sel = NULL; if (ts) { if (have_roms) nb1_tableset_set_roms(ts, &roms); sel = nb1_selector_new(ts, 3); }
    char latest[1100], aim[1100], aim_tmp[1110], game[1100]; snprintf(latest, sizeof latest, "%s/latest.txt", ipc); snprintf(aim, sizeof aim, "%s/aim.txt", ipc); snprintf(aim_tmp, sizeof aim_tmp, "%s.tmp", aim); snprintf(game, sizeof game, "%s/game.txt", ipc);
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
            if (sscanf((char *)gb, "%u %d %d %d %d %d", &gf, &l, &s, &m, &sg, &d) == 6) { gi.valid = 1; gi.lives = l; gi.score = s; gi.mode = m; gi.stage = sg; gi.diff = d; } free(gb); } }
        if (sel) { const char *nm = NULL; if (nb1_selector_update(sel, st, st, &nm) && nm) nb1_bot_set_scene(bot, nm); }
        nb1_bot_step(bot, st, &gi, fr, &sx, &sy, &trig, &coin, &start); free(st); steps++;
        FILE *f = fopen(aim_tmp, "w"); if (f) { fprintf(f, "%d %.2f %.2f %d %d %d 0\n", ++seq, sx, sy, trig, coin, start); fclose(f); remove(aim); rename(aim_tmp, aim); }
        if (steps % 120 == 0) { fprintf(stderr, "%s\n", nb1_bot_status(bot)); if (stats) nb1_bot_save_stats(bot, stats); }
    }
    if (stats) nb1_bot_save_stats(bot, stats); fprintf(stderr, "%s\n", nb1_bot_status(bot)); return 0;
}
