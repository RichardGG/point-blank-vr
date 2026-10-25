#!/usr/bin/env python3
"""Generate the synthetic PNGs used by edits_stress.json (no game content). Run once; the PNGs are committed."""
import numpy as np, os
from PIL import Image
here = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'assets')
os.makedirs(here, exist_ok=True)
rng = np.random.default_rng(5)
# RGBA texture, deliberately not the item size (40x30) so the nearest-neighbour resize is exercised; some transparent holes
yy, xx = np.mgrid[0:30, 0:40]
rgba = np.stack([xx * 6 % 256, yy * 8 % 256, (xx + yy) * 4 % 256, np.where((xx // 5 + yy // 5) % 4 == 0, 0, 255)], -1).astype(np.uint8)
Image.fromarray(rgba, 'RGBA').save(os.path.join(here, 'tex_rgba.png'))
Image.fromarray(rgba[..., :3], 'RGB').resize((57, 41)).save(os.path.join(here, 'tex_rgb.png'))
Image.fromarray(rgba[..., 0], 'L').resize((288, 224)).save(os.path.join(here, 'tex_gray.png'))
Image.fromarray(rgba[..., [0, 3]].copy(), 'LA').resize((33, 33)).save(os.path.join(here, 'tex_la.png'))
pal = Image.fromarray((xx // 4 * 3 + yy // 4) .astype(np.uint8), 'P'); pal.putpalette([int(v) for v in rng.integers(0, 256, 768)])
pal.info['transparency'] = 0; pal.save(os.path.join(here, 'tex_pal.png'), transparency=0)
# 288x224 mask image: a white blob + a coloured (non-white) blob + alpha-only region
m = np.zeros((224, 288, 4), np.uint8); Y, X = np.mgrid[0:224, 0:288]
m[((X - 90) ** 2 + (Y - 80) ** 2) < 45 ** 2] = (255, 255, 255, 255)
m[((X - 200) ** 2 + (Y - 150) ** 2) < 40 ** 2] = (0, 0, 0, 255)            # opaque black: counts only with "mask_white": false
m[(X > 240) & (Y < 60)] = (10, 10, 10, 128)
Image.fromarray(m, 'RGBA').save(os.path.join(here, 'mask_shape.png'))
print('assets written to', here)
