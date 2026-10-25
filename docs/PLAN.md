# Plan

1. **C scene builder** (port of `nb1render.py` and `depthscene.py` from the research repo): takes NB-1 RAM, palette and ROM
   data, produces depth-ordered RGBA quads, planes and a ray/quad aim solver. The Python code is the test oracle.
2. **Aim fix**: snap to a pixel where the hit object is visible to the arcade eye; aim against the frame being shown (see research notes).
3. **Host integration (PCVR first)**: an `nb1` game module behind the host interface; feed quads to a new RGBA quad draw path in
   `quest_gl.c` (unverified whether the Time Crisis sprite/atlas path can take them).
4. **Input**: aim ray to arcade pixel to gun registers (`gun_regs`, verified 99/99 against MAME).
5. **Core**: MAME-state-driven first, then a standalone NB-1 core (68EC020, C352, M37702 are the open items).
6. **Quest packaging**: reuse the existing APK/signing flow; rename package and strings.
7. **Depth authoring** for the other scenes.

Rules: no ROMs or ROM-derived content in this repo.
