/* nb1_tool: command-line harness for the C library (used by tests/run_oracle.py).
 *   nb1_tool dump ROMDIR TILE_STATE SPR_STATE TABLE|- OUTDIR
 * Writes everything the oracle comparison needs into OUTDIR. */
#include "nb1.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#endif

static FILE *fopenf(const char *dir, const char *name, const char *mode) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, mode);
    if (!f) { perror(p); exit(1); } return f;
}
static void put(const char *dir, const char *name, const void *buf, size_t n) { FILE *f = fopenf(dir, name, "wb"); fwrite(buf, 1, n, f); fclose(f); }

static const double CAMS[][2] = { { 20, 5 }, { 45, 10 }, { 70, 10 }, { 85, 0 }, { -80, 30 } };

static int dump(int argc, char **argv) {
    if (argc < 7) { fprintf(stderr, "usage: nb1_tool dump ROMDIR TILE SPR TABLE|- OUTDIR\n"); return 2; }
    const char *out = argv[6]; mkdir(out, 0755);
    Nb1Roms roms; if (nb1_roms_load(&roms, argv[2])) return 1;
    Nb1State *st = nb1_state_load(argv[3]), *sst = nb1_state_load(argv[4]); if (!st || !sst) { fprintf(stderr, "bad state\n"); return 1; }
    Nb1Table *tab = strcmp(argv[5], "-") ? nb1_table_load(argv[5]) : nb1_table_default();
    if (!tab) { fprintf(stderr, "bad table\n"); return 1; }

    uint8_t *img = malloc(NB1_W * NB1_H * 3);
    nb1_render_composite(st, sst, &roms, img); put(out, "composite.rgb", img, NB1_W * NB1_H * 3);

    Nb1Entry *ents = malloc(256 * sizeof *ents); int ne = nb1_sprite_entries(sst, ents);
    FILE *f = fopenf(out, "entries.tsv", "w");
    for (int i = 0; i < ne; i++) {
        const Nb1Entry *e = &ents[i];
        fprintf(f, "E %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n", i, e->which, e->pri, e->color, e->offset, e->flipx, e->flipy,
                e->clip[0], e->clip[1], e->clip[2], e->clip[3], e->ncols, e->nrows, e->hsize, e->vsize, e->x, e->y, e->ntiles);
        for (int k = 0; k < e->ntiles; k++) fprintf(f, "T %d %d %d %d %d\n", e->tiles[k].t, e->tiles[k].x, e->tiles[k].y, e->tiles[k].w, e->tiles[k].h);
    }
    fclose(f);

    Nb1Scene sc; nb1_build_scene(st, sst, &roms, tab, &sc);
    f = fopenf(out, "items.tsv", "w");
    for (int i = 0; i < sc.n; i++) {
        const Nb1Item *it = &sc.items[i];
        fprintf(f, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.17g\t%.17g\t%.17g\t%d\t%d\t%d\t%d\t%.17g\t%d\t%d\n", i, it->kind, it->id, it->pri, it->order,
                it->x0, it->y0, it->w, it->h, it->has_key ? it->key : -1, it->pal, it->ncols, it->nrows, it->z, it->tilt_x, it->tilt_y, it->hud, it->rule, it->shadow, it->erased,
                it->zoom, it->has_plane, it->decal);
        char nm[64]; snprintf(nm, sizeof nm, "tex_%03d.rgba", i); put(out, nm, it->tex, (size_t)it->w * it->h * 4);
        snprintf(nm, sizeof nm, "pen_%03d.i32", i); put(out, nm, it->pen, (size_t)it->w * it->h * 4);
    }
    fclose(f);

    Nb1Cam cam; nb1_arcade_camera(&cam, 1);
    uint8_t *view = malloc((size_t)cam.w * cam.h * 3); nb1_render_cam(&sc, &cam, view); put(out, "view_eye.rgb", view, (size_t)cam.w * cam.h * 3); free(view);

    const double target[3] = { 0, 0, -8.0 };
    FILE *af = fopenf(out, "aim.tsv", "w");
    for (unsigned c = 0; c < sizeof CAMS / sizeof *CAMS; c++) {
        Nb1Cam oc; nb1_orbit_camera(target, 14.0, CAMS[c][0], CAMS[c][1], &oc); oc.f = 420.0; oc.w = 640; oc.h = 480;
        uint8_t *v = malloc(640 * 480 * 3); nb1_render_cam(&sc, &oc, v);
        char nm[64]; snprintf(nm, sizeof nm, "cam_%u.rgb", c); put(out, nm, v, 640 * 480 * 3); free(v);
        for (int vy = 8; vy < 480; vy += 24) for (int ux = 8; ux < 640; ux += 24) {
            double a = (ux + .5 - 320) / oc.f, b = (240 - (vy + .5)) / oc.f, d[3];
            for (int k = 0; k < 3; k++) d[k] = a * oc.right[k] + b * oc.up[k] + oc.fwd[k];
            Nb1Aim r; int ok = nb1_aim(&sc, oc.E, d, 0, &r);
            fprintf(af, "%u\t%d\t%d\t%d\t%d\t%.9g\t%.9g\n", c, ux, vy, ok, ok ? r.item : -2, ok ? r.sx : 0.0, ok ? r.sy : 0.0);
        }
    }
    /* off-axis rays from near the arcade eye through arcade pixels */
    const double O[3] = { 0.2, -0.1, 0.5 };
    for (int y = 3; y < NB1_H; y += 11) for (int x = 3; x < NB1_W; x += 13) {
        double P[3] = { (x + .5 - NB1_W / 2.0) / NB1_FOCAL, (NB1_H / 2.0 - (y + .5)) / NB1_FOCAL, -1.0 }, d[3] = { P[0] - O[0], P[1] - O[1], P[2] - O[2] };
        Nb1Aim r; int ok = nb1_aim(&sc, O, d, 0, &r);
        fprintf(af, "9\t%d\t%d\t%d\t%d\t%.9g\t%.9g\n", x, y, ok, ok ? r.item : -2, ok ? r.sx : 0.0, ok ? r.sy : 0.0);
    }
    fclose(af);

    /* aim with snapping: same ray sets as above */
    int32_t *ids = malloc(NB1_W * NB1_H * 4); nb1_visible_map(&sc, ids); put(out, "ids.i32", ids, NB1_W * NB1_H * 4);
    FILE *sf = fopenf(out, "aimsnap.tsv", "w");
    for (unsigned c = 0; c < sizeof CAMS / sizeof *CAMS; c++) {
        Nb1Cam oc; nb1_orbit_camera(target, 14.0, CAMS[c][0], CAMS[c][1], &oc); oc.f = 420.0;
        for (int vy = 8; vy < 480; vy += 24) for (int ux = 8; ux < 640; ux += 24) {
            double a = (ux + .5 - 320) / oc.f, b = (240 - (vy + .5)) / oc.f, d[3];
            for (int k = 0; k < 3; k++) d[k] = a * oc.right[k] + b * oc.up[k] + oc.fwd[k];
            Nb1AimSnap r; int ok = nb1_aim_snap(&sc, ids, oc.E, d, 24.0, &r);
            fprintf(sf, "%u\t%d\t%d\t%d\t%d\t%.9g\t%.9g\t%d\t%d\t%d\n", c, ux, vy, ok, ok ? r.a.item : -2, ok ? r.a.sx : 0.0, ok ? r.a.sy : 0.0, ok ? r.snapped : 0, ok ? r.hidden : 0, ok ? r.cover : 0);
        }
    }
    fclose(sf);
    /* the same rays with the HUD ignored for aiming */
    sc.aim_skip_hud = 1; sf = fopenf(out, "aimhud.tsv", "w");
    for (unsigned c = 0; c < sizeof CAMS / sizeof *CAMS; c++) {
        Nb1Cam oc; nb1_orbit_camera(target, 14.0, CAMS[c][0], CAMS[c][1], &oc); oc.f = 420.0;
        for (int vy = 8; vy < 480; vy += 24) for (int ux = 8; ux < 640; ux += 24) {
            double a = (ux + .5 - 320) / oc.f, b = (240 - (vy + .5)) / oc.f, d[3];
            for (int k = 0; k < 3; k++) d[k] = a * oc.right[k] + b * oc.up[k] + oc.fwd[k];
            Nb1AimSnap r; int ok = nb1_aim_snap(&sc, ids, oc.E, d, 24.0, &r);
            fprintf(sf, "%u\t%d\t%d\t%d\t%d\t%.9g\t%.9g\t%d\t%d\t%d\n", c, ux, vy, ok, ok ? r.a.item : -2, ok ? r.a.sx : 0.0, ok ? r.a.sy : 0.0, ok ? r.snapped : 0, ok ? r.hidden : 0, ok ? r.cover : 0);
        }
    }
    fclose(sf); sc.aim_skip_hud = 0; free(ids);

    f = fopenf(out, "gun.tsv", "w");
    for (int y = 0; y < NB1_H; y++) for (int x = 0; x < NB1_W; x++) {
        Nb1GunRegs g = nb1_gun_regs(x, y); int X = -1, Y = -1; int ok = nb1_regs_to_pixel(g, &X, &Y);
        fprintf(f, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", x, y, g.x1, g.y1, ok, X, Y, g.x2);
    }
    fclose(f);
    nb1_scene_free(&sc); nb1_table_free(tab); free(img); free(ents); free(st); free(sst); nb1_roms_free(&roms);
    return 0;
}

/* nb1_tool select SCENE_TABLES.json STATE... : print the table name chosen for each state file ("-" = none) */
static int select_cmd(int argc, char **argv) {
    Nb1TableSet *ts = nb1_tableset_load(argv[2]); if (!ts) { fprintf(stderr, "cannot load %s\n", argv[2]); return 1; }
    for (int i = 3; i < argc; i++) {
        Nb1State *st = nb1_state_load(argv[i]); if (!st) { printf("%s\t?\n", argv[i]); continue; }
        const char *name; nb1_tableset_select(ts, st, st, &name); printf("%s\t%s\n", argv[i], name ? name : "-"); free(st);
    }
    nb1_tableset_free(ts); return 0;
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "select")) return select_cmd(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "dump")) return dump(argc, argv);
    fprintf(stderr, "usage: nb1_tool dump ROMDIR TILE_STATE SPR_STATE TABLE|- OUTDIR\n"); return 2;
}
