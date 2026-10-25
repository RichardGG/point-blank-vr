# nb1: Point Blank video and depth-scene library (C)

C port of the Python research code (`oracle/nb1render.py`, `oracle/depthscene.py`, copied unchanged from the research repo).
No dependencies beyond libc/libm.

| File | Contents |
|---|---|
| `nb1.h` | public API |
| `nb1_video.c` | ROM and snapshot loading, C116 palette, C123 tilemaps, C355 sprite decode and stretch, full composite (equals MAME's picture) |
| `nb1_scene.c` | depth-table rules, sprite-buffer erasers, C116 blanking window, quads (fronto, tilted, projected planes), z-buffered view, ray/quad aim solver, gun registers |
| `nb1_json.c` | minimal JSON reader for depth tables |
| `nb1_tool.c` | command-line harness used by the tests |

## Not ported yet
Table `edits` (texture replacement, punch-outs, splits and the inpainting they need) are parsed and counted but ignored.
Everything else in the Python scene builder is covered. Nothing here drives a game core: input is a RAM snapshot in the
`snap.lua` layout (`Nb1State`), so a core only has to fill that struct.

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
* Table `edits` are not covered at all (not ported).

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
