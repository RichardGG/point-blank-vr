/* Internal shared definitions (not part of the public API). */
#ifndef NB1_INTERNAL_H
#define NB1_INTERNAL_H
#include "nb1.h"
#include "nb1_json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CX (NB1_W / 2.0)
#define CY (NB1_H / 2.0)
#define FOCAL NB1_FOCAL

static inline double dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void cross(const double *a, const double *b, double *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static inline void unproject(double x, double y, double z, double *o) { o[0] = (x - CX) * z / FOCAL; o[1] = (CY - y) * z / FOCAL; o[2] = -z; }

/* ---- item matching (depth rules and edits share it) */
typedef struct {
    int kind;                                   /* -1 any */
    int nlayer, layer[16]; int npri, pri[16]; int npal, pal[16]; int nid, id[32];
    int has_code, code_lo, code_hi; int has_size, sw, sh;
    int part_mode; char part[32];               /* 0 any, 1 "part": null (no part), 2 "part": "name" */
    int has_cx, has_cy; double cx0, cx1, cy0, cy1;
} Match;
int nb1_match_item(const Match *m, const Nb1Item *it);

typedef struct {
    Match m; int hud, has_z, has_plane, has_fc, has_cc, has_zd; double z, pn[3], pd, fc, cc, zd, zlo, zhi, tilt_x, tilt_y;
} Rule;

/* ---- edits */
typedef struct { int type; int nclone; int off[8][2]; } Fill;          /* type 0 inpaint, 1 none, 2 clone (then inpaint what is left) */
typedef struct {
    char name[32]; int has_name;
    int region;                                  /* 1 rect, 2 poly, 3 mask */
    int rect[4]; int nrings; int *ring_n; int **ring_xy;   /* ring_xy[r] = x0,y0,x1,y1,... */
    char mask[256]; int mask_white;
    Fill fill;
} Part;
typedef struct {
    Match m;
    int has_texture; char texture[256];
    int has_punch; Match from; int grow; Fill pfill;
    int nparts; Part *parts;
} Edit;

typedef struct { Match m; double k; } Scale;                    /* "scales": every matching non-HUD item's depth (and plane distance) is multiplied by k */

struct Nb1Table {
    Json *root;                                      /* the parsed table file (the "surround" section is read from it when a scene is built) */
    int shallow;                                     /* 1 = a copy made by nb1_table_with_scales: shares rules/edits with its parent, owns only `scales` */
    int nscales; Scale *scales;
    double far, z_front, z_back, hud_z, slot_gap; int nrules; Rule *rules;
    int ndecals; int decal_lo[16], decal_hi[16];     /* sprite tile-code ranges drawn as decals */
    int nedits; Edit *edits; char base[512];         /* base: directory of the table file (relative texture / mask paths) */
};

/* ---- helpers implemented elsewhere */
int  nb1_png_load(const char *path, int *w, int *h, uint8_t **rgba);               /* 8-bit PNG -> RGBA, 0 on success */
void nb1_fill_polygon(const int *xy, int n, int w, int h, uint8_t *mask);          /* ORs the polygon into mask[h][w] */
void nb1_inpaint(uint8_t *img, const uint8_t *hole, const uint8_t *alpha, int w, int h);   /* img RGB; hole/alpha per pixel */
int  nb1_apply_edits(Nb1Item *items, int n, int cap, const Nb1Table *tab, uint8_t rgb[0x2000][3]);   /* returns the new item count */
/* last path separator, '/' or (Windows) '\\' */
static inline const char *nb1_last_sep(const char *p) { const char *a = strrchr(p, '/'), *b = strrchr(p, '\\'); return a > b ? a : b; }
static inline int nb1_is_abs(const char *p) { return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':'); }
#endif
