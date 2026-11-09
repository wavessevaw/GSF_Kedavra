#!/usr/bin/env python3
"""Reproducible spatial-filter lab. Requires numpy, scipy, Pillow; not runtime dependencies.

The executable runs actual Vulkan SPIR-V. Images are never processed by a CPU shader port.
Synthetic ground truth measures controlled degradation only, not recovered game detail.
"""
import argparse
import hashlib
import html
import json
import re
from pathlib import Path
import subprocess

import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from scipy.ndimage import maximum_filter, minimum_filter, uniform_filter

MODES = {"off": "off", "cas": "cas=0.5", "dls": "dls=0.5:0.17",
         "rcas": "rcas=0.5", "detail": "detail=0.5", "detail_max": "detail=1"}


def luma(rgb):
    return np.asarray(rgb, dtype=np.float64)[..., :3] @ [0.2126, 0.7152, 0.0722]


def metrics(src, out, ref):
    s, o, r = map(luma, (src, out, ref))
    mse = np.mean((np.asarray(out, float)-np.asarray(ref, float))**2)
    mu1, mu2 = uniform_filter(o, 7), uniform_filter(r, 7)
    v1 = uniform_filter(o*o, 7)-mu1*mu1
    v2 = uniform_filter(r*r, 7)-mu2*mu2
    cov = uniform_filter(o*r, 7)-mu1*mu2
    ssim = ((2*mu1*mu2+6.5025)*(2*cov+58.5225) /
            ((mu1*mu1+mu2*mu2+6.5025)*(v1+v2+58.5225)))
    low = minimum_filter(src, size=(3, 3, 1))
    high = maximum_filter(src, size=(3, 3, 1))
    overshoot = np.maximum(np.asarray(out, float)-high, np.asarray(low, float)-out).clip(0)
    # Quantized 8-bit output: tolerate <=1 code value in the violation rate, report max too.
    return {"psnr_db": float(-10*np.log10(max(mse, 1e-12)/255**2)),
            "ssim_7x7": float(ssim[3:-3, 3:-3].mean()),
            "local_extrema_violation_pct": float(100*np.mean(overshoot > 1)),
            "overshoot_max_codes": float(overshoot.max()),
            "change_mae_codes": float(np.abs(o-s).mean()),
            "contrast_std_ratio": float(o.std()/max(s.std(), 1e-9)),
            "gradient_mae_to_reference": float((np.abs(np.diff(o, axis=0)-np.diff(r, axis=0)).mean()+
                                               np.abs(np.diff(o, axis=1)-np.diff(r, axis=1)).mean())/2)}


