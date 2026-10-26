# pbvr: PC VR host (OpenXR + OpenGL)

`pbvr` draws the depth scene in stereo in a headset and sends the controller aim back to MAME. It uses the same scene code as `pbdesk` (`desktop/glscene.h`) and the same files with MAME (`tools/play/pbplay.lua` in the research repo): states in, `aim.txt` out.

    pbvr --roms DIR --ipc DIR [--tables scene_tables.json | --table T.json] [--screen-diag 29] [--screen-dist 0.9] [--viewport-scale 1] [--gun-pitch 0] [--gaze-aim] [--no-vr]

With `pbplay.py`: `python3 pbplay.py ... --viewer vr --pbdesk path/to/pbvr`.

* **Runtime:** any OpenXR runtime with OpenGL support (SteamVR, Meta Quest Link/Oculus, WMR). The OpenXR loader (`openxr_loader.dll`, Linux `libopenxr_loader.so.1`) is loaded when the program starts, so put the DLL next to `pbvr.exe`; the program tells you if it is missing or no runtime is active.
* **Space:** the arcade eye is your head position when you recenter (at start, or right stick click). The picture is a window of `--screen-diag` inches at `--screen-dist` metres; layers keep their authored sizes at a depth scale of f_eff/346 (see the nb1 README, "Physical viewport"). `[` and `]` (window focused) change the viewport size.
* **Controls (Touch; Index and the KHR simple controller are bound too):** trigger = fire (the hand that pulled it last is the gun), A/X = coin, B/Y = start, left stick click = pause the game (MAME pauses), right stick click = recenter. Keyboard on the mirror window: C coin, V start, P pause, R recenter, ESC quit. A laser and a red cross show where the shot lands; the controller buzzes on each shot. With no tracked controller the head aims (`--gaze-aim` forces that).
* **Threading:** a worker thread reads MAME's states and builds scenes; the render thread only uploads a finished scene and draws, so a slow scene build does not drop headset frames.
* **Sound:** MAME plays the game audio itself on the Windows default output device, so start it **without** `-sound none` and set Windows' default playback device to the headset (or the speakers). `Play-PC.ps1` starts MAME (with sound) and pbvr together.
* **Build:** Linux `make pbvr` (needs SDL2, the OpenXR headers `libopenxr-dev`); Windows cross-compile with `build_windows.sh` (MinGW-w64; see the script header), or build the same three source groups with MSVC/MinGW on Windows.

Status: compiled for Windows (MinGW); on Linux run against Monado's simulated HMD (software Vulkan/GL) to check the session, swapchains, frame loop, worker thread, MAME link and the aim chain. NOT yet run on Windows, with a real headset, or with real controllers.

## Pause-mode editor (layer depth tuning)

Pause the game (left stick click, or `P` on the mirror window), and the buttons edit instead of playing. A text panel below the picture says what is selected.

| Control | Action |
|---|---|
| trigger | select the layer (or sprite type) the laser points at; it is outlined in yellow |
| grip | switch the scope: whole layer (a tilemap layer, or every sprite of that priority) / this sprite type only (every sprite with the same tile code) |
| left stick up / down | scale the selected group: push it away / pull it closer along the arcade eye's rays, so the arcade picture never changes, only its real size and distance |
| right stick up / down | adjust the current target: viewport size (bigger window = layers closer, sizes unchanged), screen tilt, floor height, ceiling height, or how far the HUD sits below the picture. Right stick click while paused cycles the target (the panel under the picture marks it with `>`) |
| A (right) | jump to the selected layer at real size (`--jump-dist`, 1 m away) and back to the playing area |
| B (right) | save the edits |
| X (left) / Y (left) | reset the selected group / every edit of this scene |

The controls are also listed on a panel to your left (turn your head left) and this scene's edits on a panel to your right.

Keyboard stand-ins on the mirror window: Enter select, G scope, Up/Down scale, PageUp/PageDown viewport, J jump, S save, X, Y.

**Tilt, floor and HUD.** The play window is level by default (tilt 0 for every scene; a table's `surround.pitch` or the editor can tilt it). The floor is at the bottom edge of the deepest continued layer and the ceiling at its top, so neither covers the wrapped background (scenes whose art also continues vertically, like the tower, keep the recipe's heights). Only sideways tiling or mirroring is used; vertically the art continues only where the game's own map does. The HUD (hearts, timer, bullets) is moved down below the picture (`--hud-below N` arcade pixels, default 72; `--hud-overlay` keeps it on the picture) and white corner marks show the play window (`--no-brackets` hides them). The tilt, floor and ceiling edits are saved with the scale edits (`"world"` in the save) and `merge_scales.py` writes the changed ones into the table's `surround`. The values for each family are estimates to be tuned in the headset.

**Labels for recording.** The scene name (number, family, table) is written under the picture, and every HUD sprite carries a small label (`S<priority> #<tile code>`, tilemap layers as `tilemap L<n>`). HUD items are never moved by the edit tools; the labels just say what they are. `--no-labels` hides all of it.

Edits are kept per scene (the captured scene whose background art is nearest) and applied by the library as a table's `"scales": [{"match": {...}, "scale": k}]` (depth and plane distance times k, non-HUD items only; all matching entries multiply). Saving appends one line to `pbvr_layer_edits.jsonl` (`--edits-out`): the scene (table name, nearest scene number and similarity, game frame), the viewport settings, and for every edit the layer, its match, the scale, and the depth before and after in authored and real metres, plus a ready-to-paste `table_snippet`. `tools/merge_scales.py pbvr_layer_edits.jsonl --scene N --table T.json` writes the last save of scene N into the table (`--list` shows what was saved).

What "split a sprite from its layer" means here: choosing the sprite-type scope gives that tile code its own scale, separate from the rest of the layer. Sprites are only moved in depth (along the eye's rays), not sideways, and a single sprite instance can't be picked apart from others with the same code.


## Jumping between scenes (testing)

`research/data/scenes` in the research repo holds one frozen snapshot per captured scene (121 scenes, 4.5 MiB): the RAM the viewers read, composed like `pbplay.lua` publishes it. `pbvr --scenes DIR [--scene N]` shows them without MAME (unplayable, paused, the editor panels on; left stick click switches those off); **flick either stick sideways** (or press Left / Right on the mirror window) for the previous / next scene. The panel under the picture names the scene number, family and label. Edits you save are filed under that scene number.

`--live` (with `--ipc` and MAME started with `-state_directory <research>\states`) does the same with the playable MAME save states instead: each flick makes MAME load that scene's state (0.264 only) and play on from there. `Play-PC.ps1 -Scenes <path> [-Scene N] [-Live]` starts it all; `pbdesk --scenes DIR --scene N` is the desktop version (Left / Right). Test aids: `PBVR_TOUR=n` visits n scenes and saves a screenshot of each (`PBVR_SHOT=prefix`).
