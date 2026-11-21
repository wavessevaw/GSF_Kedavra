#!/usr/bin/env python3
"""Compare both source directions of the ray-rendered pillar crossing (SDR)."""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def pixels(path):
    with Image.open(path) as image:
        return np.asarray(image, dtype=np.int16)


def opaque(image):
    return (image == [39, 52, 63]).all(axis=2)


def compare(before, after):
    report = {}
    for mode, phase in ((2, 1), (3, 1), (3, 2)):
        errors = np.zeros(2)
        stuck = np.zeros(2, dtype=int)
        count = 0
        for frame in range(1, 8):
            name = f"h_pillar_crossing_x{mode}_f{frame}_t{phase}"
            names = [f"h_pillar_crossing_source{f}.ppm" for f in (frame-1, frame)]
            names.append(f"{name}_truth.ppm")
            sources = [pixels(before / n) for n in names]
            for n, data in zip(names, sources):
                np.testing.assert_array_equal(data, pixels(after / n))
            a, b, truth = sources
            # Only used for scoring. The engine never receives these colours or masks.
            actor = (truth[..., 0] - truth[..., 2] > 30) & (truth[..., 0] - truth[..., 1] > 20)
            mask = (opaque(a) != opaque(b)) & actor
            count += int(mask.sum())
            for i, folder in enumerate((before, after)):
                gen = pixels(folder / f"{name}_gen.ppm")
                stuck[i] += np.count_nonzero(mask & opaque(gen))
                errors[i] += np.abs(gen[mask] - truth[mask]).sum() / 3
        assert count > 0, "Empty silhouette mask"
        report[f"{phase}/{mode}"] = dict(samples=count, mean_rgb_error=(errors/count).tolist(),
                                         opaque_on_actor=stuck.tolist())
    return report


if __name__ == "__main__":
    report = compare(*map(Path, sys.argv[1:]))
    print(json.dumps(report, indent=2))
    before, after = np.sum([r["opaque_on_actor"] for r in report.values()], axis=0)
    assert after < before, "The number of opaque samples on the actor did not improve"
    # Also check actual RGB accuracy. Merely changing an opaque sample by one code
    # value passes the first count while reconstructing essentially the same wrong colour.
    total = sum(np.array(r["mean_rgb_error"]) * r["samples"] for r in report.values())
    assert total[1] < total[0], "The reconstructed silhouette became less accurate"
