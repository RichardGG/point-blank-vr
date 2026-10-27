# nb1: Point Blank video and depth-scene library (C)

C port of the Python research code (`oracle/nb1render.py`, `oracle/depthscene.py`, copied unchanged from the research repo).
No dependencies beyond libc/libm.

| File | Contents |
|---|---|
| `nb1.h` | public API |
| `nb1_video.c` | ROM and snapshot loading, C116 palette, C123 tilemaps, C355 sprite decode and stretch, full composite (equals MAME's picture) |
| `nb1_scene.c` | depth-table rules, sprite-buffer erasers, C116 blanking window, quads (fronto, tilted, projected planes), z-buffered view, ray/quad aim solver, gun registers |
| `nb1_table.c` | depth-table JSON (rules, decals, edits) and item matching |
| `nb1_edits.c` | edit engine (texture / punch / split), polygon rasteriser, hole fill |
| `nb1_png.c` | small PNG reader (zlib) |
| `nb1_json.c` | minimal JSON reader |
| `nb1_tool.c` | command-line harness used by the tests |

## Scope
The whole Python scene builder is ported, including the table `edits` (texture replacement, punch-outs, splits). Nothing here drives a
game core: input is a RAM snapshot in the `snap.lua` layout (`Nb1State`), so a core only has to fill that struct.
Depends on libc, libm and zlib (PNG textures and masks for edits).

## Build and test
```sh
make                                  # builds ./nb1_tool
PB_ROMS=<dir with the ptblank ROM files> PB_SNAPS=<snap.lua output dir> \
  python3 -I tests/run_oracle.py [--table table.json] [frame ...]
```
`run_oracle.py` needs ROMs and snapshots that are not in this repository (see the research repo's README for how to make them). It checks, per frame:
composite vs the oracle and vs MAME's own PNG; sprite entries and tiles; every item's fields, depth, texture and pen buffer;
the arcade-eye view and five orbit views; the aim solver on a fixed ray set; the gun registers over all pixels.

`tests/rules_stress.json` is a synthetic table that touches every supported rule key. It is not a real scene.

## Verification status (what was actually run)
* Default table, all 197 snapshots: composite == oracle == MAME; entries, items, views, aim, gun registers all match.
  (An earlier 9 failures were the harness printing floats with 12 digits and a mask field; both fixed and re-run.)
* `tests/rules_stress.json` and the real corridor, garden and tree tables: every 12th snapshot (17 frames each) match.
* Mutation check: deliberately breaking the eraser rule, tilt sign, depth slot gap and blanking window each make the test fail
  (on frames that exercise them: 3200 for tilt, 17450 for the blanking window). Changing the aim tie-break (`<` to `<=`) is NOT
  caught, because no test scene has two hits at exactly equal distance.
* Table `edits`: the real garden, corridor and tree tables and `tests/edits_stress.json` (texture of every PNG type, rect / poly / mask regions,
  clone / inpaint / none fills, punch with grow) match the oracle (see below for what was run).

## Aim snapping (added)
`nb1_visible_map()` records which item the arcade shows at each pixel; `nb1_aim_snap()` moves the aimed pixel (within 24 px) to a pixel
where the hit item is visible to the arcade eye, flags `snapped` / `hidden` / `cover`, and returns 0 on a miss (no fallback plane: the
caller keeps its last pixel). Measured on three scenes at yaw 20-85 degrees with random mouse rays: pixels where the hit item was visible to
the game went from 76% (381 of 498 hits; e.g. 1100 yaw 70: 62 of 83) to 98% (489 of 498); the rest are items fully covered or more than 24 px from a visible pixel.
Checked against the Python `aim_snap` (2,700 rays x 3 frames x 3 tables) and its id map (0 px differ).

## Decals (shot marks)
Sprites whose tile code is in the table's `"decals": [[lo, hi], ...]` (default: the shot-mark tiles 0xee5-0xee7, a 16x16 blob that grows
into a sparkle, seen 2/5/8 frames after the trigger in 86 of 265 traced shots) are placed ON the item the arcade drew them over:
the latest-drawn opaque non-decal item under their centre pixel. They inherit its plane, tilt and HUD flag, and their depth is
where the eye ray through their centre meets that surface (2 mm in front). Decals never block `nb1_aim` or the visible map. `"decals": []` turns it off.
Other hit effects (e.g. tiles 4bd/4ba, f78/f8a/f81) were seen after some shots but have not been examined, so they are not decals yet.
Checked: C == Python for 40 decal items (plane, upright and tilted owners); marks lie within 2 mm of the owner's plane with identical normals.

## Edits: where C, the Python oracle and the old tool differ
The edit operations are defined by the oracle (`oracle/depthscene.py`) and ported exactly. Three operations are defined by explicit rules
instead of by a library, because the libraries could not be reproduced bit-for-bit in C:
* **Polygon regions**: scanline fill in float32 (round half up on the left, half down on the right). Identical to Pillow on every polygon the
  real tables use (0 px); it differs from Pillow by a few pixels at thin vertices of ~10% of random polygons.
* **`inpaint` fill**: deterministic layer-by-layer weighted mean of nearby opaque pixels (integer arithmetic). Replaces OpenCV Telea, so the
  fills in the tree table look a little different (slightly streakier); garden and corridor frames checked were unchanged.
* **Texture resize** (a texture PNG whose size differs from the item's): exact integer nearest-neighbour. Textures exported by the tool are
  already the right size.
`tests/make_assets.py` regenerates the synthetic PNGs in `tests/assets/` (no game content).

## Stable table choice and fixed doors
`nb1_selector_*` wraps the table selection for a stream of frames: the current table is kept while the background art is still the same scene, so a sprite that a rule looks for coming and going no longer flips the whole layering (the hallway walls flickered because of that); it switches to another table after 3 consecutive frames that choose it. `pbdesk` and `pbvr` use it. The four big sprites of the gold doors between stages (tile codes 2289, 2401, 2415, 2528) have a fixed depth rule (2.6 m) in the default table and every draft table, whatever their priority or what else is on screen.

## The world around the play screen
`nb1_scene_add_surround` adds `NB1_SURROUND` items (textured polygons in world coordinates) built from the scene's own layers: big layers continued round the viewer as panoramas, a sky dome, ground / ceiling / walls, all cut so nothing lies inside the arcade picture's view pyramid (the arcade-eye render, aim and visible map ignore them). The recipe is a table's `"surround"` section; see `research/SURROUND.md` in the research repo. `pbdesk` and `pbvr` build it by default (`--no-surround` to switch it off). `nb1_tableset_set_roms` lets table selection also compare a coarse thumbnail of the picture, and `nb1_selector_*` keeps the choice steady.

## Desktop viewer: `pbdesk`
`make pbdesk` (needs SDL2 and OpenGL 3.3: macOS `brew install sdl2`, Debian/Ubuntu `apt install libsdl2-dev libgl-dev`; the Makefile says so if SDL2 is missing) builds `nb1/pbdesk`, an SDL2 + OpenGL viewer for the live scene:
one textured quad per item (planes are texture-mapped by projecting through the arcade eye in the fragment shader), an orbit camera, mouse-ray aim
with the snapping, and the same aim protocol as the Python viewer, so it plugs into `tools/play/pbplay.py --viewer c --pbdesk nb1/pbdesk`
in the research repo (MAME runs the game and publishes frames; pbdesk reads them and writes `aim.txt`). `--state FILE` shows a single saved state.
ROMs can be a folder or the MAME `ptblank.zip`. Keys are listed at the top of `desktop/pbdesk.c` (F1 in the window).
Screenshots are PPM (`F5`, or `--shot out.ppm`).
In orbit views each layer gets a metre ruler (ticks every 0.25 m, longer at 0.5 m and 1 m, labelled in metres, top label with layer, distance and height; red near to cyan far). `M` cycles rulers off / main / all; `--no-ruler`, `--ruler-all`.
Physical viewport: the picture is treated as a window of a 29" 4:3 screen (0.59 m wide) seen at 0.9 m (`--screen-diag`, `--screen-dist`), which fixes the viewing angle (36.3 deg across). The tables are authored for f = 346 px, so orbit-view depths are multiplied by 288 D / (W * 346) (x1.27 by default) while sizes and heights stay as authored; the arcade-eye picture is unchanged. `[` / `]` or `--viewport-scale K` make the window K times wider: same sizes, layers closer. Rulers and the aim text show the scaled distances.

What was checked (headless, Mesa software OpenGL under Xvfb; nothing here was run on macOS or Windows):
* Straight arcade view vs the library's software render: 0 of 64,512 px differ on all 8 frames tried (default, garden, corridor, tree, edits and rules stress tables).
* Orbit views vs the software renderer: at most 16 of 307,200 px differ (frames 500, 3200, 21050, 2450; yaw 20 to -80).
* `aim.txt` against the Python oracle for 14 mouse positions (hits, misses that keep the last pixel, snapped, hidden): all agree.
* End to end with real MAME 0.264 and a saved state: the game renders live, a scene build takes about 20 ms.
Known gaps: sprite shadow pens (fades) are drawn as a flat 50% darkening, not through the palette; no outline / label overlays yet; no sound (MAME can
play it with `--sound`); the gun registers are still written by the MAME script from `aim.txt` (as before).

## Automatic table selection
`nb1_tableset_load("scene_tables.json")` + `nb1_tableset_select(...)` pick a depth table per frame (the Python twin is `depthscene.select_table`).
Entries match by exact sprite codes (`codes_all` / `codes_any`, first wins) or by background-art similarity to captured reference scenes
(`art_scenes`, `min_similarity`, Jaccard over (layer, tile code) pairs from `scene_refs.json`; the most similar entry wins). `pbdesk --tables scene_tables.json`
uses it; an explicit `--table` overrides. `tests/run_select.py` checks C against Python (318 of 318 states agreed). See the research repo's `research/DEPTH-SCENES.md`.

## Sprite library

`nb1_sprites.c` gives access to every sprite of the sprite ROM at any time, whether or not the current scene has it loaded (for decorating scenes).
The ROM (32,768 tiles of 16x16 pixels, 8 bpp, 255 = transparent; 22,956 are not empty) holds pixels only. Which tiles form a sprite, and the palette, are written into sprite RAM by the game at run time, so
`research/tools/nb1/make_sprite_library.py` collects them from the captured scenes and from long MAME runs (`research/data/sprites`): **1,566 frames** (columns x rows of tiles plus up to 6 palettes each, 700 palettes) covering 16,268 of the 22,956 non-empty tiles (71%).
The long runs are `tools/mame/trace.lua` with `PB_HARVEST` (video-state dumps), `PB_KEEP=0x1c00b6=3` (keeps the lives full so the bot gets through every stage), `PB_MODE` (shoots the training / beginner / expert / very hard button) and `PB_STATE` (starts from each of the 121 save states), consumed by `harvest_sprites.py`; about 115,000 dumps in all.
`nb1_sprlib_load`, `nb1_sprlib_frame_tex` (a frame with one of its palettes) and `nb1_sprlib_tiles_tex` (any run of ROM tiles, any grid; palette of the nearest seen tile, grey if none) return `Nb1Tex`, like `nb1_entry_tex`.
`nb1_tool sprlib LIB ROMS OUT [tiles FIRST NCOLS NROWS]` writes them out. The C and Python renderings of all frames are identical.
Not extracted: the shapes and palettes of the other 6,688 non-empty tiles (the game's composition data is not stored verbatim in the program ROM; the library keeps a guess in `guess.json` that is about 75% right on known sprites).
