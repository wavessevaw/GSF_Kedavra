#!/usr/bin/env python3
"""Standalone Deck timestamp comparison; standard library only. Does not control Gamescope.

Run a quiet device first, then repeat while a reproducible game replay is active.
Do not interpret this isolated pass as complete compositor performance.
"""
import argparse
import json
import math
from pathlib import Path
import random
import statistics
import stat
import subprocess

p = argparse.ArgumentParser()
p.add_argument("--bench-dir", type=Path, default=Path(__file__).resolve().parent)
p.add_argument("--out", type=Path, required=True)
p.add_argument("--samples", type=int, default=200)
p.add_argument("--size", default="1280x800")
a = p.parse_args()
w,h = map(int, a.size.split("x"))
if not 1 <= a.samples <= 10000 or not (1 <= w <= 8192 and 1 <= h <= 8192):
    p.error("invalid dimensions/sample count")
root = a.out.resolve(); root.mkdir(parents=True, exist_ok=True)
bundle = a.bench_dir.resolve()
research = (bundle/"kfg_detailtest").exists()
exe = bundle/("kfg_detailtest" if research else "kfg_posttest")
spv = bundle/("research" if research else ".")
if any(c.isspace() for c in str(root)):
    p.error("--out path must not contain whitespace (batch manifest format)")
# Decky extracts ZIPs without execute bits. Prefer the already installed runtime
# loader, which also supplies the bundled glibc on older SteamOS installations.
runtime = Path("/var/lib/kedavra/runtime")
loader = runtime/"lib/ld-linux-x86-64.so.2"
if loader.exists():
    command = [str(loader), "--library-path", str(runtime/"lib"), str(exe)]
else:
    exe.chmod(exe.stat().st_mode | stat.S_IXUSR)
    command = [str(exe)]
modes = ["off", "cas=0.5", "dls=0.5:0.17", "detail=0.5", "detail=1"]
if research: modes.append("rcas=0.5")
rng = random.Random(1500)
src = bytearray()
for y in range(h):
    for x in range(w):
        v = int(max(0,min(255,128+38*math.sin(x*.18)*math.cos(y*.13)+rng.uniform(-3,3))))
        src.extend((v,v,v,255))
(root/"input.rgba").write_bytes(src)
rows = []
for _ in range(a.samples):
    order = modes.copy(); rng.shuffle(order)
    rows += [f"{w} {h} {spec} {root}/input.rgba {root}/output.rgba 1" for spec in order]
(root/"batch.txt").write_text("\n".join(rows)+"\n")
with (root/"timestamps.log").open("w") as f:
    subprocess.run(command+["--spv",str(spv),"--batch",str(root/"batch.txt")],stdout=f,stderr=subprocess.STDOUT,check=True)
device, timings = "unknown", {}
for line in (root/"timestamps.log").read_text().splitlines():
    if line.startswith("device:"): device = line[8:]
    if line.startswith("SAMPLE\t"):
        _,_,spec,_,ms = line.split("\t")
        timings.setdefault(spec,[]).append(float(ms))
def pct(v, q):
    s=sorted(v); x=(len(s)-1)*q; i=int(x)
    return s[i]+(s[min(i+1,len(s)-1)]-s[i])*(x-i)
summary = {"device":device,"resolution":[w,h],"scope":"isolated Vulkan dispatch; not total composite or frame pacing",
           "steam_deck_identity_verified":False,"software_driver":any(s in device.lower() for s in ["llvmpipe","lavapipe","swiftshader"]),
           "modes":{m:{"samples_ms":v,"mean":statistics.mean(v),"p50":pct(v,.5),"p95":pct(v,.95),"p99":pct(v,.99)} for m,v in timings.items()}}
(root/"results.json").write_text(json.dumps(summary,indent=2)+"\n")
print(json.dumps({k:v for k,v in summary.items() if k!="modes"},indent=2))
for m,v in summary["modes"].items():
    print(m, {k:round(x,5) for k,x in v.items() if k!="samples_ms"})
