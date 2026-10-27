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
Nb1State *nb1_state_load_z(const char *path);                 /* the same, zlib-compressed (data/scenes) */
Nb1State *nb1_state_from_bytes(const uint8_t *b, size_t len);   /* same layout, 229,464 bytes; NULL if too short */
#define NB1_STATE_FILE_BYTES 229464

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

/* ---- sprite library: every sprite of the game's ROM, drawable at any time (data: research/data/sprites, made by make_sprite_library.py).
 * A "frame" is a sprite seen in the captured scenes: columns x rows of 16x16 ROM tiles plus the palette(s) it was drawn with. Any other run of ROM tiles can be drawn too,
 * with a guessed shape and the palette of the nearest seen tile. Results are Nb1Tex (rgba[h][w][4], pen = ROM pixel value or -1; free rgba and pen). */
typedef struct Nb1SprLib Nb1SprLib;
Nb1SprLib *nb1_sprlib_load(const char *dir);            /* NULL if frames.json is missing */
void nb1_sprlib_free(Nb1SprLib *L);
int  nb1_sprlib_count(const Nb1SprLib *L);
int  nb1_sprlib_info(const Nb1SprLib *L, int id, int *ncols, int *nrows, int *npalettes, const int **scenes, int *nscenes);
int  nb1_sprlib_frame_tex(const Nb1SprLib *L, const Nb1Roms *roms, int id, int palette_variant, Nb1Tex *out);
int  nb1_sprlib_tiles_tex(const Nb1SprLib *L, const Nb1Roms *roms, int first_tile, int ncols, int nrows, Nb1Tex *out);

/* ---- auto-player. Feed it every new sprite state; it returns where to aim (arcade pixels) and whether to hold the trigger.
 * rules: research/data/bot_rules.json (NULL = built-in defaults). game: the game's own numbers if known (valid = 0 otherwise). */
typedef struct Nb1Bot Nb1Bot;
typedef struct { int valid, lives, score, mode, stage, diff; } Nb1GameInfo;
Nb1Bot *nb1_bot_new(const char *rules_path);
void nb1_bot_free(Nb1Bot *b);
void nb1_bot_set_scene(Nb1Bot *b, const char *table_name);       /* the depth table / scene family in use: selects the per-scene rules */
void nb1_bot_step(Nb1Bot *b, const Nb1State *sprite_state, const Nb1GameInfo *game, unsigned frame, double *sx, double *sy, int *trigger, int *coin, int *start);
const char *nb1_bot_status(const Nb1Bot *b);
int  nb1_bot_save_stats(const Nb1Bot *b, const char *path);    /* what it learned while playing: [[code, colour, shots, hits, penalties], ...] (the format of the rules file's "keys") */

/* ---- depth table (JSON; the same format as the Python tool, including "edits") */
typedef struct Nb1Table Nb1Table;
Nb1Table *nb1_table_default(void);
Nb1Table *nb1_table_load(const char *path);          /* NULL on error */
void nb1_table_free(Nb1Table *t);
double nb1_table_far(const Nb1Table *t);
/* Depth scaling: multiply the depth of every non-HUD item matching the spec by k, moving it along the arcade eye's rays (so the arcade
 * picture does not change, only the layer's real size / distance). The same thing as a table's "scales": [{"match": {...}, "scale": k}]. */
typedef struct { int kind /* NB1_TILEMAP / NB1_SPRITE / -1 any */, layer /* tilemap layer or -1 */, pri /* -1 any */, has_code, code_lo, code_hi; double k; } Nb1ScaleSpec;
/* a copy of `base` (sharing its rules and edits) with the specs applied after the table's own scales; free it with nb1_table_free */
Nb1Table *nb1_table_with_scales(const Nb1Table *base, const Nb1ScaleSpec *specs, int n);

/* ---- automatic table selection (scene_tables.json + scene_refs.json beside it; same rules as depthscene.select_table in the oracle):
 * an entry matches by exact sprite codes ("codes_all" / "codes_any": the first such entry wins) or by background-art similarity to captured
 * reference scenes ("art_scenes" + "min_similarity", Jaccard on (layer, tile code) pairs: the most similar entry wins). */
