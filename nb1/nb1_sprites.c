#pragma GCC diagnostic ignored "-Wmisleading-indentation"
/* The sprite library: every sprite of the game's sprite ROM that can be drawn at any time, whether or not the current scene has it loaded.
 * The ROM holds pixels only (16x16 tiles, 8 bpp); the library (research/tools/nb1/make_sprite_library.py) adds what the game keeps in RAM: which tiles form a sprite
 * and the palette. See nb1.h. */
#include "nb1.h"
#include "nb1_json.h"
#include "nb1_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int ncols, nrows, npal; int *codes; int *pals; int nscenes; int *scenes; } LibFrame;
struct Nb1SprLib { int nframes; LibFrame *f; int npal; uint8_t *pal; uint16_t *tile_pal; int ntiles; };

static uint8_t *slurp_file(const char *dir, const char *name, size_t *n) {
    char p[1024]; snprintf(p, sizeof p, "%s%s%s", dir, (strlen(dir) && dir[strlen(dir) - 1] != '/' && dir[strlen(dir) - 1] != '\\') ? "/" : "", name);
    FILE *f = fopen(p, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(len ? (size_t)len : 1); if (fread(b, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(b); return NULL; } fclose(f); *n = (size_t)len; return b;
}
static int *json_ints(const Json *a, int *n) { *n = 0; if (!a || a->t != J_ARR) return NULL; int *v = malloc((size_t)(a->n ? a->n : 1) * sizeof *v); for (int i = 0; i < a->n; i++) v[i] = (int)a->kid[i]->num; *n = a->n; return v; }

Nb1SprLib *nb1_sprlib_load(const char *dir) {
    size_t n; uint8_t *fj = slurp_file(dir, "frames.json", &n); if (!fj) return NULL;
    char *txt = malloc(n + 1); memcpy(txt, fj, n); txt[n] = 0; free(fj); Json *j = json_parse(txt); free(txt); if (!j) return NULL;
    const Json *fr = json_get(j, "frames"); Nb1SprLib *L = calloc(1, sizeof *L); if (!fr || fr->t != J_ARR) { json_free(j); free(L); return NULL; }
    L->ntiles = (int)json_get(j, "tiles")->num; L->nframes = fr->n; L->f = calloc((size_t)(fr->n ? fr->n : 1), sizeof *L->f);
    for (int i = 0; i < fr->n; i++) {
        const Json *o = fr->kid[i]; LibFrame *f = &L->f[i]; int nc;
        f->ncols = (int)json_get(o, "ncols")->num; f->nrows = (int)json_get(o, "nrows")->num;
        f->codes = json_ints(json_get(o, "codes"), &nc); f->pals = json_ints(json_get(o, "pals"), &f->npal); f->scenes = json_ints(json_get(o, "scenes"), &f->nscenes);
    }
    json_free(j);
    L->pal = slurp_file(dir, "palettes.bin", &n); L->npal = L->pal ? (int)(n / 768) : 0;
    uint8_t *tp = slurp_file(dir, "tile_pal.bin", &n); L->tile_pal = calloc((size_t)L->ntiles, sizeof *L->tile_pal);
    for (int i = 0; i < L->ntiles; i++) L->tile_pal[i] = (tp && (size_t)(2 * i + 1) < n) ? (uint16_t)(tp[2 * i] | tp[2 * i + 1] << 8) : 65535; free(tp);
    return L;
}
void nb1_sprlib_free(Nb1SprLib *L) {
    if (!L) return; for (int i = 0; i < L->nframes; i++) { free(L->f[i].codes); free(L->f[i].pals); free(L->f[i].scenes); } free(L->f); free(L->pal); free(L->tile_pal); free(L);
}
int nb1_sprlib_count(const Nb1SprLib *L) { return L ? L->nframes : 0; }
int nb1_sprlib_info(const Nb1SprLib *L, int id, int *ncols, int *nrows, int *npalettes, const int **scenes, int *nscenes) {
    if (!L || id < 0 || id >= L->nframes) return 0; const LibFrame *f = &L->f[id]; *ncols = f->ncols; *nrows = f->nrows; *npalettes = f->npal; *scenes = f->scenes; *nscenes = f->nscenes; return 1;
}
/* a grid of tile codes (-1 = empty) with one palette (256 RGB entries); pen 255 is transparent */
static int grid_tex(const Nb1Roms *roms, const int *codes, int nc, int nr, const uint8_t *pal, Nb1Tex *out) {
    int w = nc * 16, h = nr * 16; if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    uint8_t *rgba = calloc((size_t)w * h, 4); int32_t *pen = malloc((size_t)w * h * 4); for (size_t k = 0; k < (size_t)w * h; k++) pen[k] = -1;
    for (int k = 0; k < nc * nr; k++) {
        int c = codes[k]; if (c < 0 || (size_t)c >= roms->nspr) continue; const uint8_t *t = roms->spr + (size_t)c * 256; int x0 = (k % nc) * 16, y0 = (k / nc) * 16;
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { int v = t[y * 16 + x]; if (v == 255) continue; size_t o = (size_t)(y0 + y) * w + x0 + x;
            pen[o] = v; rgba[o * 4] = pal[v * 3]; rgba[o * 4 + 1] = pal[v * 3 + 1]; rgba[o * 4 + 2] = pal[v * 3 + 2]; rgba[o * 4 + 3] = 255; }
    }
    out->x0 = 0; out->y0 = 0; out->w = w; out->h = h; out->rgba = rgba; out->pen = pen; return 1;
}
int nb1_sprlib_frame_tex(const Nb1SprLib *L, const Nb1Roms *roms, int id, int variant, Nb1Tex *out) {
    if (!L || id < 0 || id >= L->nframes) return 0; const LibFrame *f = &L->f[id]; if (!f->npal || variant < 0 || variant >= f->npal) variant = 0;
    int p = f->pals[variant]; if (p < 0 || p >= L->npal) return 0; return grid_tex(roms, f->codes, f->ncols, f->nrows, L->pal + (size_t)p * 768, out);
}
/* any run of tiles of the sprite ROM (first, first+1, ... in ncols x nrows), drawn with the palette of the nearest tile the scenes showed (grey if none) */
int nb1_sprlib_tiles_tex(const Nb1SprLib *L, const Nb1Roms *roms, int first, int ncols, int nrows, Nb1Tex *out) {
    if (ncols <= 0 || nrows <= 0 || ncols * nrows > 4096) return 0; int *codes = malloc((size_t)ncols * nrows * sizeof *codes); for (int k = 0; k < ncols * nrows; k++) codes[k] = first + k;
    uint8_t grey[768]; for (int i = 0; i < 256; i++) grey[i * 3] = grey[i * 3 + 1] = grey[i * 3 + 2] = (uint8_t)i;
    const uint8_t *pal = grey; if (L && first >= 0 && first < L->ntiles && L->tile_pal[first] != 65535 && L->tile_pal[first] < L->npal) pal = L->pal + (size_t)L->tile_pal[first] * 768;
    int ok = grid_tex(roms, codes, ncols, nrows, pal, out); free(codes); return ok;
}
