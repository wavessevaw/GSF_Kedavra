"""Check every generated phase of an unscaled hard-suite crosshair sequence."""
import argparse
from pathlib import Path
from statistics import mean


def samples(path):
    magic, dimensions, maximum, pixels = path.read_bytes().split(b"\n", 3)
    width, height = map(int, dimensions.split())
    if magic != b"P6" or maximum != b"255" or len(pixels) != width * height * 3:
        raise ValueError(f"Invalid PPM: {path}")
    values = []
    for y in range(height // 2 - 18, height // 2 + 18):
        for x in range(width // 2 - 18, width // 2 + 18):
            dx, dy = abs(x + 0.5 - width / 2), abs(y + 0.5 - height / 2)
            if max(dx, dy) < 2 or (min(dx, dy) < 2 and 4 <= max(dx, dy) < 18):
                i = (y * width + x) * 3
                values.extend(pixels[i:i + 3])
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("scene", choices=["h_courtyard", "h_courtyard_reverse", "h_courtyard_reversal", "h_lines_slow"])
    parser.add_argument("--max-error", type=float, help="Maximum allowed mean RGB error in any single generated frame")
    args = parser.parse_args()
    passed = True
    for multiplier in (2, 3):
        for phase in range(1, multiplier):
            errors = []
            for frame in range(1, 8):
                name = f"{args.scene}_x{multiplier}_f{frame}_t{phase}"
                generated, reference = (samples(args.directory / f"{name}_{kind}.ppm") for kind in ("gen", "truth"))
                errors.append(mean(abs(a - b) for a, b in zip(generated, reference, strict=True)))
            print(f"{args.scene} {phase}/{multiplier}: mean {mean(errors):.3f}, worst {max(errors):.3f} / 255")
            if args.max_error is not None and max(errors) > args.max_error:
                passed = False
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
