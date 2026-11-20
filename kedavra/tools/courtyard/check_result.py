"""Regression limits on paired forward/backward runs, using the same Vulkan driver."""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--snap", action="store_true")
    args = parser.parse_args()
    old, new = (json.loads(p.read_text()) for p in (args.baseline, args.candidate))
    for direction in ("forward", "backward"):
        for a, b in zip(old[direction]["hard"], new[direction]["hard"], strict=True):
            if a["mode"] != b["mode"]:
                raise SystemExit("Mismatched phase counts")
            # Sudden changes should be left close to linear interpolation.
            limit = a["temporal"] + 0.1 if args.snap else a["temporal"] * 0.99
            if b["temporal"] > limit:
                raise SystemExit(f"{direction} x{b['mode']}: temporal error {b['temporal']} > {limit}")
        for key, a in old[direction]["sides"].items():
            b = new[direction]["sides"][key]
            if a["pixels"] != b["pixels"] or b["mae"] > a["mae"] + 0.15:
                raise SystemExit(f"{direction} {key}: {a} -> {b}")


if __name__ == "__main__":
    main()
