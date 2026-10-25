# pbvr: PC VR host (OpenXR + OpenGL)

`pbvr` draws the depth scene in stereo in a headset and sends the controller aim back to MAME. It uses the same scene code as `pbdesk` (`desktop/glscene.h`) and the same files with MAME (`tools/play/pbplay.lua` in the research repo): states in, `aim.txt` out.

    pbvr --roms DIR --ipc DIR [--tables scene_tables.json | --table T.json] [--screen-diag 29] [--screen-dist 0.9] [--viewport-scale 1] [--gun-pitch 0] [--gaze-aim] [--no-vr]

With `pbplay.py`: `python3 pbplay.py ... --viewer vr --pbdesk path/to/pbvr`.

* **Runtime:** any OpenXR runtime with OpenGL support (SteamVR, Meta Quest Link/Oculus, WMR). The OpenXR loader (`openxr_loader.dll`, Linux `libopenxr_loader.so.1`) is loaded when the program starts, so put the DLL next to `pbvr.exe`; the program tells you if it is missing or no runtime is active.
* **Space:** the arcade eye is your head position when you recenter (at start, or right stick click). The picture is a window of `--screen-diag` inches at `--screen-dist` metres; layers keep their authored sizes at a depth scale of f_eff/346 (see the nb1 README, "Physical viewport"). `[` and `]` (window focused) change the viewport size.
* **Controls (Touch; Index and the KHR simple controller are bound too):** trigger = fire (the hand that pulled it last is the gun), A/X = coin, B/Y = start, left stick click = pause the game (MAME pauses), right stick click = recenter. Keyboard on the mirror window: C coin, V start, P pause, R recenter, ESC quit. A laser and a red cross show where the shot lands; the controller buzzes on each shot. With no tracked controller the head aims (`--gaze-aim` forces that).
* **Threading:** a worker thread reads MAME's states and builds scenes; the render thread only uploads a finished scene and draws, so a slow scene build does not drop headset frames.
* **Build:** Linux `make pbvr` (needs SDL2, the OpenXR headers `libopenxr-dev`); Windows cross-compile with `build_windows.sh` (MinGW-w64; see the script header), or build the same three source groups with MSVC/MinGW on Windows.

Status: compiled for Windows (MinGW); on Linux run against Monado's simulated HMD (software Vulkan/GL) to check the session, swapchains, frame loop, worker thread, MAME link and the aim chain. NOT yet run on Windows, with a real headset, or with real controllers.
