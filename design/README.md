# Design data

Our own decisions about how the arcade's flat layers become a 3D scene. Numbers and sprite/tile codes only, no game pictures, so the app can ship with them.

* `tables/` one depth table per family of scenes (`garden.json`, `corridor.json`, ...) and `scene_tables.json`, which picks the table for a scene from the sprite tile codes it contains
  (and, where given, from the reference art in `scene_refs.json`: see below).
* `families.json` the hand classification of the 121 captured scenes into families (which scenes share background art, and what kind of depth design each family needs).
* `examples/` the small example tables used by the depth demos.

Not here, because they come from the game: `scene_refs.json` (tile codes and 16x11 thumbnails of the reference art) and every picture. They live in the private research repository
(`point-blank-research/game/scenes/scene_refs.json`, `design/renders/`). The loaders look for `scene_refs.json` next to `scene_tables.json`; in the research repository
`research/data/tables/` is a directory of symlinks that puts the two together (to be replaced by an explicit path option).

The scene numbers used in these files are the ones of the scene folders in the research repository (`game/scenes/NNN-name/`; see `research/legacy/LEGACY_SCENES.md` for the numbers as they were before the reorganisation).
