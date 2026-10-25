# Point Blank VR

Quest 3 / PCVR port of Namco's NB-1 *Point Blank* (*Gun Bullet*) with in-world depth: sprites placed at per-object depths, some on
projected planes, HUD kept flat, VR controller as the gun. Research, tooling and measurements live in the private
`point-blank-research` repository.

## Status: seed only, does not build yet

This repository was seeded from the Time Crisis VR host. Nothing here runs Point Blank yet.

Kept (generic): OpenXR host and frame loop, GLES3 compatibility layer with multiview stereo, clock decoupling, refresh and
resolution handling, menu and options, gun model, Android/PC packaging scripts.

Still Time Crisis specific, to be replaced: `quest/quest_scene.c` (polygon world and aim), the sprite path, `quest/CMakeLists.txt`
and `tools/build*.py` (they expect the `upstream` submodule and a lifted game), Java package `org.timecrisis`, launcher art
(removed), names and strings. The original README and docs are under `docs/time-crisis-vr/` for reference.

Removed from the seed: the `upstream` submodule, upstream patcher and crate translator, release notes, launcher artwork, TC-specific reports.

See [docs/PLAN.md](docs/PLAN.md).
