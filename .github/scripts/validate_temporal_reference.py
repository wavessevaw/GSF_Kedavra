"""Validate the shape and provenance of a software-Vulkan observation.

This is an integrity check, not an image-quality or hardware acceptance test.
"""
import hashlib
import json
import math
import pathlib
import sys

BASE = "62621c4f9b60fd3cb95edafb62a1a44a1bb2c21e"
GAMESCOPE = "aa289ea5977d8b21b92b0f246f0e8f0c225d180f"
SCENES = {"h_turn64": {3}, "h_parallax": {3}, "h_lines_slow": {2, 3}}
METRICS = {"psnr", "psnr_worst", "errgrad", "blk", "badblk_pct", "badblk_max", "temporal", "lineflk", "sharpflk_pct"}


def validate(root):
    root = pathlib.Path(root)
    observations = {}
    for scene, expected_modes in SCENES.items():
        path = root / f"{scene}.json"
        raw = path.read_bytes()
        document = json.loads(raw)
        if document.get("size") != [1280, 800] or "llvmpipe" not in document.get("device", "").lower():
            raise ValueError(f"unexpected resolution or device: {scene}")
        rows = document.get("hard")
        if not isinstance(rows, list) or len(rows) != len(expected_modes):
            raise ValueError(f"missing or duplicate modes: {scene}")
        seen = set()
        for row in rows:
            mode = row.get("mode")
            if row.get("scene") != scene or type(mode) is not int or mode not in expected_modes or mode in seen:
                raise ValueError(f"unexpected scene or mode: {scene}")
            seen.add(mode)
            for key in METRICS:
                value = row.get(key)
                if type(value) not in (int, float) or not math.isfinite(value):
                    raise ValueError(f"invalid {key}: {scene} x{mode}")
        if seen != expected_modes:
            raise ValueError(f"missing modes: {scene}")
        observations[scene] = {"sha256": hashlib.sha256(raw).hexdigest(), "modes": sorted(seen)}
    return {
        "classification": "software Vulkan simulation; no hardware or visual quality acceptance",
        "quality_status": "UNVERIFIED",
        "gsf_commit": BASE,
        "gamescope_commit": GAMESCOPE,
        "resolution": [1280, 800],
        "frames_per_scene": 8,
        "evidence": observations,
    }


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: validate_temporal_reference.py CAPTURE_DIRECTORY")
    directory = pathlib.Path(sys.argv[1])
    result = validate(directory)
    (directory / "manifest.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Observation structure: PASS; image quality and hardware: UNVERIFIED")
