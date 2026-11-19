"""Build a slowed side-by-side from two KFG_DETAIL_SEQUENCE_OUT directories."""
import argparse
import subprocess
from pathlib import Path

from PIL import Image, ImageDraw

parser = argparse.ArgumentParser()
parser.add_argument("baseline", type=Path)
parser.add_argument("candidate", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
times = [0, 33, 61, 108, 143, 199, 230, 272]
fps, slowdown = 60, 8
cmd = ["ffmpeg", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt",
       "rgb24", "-s", "1920x432", "-r", str(fps), "-i", "-", "-an",
       "-c:v", "libx264", "-crf", "17", "-pix_fmt", "yuv420p", str(args.output)]
with subprocess.Popen(cmd, stdin=subprocess.PIPE) as encoder:
    for i, (start, end) in enumerate(zip(times, times[1:]), start=1):
        for phase in range(3):
            frame = Image.new("RGB", (1920, 432))
            draw = ImageDraw.Draw(frame)
            name = f"h_courtyard_x3_f{i}_t{phase}"
            for col, (path, suffix, label) in enumerate([
                (args.baseline, "gen", "GFG 3.5.0 / Response90"),
                (args.candidate, "gen", "Courtyard candidate"),
                (args.candidate, "truth", "3D reference")]):
                with Image.open(path / f"{name}_{suffix}.ppm") as image:
                    frame.paste(image.resize((640, 400), Image.Resampling.LANCZOS), (640 * col, 32))
                draw.text((640*col+10, 8), f"{label}   |   {end-start} ms pair   |   phase {phase}/3   |   8x slow")
            t = lambda j: round((start + (end-start)*j/3)*fps*slowdown/1000)
            encoder.stdin.write(frame.tobytes() * (t(phase+1)-t(phase)))
    encoder.stdin.close()
    if encoder.wait():
        raise SystemExit("Video encoding failed")