def corpus(refdir):
    cases = {}
    for name in ("astronaut", "chelsea", "grass", "brick", "gravel"):
        im = Image.open(refdir/f"{name}.png").convert("RGB")
        cases[name+"_soft"] = (im.filter(ImageFilter.GaussianBlur(0.7)), im)
        cases[name+"_native"] = (im, im)
        sharp = im.filter(ImageFilter.UnsharpMask(radius=1, percent=160, threshold=2))
        cases[name+"_presharp"] = (sharp, sharp)
    w, h = 512, 320
    y, x = np.mgrid[:h, :w]
    edge = np.where(x < w//2, 65, 190).astype(np.uint8)
    edge = Image.fromarray(edge).convert("RGB")
    cases["hard_edge"] = (edge, edge)
    cases["soft_edge"] = (edge.filter(ImageFilter.GaussianBlur(1.1)), edge)
    text = Image.new("RGB", (w, h), (45, 45, 45))
    d = ImageDraw.Draw(text)
    for i in range(15):
        d.text((8, 8+i*20), "KEDAVRA 30 > 90 FPS | Thin text 0123456789", fill=(210, 210, 210))
    for i in range(30):
        d.line((340+i*5, 0, 360+i*5, h), fill=(180, 190, 200), width=1)
    cases["text_lines"] = (text.filter(ImageFilter.GaussianBlur(0.5)), text)
    cloth = np.clip(120+36*np.sin(x*1.2)*np.sin(y*0.8)+20*np.sin((x+y)*0.05),0,255).astype(np.uint8)
    cloth = Image.fromarray(cloth).convert("RGB")
    cases["cloth_procedural"] = (cloth.filter(ImageFilter.GaussianBlur(0.5)), cloth)
    metal = np.clip(30+210*x/w+8*np.sin(y*1.3)+12*np.cos((x+y)*0.1),0,255).astype(np.uint8)
    metal = Image.fromarray(metal).convert("RGB")
    cases["metal_procedural"] = (metal.filter(ImageFilter.GaussianBlur(0.6)), metal)
    rng = np.random.default_rng(20261109)
    for sigma in (1, 3, 8, 16):
        noise = np.clip(np.rint(128+rng.normal(0, sigma, (h, w))),0,255).astype(np.uint8)
        im = Image.fromarray(noise).convert("RGB")
        cases[f"noise_{sigma}"] = (im, im)
    checker = Image.fromarray(np.uint8(80+100*((x+y)%2))).convert("RGB")
    cases["nyquist"] = (checker, checker)
    color = Image.new("RGB", (w, h), (210, 65, 30))
    ImageDraw.Draw(color).rectangle((w//2, 0, w, h), fill=(30, 170, 210))
    cases["color_edge"] = (color.filter(ImageFilter.GaussianBlur(0.8)), color)
    # Reproducible subpixel pans; these are analytical moving inputs, NOT generated frames.
    base = cases["astronaut_soft"][0]
    truth = cases["astronaut_soft"][1]
    for i in range(24):
        a = base.transform(base.size, Image.Transform.AFFINE, (1,0,i/3,0,1,i/9), Image.Resampling.BICUBIC)
        b = truth.transform(truth.size, Image.Transform.AFFINE, (1,0,i/3,0,1,i/9), Image.Resampling.BICUBIC)
        cases[f"pan_{i:03}"] = (a, b)
    return cases


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--exe", type=Path, required=True)
    p.add_argument("--spv", type=Path, required=True)
    p.add_argument("--references", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--fg", type=Path, help="actual kfg_vktest --out / KFG_HARD_OUT frames")
    p.add_argument("--bench", type=int, default=0, help="additional 1280x800 timestamp repetitions per mode")
    args = p.parse_args()
    root = args.out.resolve(); root.mkdir(parents=True, exist_ok=True)
    cases = corpus(args.references)
    if args.fg:
        for f in sorted(args.fg.glob("*_gen.ppm")):
            truth = f.with_name(f.name.replace("_gen", "_truth"))
            if truth.exists():
                cases["fg_"+f.stem] = (Image.open(f).convert("RGB"), Image.open(truth).convert("RGB"))
        for f in sorted(args.fg.glob("*_source_*.ppm")):
            im = Image.open(f).convert("RGB")
            cases["fg_"+f.stem] = (im, im)
    manifest, arrays, provenance = [], {}, {}
    for name, (src, ref) in cases.items():
        folder = root/name; folder.mkdir(exist_ok=True)
        src.save(folder/"input.png"); ref.save(folder/"reference.png")
        rgba = np.asarray(src.convert("RGBA")); rgba.tofile(folder/"input.rgba")
        provenance[name] = hashlib.sha256(rgba.tobytes()).hexdigest()
        arrays[name] = (np.asarray(src), np.asarray(ref))
        for mode, spec in MODES.items():
            manifest.append(f"{src.width} {src.height} {spec} {folder}/input.rgba {folder}/{mode}.rgba 1")
    if args.bench:
        folder = root/"bench"; folder.mkdir(exist_ok=True)
        bench = cases["astronaut_soft"][0].resize((1280,800), Image.Resampling.BICUBIC).convert("RGBA")
        np.asarray(bench).tofile(folder/"input.rgba")
        # Interleave modes per round, to reduce systematic thermal/order bias.
        for k in range(args.bench):
            modes = list(MODES); modes = modes[k%len(modes):]+modes[:k%len(modes)]
            for mode in modes:
                manifest.append(f"1280 800 {MODES[mode]} {folder}/input.rgba {folder}/{mode}.rgba 1")
    m = root/"batch.txt"; m.write_text("\n".join(manifest)+"\n")
    with (root/"vulkan.log").open("w") as log:
        subprocess.run([str(args.exe.resolve()), "--spv", str(args.spv.resolve()), "--batch", str(m)], stdout=log, stderr=subprocess.STDOUT, check=True)
    results, timings = {}, {}
    for line in (root/"vulkan.log").read_text().splitlines():
        if line.startswith("device:"): device = line.removeprefix("device: ")
        if line.startswith("SAMPLE\t"):
            _, path, spec, _, ms = line.split("\t")
            if "/bench/" in path: timings.setdefault(spec, []).append(float(ms))
    preview = ["<html><meta charset='utf-8'><title>Kedavra Detail comparison</title>",
               "<style>body{background:#181818;color:#eee;font:16px sans-serif} img{width:256px} td{vertical-align:top}</style>",
               "<h1>Kedavra Detail: research comparison</h1><p>Same inputs, actual Vulkan output. Native pixel PNGs linked. No Steam Deck validation.</p>"]
    outputs = {}
    for name, (src, ref) in arrays.items():
        folder = root/name; results[name] = {}; outputs[name] = {}
        show = not name.startswith(("fg_", "pan_"))
        if show: preview.append(f"<h2>{html.escape(name)}</h2><table><tr>")
        for mode in ["input", "reference", *MODES]:
            if mode in MODES:
                rgba = np.fromfile(folder/f"{mode}.rgba", np.uint8).reshape((*src.shape[:2], 4))
                assert np.all(rgba[..., 3] == 255), (name, mode, "alpha changed")
                out = rgba[..., :3]; outputs[name][mode] = out
                results[name][mode] = metrics(src, out, ref)
                Image.fromarray(out).save(folder/f"{mode}.png")
            if show: preview.append(f"<td>{mode}<br><a href='{name}/{mode}.png'><img src='{name}/{mode}.png'></a></td>")
        assert np.array_equal(src, outputs[name]["off"]), (name, "off is not identity")
        if show: preview.append("</tr></table>")
    # Temporal modulation of the filter correction. It isolates added changes, but is
    # NOT a motion-compensated flicker score or a human perception verdict.
    temporal = {}
    groups = {"analytic_photo_pan": sorted(n for n in outputs if n.startswith("pan_"))}
    for name in outputs:
        if re.search(r"_x[23]_f\d+_t\d+_gen$", name):
            group = re.sub(r"_f\d+_t\d+_gen$", "", name)
            groups.setdefault(group, []).append(name)
    for group, names in groups.items():
        names = sorted(names)
        if len(names) < 2: continue
        temporal[group] = {}
        for mode in MODES:
            corrections = [luma(outputs[n][mode])-luma(arrays[n][0]) for n in names]
            diffs = [float(np.sqrt(np.mean((b-a)**2))) for a,b in zip(corrections, corrections[1:])]
            temporal[group][mode] = {"correction_delta_rms_codes": float(np.mean(diffs)), "worst": max(diffs)}
            errors = [luma(outputs[n][mode])-luma(arrays[n][1]) for n in names]
            ediffs = [float(np.sqrt(np.mean((b-a)**2))) for a,b in zip(errors, errors[1:])]
            temporal[group][mode]["reference_error_delta_rms_codes"] = float(np.mean(ediffs))
    summary = {"device": device, "steam_deck_validated": False,
               "shader_sha256": hashlib.sha256((args.spv/"kfg_post.spv").read_bytes()).hexdigest(),
               "timestamp_scope": "Vulkan dispatch timestamps; upload/readback excluded; software drivers are not GPU hardware measurements",
               "input_sha256": provenance, "modes": MODES, "cases": results, "temporal": temporal,
               "bench_ms": {s: {"samples": t, "mean": float(np.mean(t)), "median": float(np.median(t)),
                                   "p95": float(np.percentile(t,95)), "p99": float(np.percentile(t,99))} for s,t in timings.items()}}
    (root/"results.json").write_text(json.dumps(summary, indent=2)+"\n")
    (root/"index.html").write_text("\n".join(preview)+"</html>")
    # 24-frame analytical pan: left input, centre CAS, right Detail. All pixels retained.
    movie = root/"video"; movie.mkdir(exist_ok=True)
    for i in range(24):
        n = f"pan_{i:03}"; a = outputs[n]
        frame = Image.fromarray(np.concatenate([a["off"], a["cas"], a["detail"]], axis=1))
        frame.save(movie/f"{i:03}.png")
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-framerate", "30", "-i", str(movie/"%03d.png"),
                    "-c:v", "libx264", "-crf", "12", "-pix_fmt", "yuv420p", str(root/"pan-comparison.mp4")], check=True)
    for group, names in groups.items():
        if not group.startswith("fg_") or len(names) < 10: continue
        path = movie/group; path.mkdir(exist_ok=True)
        for i,n in enumerate(sorted(names)):
            a = outputs[n]
            frame = Image.fromarray(np.concatenate([a["off"],a["cas"],a["detail"]], axis=1))
            ImageDraw.Draw(frame).text((5,5), n+" | off / CAS / Detail", fill="white", stroke_width=1, stroke_fill="black")
            frame.save(path/f"{i:03}.png")
        subprocess.run(["ffmpeg","-v","error","-y","-framerate","30","-i",str(path/"%03d.png"),
                        "-c:v","libx264","-crf","12","-pix_fmt","yuv420p",str(root/(group+".mp4"))],check=True)
    print(root/"results.json")


if __name__ == "__main__":
    main()