typedef struct Nb1TableSet Nb1TableSet;
Nb1TableSet *nb1_tableset_load(const char *scene_tables_json);          /* loads every table it names; NULL on error */
const Nb1Table *nb1_tableset_select(const Nb1TableSet *ts, const Nb1State *st, const Nb1State *sst, const char **name);   /* NULL = none matches */
void nb1_tableset_free(Nb1TableSet *ts);
/* Give the table set the ROMs so that art matches also compare the picture (a 16 x 11 thumbnail kept in scene_refs.json): equal tile codes alone can be leftovers. */
void nb1_tableset_set_roms(Nb1TableSet *ts, const Nb1Roms *roms);
/* Stable table choice for a video stream: nb1_tableset_select() can flip from frame to frame (a sprite that a rule looks for comes and goes), and
 * a table flipping makes the whole scene's layering jump. The selector keeps the current table while the background art stays the same scene
 * (similarity to the art when the table last matched >= 0.6), switches to another table after `agree` consecutive frames that choose it, and
 * drops to "none" only when the art has changed. */
typedef struct Nb1Selector Nb1Selector;
Nb1Selector *nb1_selector_new(const Nb1TableSet *ts, int agree);
const Nb1Table *nb1_selector_update(Nb1Selector *s, const Nb1State *st, const Nb1State *sst, const char **name);
void nb1_selector_free(Nb1Selector *s);
/* the captured scene (as numbered in scene_refs.json) whose background art is most like this state; -1 if there are no references */
int nb1_tableset_nearest_scene(const Nb1TableSet *ts, const Nb1State *st, double *similarity);

/* ---- scene */
enum { NB1_TILEMAP = 0, NB1_SPRITE = 1, NB1_MASK = 2, NB1_SURROUND = 3 };
/* a convex polygon of a surround surface, world coordinates with texture coordinates */
typedef struct { int n; double p[8][3], uv[8][2]; } Nb1Poly;
typedef struct {
    int kind, id, pri, order, x0, y0, w, h;
    uint8_t *tex; int32_t *pen;
    int has_key, key, pal, ncols, nrows;
    double z, tilt_x, tilt_y; int hud, rule, shadow, listidx, erased; double zoom;
    uint8_t *cust; /* RGB art for edited / custom texels (pen -4), or NULL */
    char part[32]; int has_part; /* name of the piece an edit split this item into */
    int has_plane; double pn[3], pd; int decal; /* drawn onto the item beneath (shot marks): never blocks aim or the visible map */
    /* derived by nb1_scene_prepare */
    double ctr[3], r[3], u[3], nrm[3], rr, uu;
    /* NB1_SURROUND items (see nb1_scene_add_surround): free polygons textured with `tex` (w x h RGBA), never part of the arcade picture, aim or visible map */
    Nb1Poly *polys; int npolys; int wrap /* horizontal: 0 clamp, 1 repeat, 2 mirrored repeat */, wrap_t /* the same, vertical */; int fade_on; double fade_c[3], fade_r0, fade_r1, fade_col[3];
} Nb1Item;
typedef struct { Nb1Item *items; int n; double far; uint8_t rgb[0x2000][3];
                 int aim_skip_hud; /* 1 = aiming ignores HUD items (the ray passes through them); 0 by default; set by the viewers */
                 int scroll_hint[4]; /* input to nb1_scene_add_surround: 1 = this tilemap layer is scrolling (the caller saw its scroll registers change) */
                 /* surround adjustments (input; adj_has: pitch, ground, ceiling) and the values actually used (output; NAN = none) */
                 int adj_has[3]; double adj_pitch, adj_ground, adj_ceil, pitch_deg, ground_y, ceil_y;
                 double clear[3]; int has_clear; /* background colour of the world around the play screen (set by nb1_scene_add_surround), 0..255 */ } Nb1Scene;

int  nb1_build_scene(const Nb1State *st, const Nb1State *sst, const Nb1Roms *roms, const Nb1Table *tab, Nb1Scene *out);
void nb1_scene_free(Nb1Scene *s);
/* The world around the play screen: extra NB1_SURROUND items built from this scene's own layers, following the table's "surround" section (or a
 * plain automatic one): the tilemap layers continued sideways from their real scroll position, a sky dome, a ground / ceiling, all clipped so
 * that nothing of it lies inside the arcade picture's view pyramid. Call after nb1_build_scene; the arcade-eye picture is unchanged. */
int nb1_scene_add_surround(Nb1Scene *s, const Nb1State *st, const Nb1Roms *roms, const Nb1Table *tab);
/* One tilemap layer's whole 64x64-tile map (512x512 RGBA, alpha 0 where empty) and where the screen window sits in it: map x = screen x + ox, map y = screen y + oy
 * (mirrored when flip). For layers 0..3; returns 0 for the HUD layers. */
int nb1_tilemap_map_tex(const Nb1State *st, const Nb1Roms *roms, int i, uint8_t rgb[0x2000][3], uint8_t *rgba, int *ox, int *oy, int *flip);

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
