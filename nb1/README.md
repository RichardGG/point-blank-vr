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
