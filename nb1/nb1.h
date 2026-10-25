/* NB-1 (Point Blank) video + depth-scene library. C port of the Python oracle in oracle/ (nb1render.py, depthscene.py).
 * Takes a RAM snapshot (see Nb1State), builds one RGBA quad per tilemap layer and sprite entry, assigns depths from
 * a rule table, and offers a z-buffered software view, a ray/quad aim solver and the gun-register mapping. */
#ifndef NB1_LIB_H
#define NB1_LIB_H
#include <stdint.h>
#include <stddef.h>

#define NB1_W 288
#define NB1_H 224

/* ---- ROMs (caller supplies a directory of MAME ptblank files) */
typedef struct { uint8_t *spr; size_t nspr; uint8_t *chr; size_t nchr; uint8_t *mask; size_t nmask; } Nb1Roms; /* tile counts */
int  nb1_roms_load(Nb1Roms *r, const char *dir);
void nb1_roms_free(Nb1Roms *r);

/* ---- raw RAM snapshot (tools/mame/snap.lua layout; all words big-endian in the file) */
typedef struct {
    uint16_t spriteram[0x10000]; uint16_t vram[0x8000]; uint16_t ctl[0x20]; uint16_t bank[8]; uint16_t pos[4];
    uint8_t pal[0x8000];
} Nb1State;
Nb1State *nb1_state_load(const char *path);

/* ---- C116 palette: 0x2000 RGB entries */
void nb1_palette_rgb(const Nb1State *st, uint8_t rgb[0x2000][3]);

/* ---- C355 sprite entries (list 1) */
typedef struct { int t, x, y, w, h; } Nb1Tile;
typedef struct {
    int which, pri, color, offset, flipx, flipy, clip[4], ncols, nrows, hsize, vsize, x, y, palette, link;
    int ntiles; Nb1Tile tiles[256];
} Nb1Entry;
int nb1_sprite_entries(const Nb1State *st, Nb1Entry *out /* [256] */);

/* Full composite exactly as the arcade draws it (tilemaps from st, sprites from sst), RGB 288x224. */
void nb1_render_composite(const Nb1State *st, const Nb1State *sst, const Nb1Roms *roms, uint8_t *rgb);

/* One sprite entry on its own: x0,y0,w,h, rgba[h][w][4], pen[h][w] (-1 empty, -2 shadow). Caller frees. NULL = draws nothing. */
typedef struct { int x0, y0, w, h; uint8_t *rgba; int32_t *pen; } Nb1Tex;
int nb1_entry_tex(const Nb1State *st, const Nb1Roms *roms, const Nb1Entry *sp, uint8_t rgb[0x2000][3], Nb1Tex *out);
/* One tilemap layer, full screen: rgba[224][288][4], pen[224][288] (-1 empty). */
void nb1_tilemap_tex(const Nb1State *st, const Nb1Roms *roms, int i, uint8_t rgb[0x2000][3], uint8_t *rgba, int32_t *pen);

/* ---- depth table (JSON; the same format as the Python tool. "edits" are not implemented yet and are counted) */
typedef struct Nb1Table Nb1Table;
Nb1Table *nb1_table_default(void);
Nb1Table *nb1_table_load(const char *path);          /* NULL on error */
void nb1_table_free(Nb1Table *t);
int  nb1_table_ignored_edits(const Nb1Table *t);
double nb1_table_far(const Nb1Table *t);

/* ---- scene */
enum { NB1_TILEMAP = 0, NB1_SPRITE = 1, NB1_MASK = 2 };
typedef struct {
    int kind, id, pri, order, x0, y0, w, h;
    uint8_t *tex; int32_t *pen;
    int has_key, key, pal, ncols, nrows;
    double z, tilt_x, tilt_y; int hud, rule, shadow, listidx, erased; double zoom;
    int has_plane; double pn[3], pd; int decal; /* drawn onto the item beneath (shot marks): never blocks aim or the visible map */
    /* derived by nb1_scene_prepare */
    double ctr[3], r[3], u[3], nrm[3], rr, uu;
} Nb1Item;
typedef struct { Nb1Item *items; int n; double far; uint8_t rgb[0x2000][3]; } Nb1Scene;

int  nb1_build_scene(const Nb1State *st, const Nb1State *sst, const Nb1Roms *roms, const Nb1Table *tab, Nb1Scene *out);
void nb1_scene_free(Nb1Scene *s);

/* Camera: eye + orthonormal forward/right/up, focal in pixels. */
typedef struct { double E[3], fwd[3], right[3], up[3], f; int w, h; } Nb1Cam;
void nb1_orbit_camera(const double target[3], double distance, double yaw_deg, double pitch_deg, Nb1Cam *c);
void nb1_arcade_camera(Nb1Cam *c, int scale);        /* the arcade eye: reproduces the arcade picture at scale 1 */
void nb1_render_cam(const Nb1Scene *s, const Nb1Cam *c, uint8_t *rgb /* w*h*3 */);

typedef struct { int item; /* index into items, -1 = fallback plane */ double hit[3], sx, sy; } Nb1Aim;
/* nb1_aim: fallback_z 0 = plane at 2.5 m, > 0 = plane at that distance, < 0 = no fallback (a miss returns 0) */
int nb1_aim(const Nb1Scene *s, const double O[3], const double D[3], double fallback_z /* 0 = 2.5 */, Nb1Aim *out);

/* Which item the arcade shows at each arcade pixel: ids[224*288] = index into items, -1 = nothing or blanked. */
void nb1_visible_map(const Nb1Scene *s, int32_t *ids);
/* aim, but the arcade pixel is moved (within radius px) to where the hit item is visible to the arcade eye, because the game only
 * sees 2D pixels. No hit => returns 0 and the caller keeps its last pixel (there is no fallback plane). */
typedef struct { Nb1Aim a; double raw_sx, raw_sy; int snapped, hidden, cover; } Nb1AimSnap;
int nb1_aim_snap(const Nb1Scene *s, const int32_t *ids, const double O[3], const double D[3], double radius, Nb1AimSnap *out);

/* ---- gun registers */
typedef struct { int x1, x2, y1, y2; } Nb1GunRegs;
Nb1GunRegs nb1_gun_regs(double sx, double sy);
int nb1_regs_to_pixel(Nb1GunRegs r, int *X, int *Y);  /* 0 = shot ignored */

extern const double NB1_FOCAL;
#endif
