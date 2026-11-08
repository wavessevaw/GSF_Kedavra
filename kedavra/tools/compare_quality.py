#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare saved Vulkan outputs against identical analytic ground truth.
Requires numpy and Pillow. SSIM uses non-overlapping 8x8 luminance windows.
No hardware timing is inferred from images.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image


def read(path):
    return np.asarray(Image.open(path), dtype=np.float64) / 255


def metrics(image, truth):
    a, b = (x @ np.array([.2126, .7152, .0722]) for x in (image, truth))
    h, w = a.shape
    def tiles(x):
        return x[:h//8*8, :w//8*8].reshape(h//8, 8, w//8, 8).transpose(0, 2, 1, 3).reshape(-1, 64)
    x, y = tiles(a), tiles(b)
    mx, my = x.mean(1), y.mean(1)
    vx, vy = x.var(1), y.var(1)
    cov = ((x-mx[:, None])*(y-my[:, None])).mean(1)
    ssim = ((2*mx*my+.01**2)*(2*cov+.03**2)/((mx*mx+my*my+.01**2)*(vx+vy+.03**2))).mean()
    edge = np.mean([np.abs(np.diff(a, axis=k)-np.diff(b, axis=k)).mean() for k in (0, 1)])
    return dict(mae=float(np.abs(image-truth).mean()), ssim_8x8=float(ssim), gradient_mae=float(edge))


def compare(before, after):
    rows = []
    for old in sorted(before.glob('*_gen.ppm')):
        new = after / old.name
        truth_name = old.name.replace('_gen.ppm', '_truth.ppm')
        if not new.exists():
            raise ValueError(f'missing candidate {new}')
        truth = read(before / truth_name)
        if not np.array_equal(truth, read(after / truth_name)):
            raise ValueError(f'ground truth differs: {truth_name}')
        rows.append(dict(frame=old.name, baseline=metrics(read(old), truth), candidate=metrics(read(new), truth)))
    if not rows:
        raise ValueError('no comparable frames')
    return dict(method='RGB MAE, 8x8 luminance SSIM, luminance gradient MAE; synthetic Vulkan correctness, not Deck validation', frames=rows)

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('baseline', type=Path)
    p.add_argument('candidate', type=Path)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    args.out.write_text(json.dumps(compare(args.baseline, args.candidate), indent=2)+'\n')
