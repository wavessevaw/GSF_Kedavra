"""Measure foreground exposure and coverage on a Courtyard sequence in both directions."""
import argparse
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image


def read(path):
    with Image.open(path) as image:
        return np.array(image)


def fence(rgb):
    # Exact opaque RGB of the analytic grille. Partial coverage is counted outside the opaque mask.
    return np.all(rgb == [22, 33, 41], axis=-1)


def measure(args, out, backwards):
    scene = args.scene
    cmd = [args.exe, "--spv", str(args.spv), "--hard", "--size", args.size, "--scene", scene,
           "--json", str(out / "score.json")]
    if args.control:
        cmd.append("--" + args.control + "-courtyard")
    if backwards:
        cmd.append("--reverse-courtyard")
    if args.regular:
        cmd.append("--regular-courtyard")
    subprocess.run(cmd, check=True, env={**os.environ, "KFG_DETAIL_SEQUENCE_OUT": str(out), "KFG_HARD_X2": "1"})
    hashes = {}
    totals = {}
    for f in range(8):
        image = read(out / f"{scene}_source{f}.ppm")
        hashes[("source", 7 - f if backwards else f)] = hashlib.sha256(image).digest()
    for f in range(1, 8):
        a = fence(read(out / f"{scene}_source{f-1}.ppm"))
        b = fence(read(out / f"{scene}_source{f}.ppm"))
        for m, j in ((2, 1), (3, 1), (3, 2)):
            stem = f"{scene}_x{m}_f{f}_t{j}"
            truth = read(out / (stem + "_truth.ppm"))
            tick = (f - 1) * m + j
            hashes[(m, 7 * m - tick if backwards else tick)] = hashlib.sha256(truth).digest()
            error = abs(read(out / (stem + "_gen.ppm")).astype(np.int16) - truth).sum(axis=-1)
            target = fence(truth)
            for label, mask in (("revealed", a & ~b & ~target), ("covered", ~a & b & target)):
                values = error[mask]
                key = f"{j}/{m} {label}"
                item = totals.setdefault(key, [0, 0])
                item[0] += int(values.sum())
                item[1] += values.size
    return hashes, {"hard": json.loads((out / "score.json").read_text())["hard"],
                    "sides": {k: {"mae": v / (3*n), "pixels": n} for k, (v,n) in totals.items()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--spv", required=True, type=Path)
    parser.add_argument("--scene", default="h_courtyard_reversal", choices=["h_courtyard", "h_courtyard_reversal"])
    parser.add_argument("--size", default="1280x800")
    parser.add_argument("--control", choices=["snap", "stop"])
    parser.add_argument("--regular", action="store_true")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    results = {}
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)
        fwd, results["forward"] = measure(args, path, False)
        rev, results["backward"] = measure(args, path, True)
    if fwd != rev:
        raise RuntimeError("Forward and reverse sequences do not have identical source and reference pixels")
    results["matching_images"] = len(fwd)
    args.out.write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
