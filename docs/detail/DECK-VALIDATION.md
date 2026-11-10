# Steam Deck acceptance procedure

Hardware validation has not been performed. Record LCD/OLED model, SteamOS version,
kernel, Mesa/RADV, power limit, clocks, display refresh, game build and exact candidate
commit. Run on both devices where available; keep temperature and workload comparable.

## Isolated filter cost

The install candidate includes a standalone benchmark under the plugin's `bench/`.
It needs Python 3 standard library and Vulkan, no numpy/Pillow/scipy. From that folder:

```sh
python3 bench_detail.py --out "$PWD/results-idle" --samples 200 --size 1280x800
```

It compares off, CAS, DLS, research RCAS, Detail 0.5 and Detail 1 with identical input,
five excluded warm-ups per sample and deterministic shuffled order. Save the raw
timestamps and JSON containing mean/P50/P95/P99. The research shader is separate
from Gamescope and cannot enable RCAS in production. Confirm the reported device
is AMD RADV, not llvmpipe/lavapipe. Record device identity separately; the script
does not infer that an arbitrary AMD device is a Steam Deck.

Repeat while running the same deterministic game replay. Target: Detail ≤0.3 ms at
1280×800 and preferably ≤1.10× CAS. If CAS itself exceeds the budget, report actual
values. The off case emits no dispatch and is reported as zero, not an empty GPU pass.

## Full compositor and pacing

Use identical saved replay/camera paths. Alternate off/CAS/DLS/Detail, at least three
60-second captures each, after warming the game. Test FG off, ×2, ×3; source 30 fps
and sub-30 unstable source cadence; 60 Hz LCD and 90 Hz OLED. Measure GPU post and
analysis cost separately. Existing KEDAVRA status is sampled once per second and
its moving average **cannot** substitute for per-frame compositor P95/P99.
Use Gamescope GPU timestamp capture/profiling on the device for that requirement.

Use the existing panel's pacing capture or `kfg_trace` and the included
`kfg_pacing.py` to retain actual DRM page-flip intervals, P95/P99, missed slots,
reordering and available latency. Do not relabel model/simulated traces as hardware.
Compare against the same 1.5.0 workload; Detail's per-display-frame cost may exceed
the old source-prefilter total even when each dispatch meets the CAS budget.

## Visual matrix

Capture lossless real, first intermediate, second intermediate and next real frame.
Inspect full-speed video and matching frame crops for faces, hair/fur, fabric,
vegetation, stone/metal, thin text, baked HUD, cursor and subtitles. Include fast
turns, noisy scenes, already sharpened upscaler output and known FG block/ghost errors.
Compare equal perceived sharpness as well as equal numerical strength; the controls
are not calibrated to equal strength across algorithms.

Check HDR10/PQ: Detail-only must bypass, brightness unchanged; legacy effects retain
the baseline policy. Check scRGB with negative and >1 values: no new clipping or
filtering. Test SDR↔HDR, resize/aspect changes, repeated frames, live strength updates,
Detail↔CAS/DLS, FG mode changes and focus changes. Test standalone Detail with native
rendering, TAA, FSR and OptiScaler; keep off when the source is already sufficiently sharp.

## Release gate

Accept only if the target scenarios improve perceptually, new halos/geometry errors
are absent, noise and temporal artifacts are acceptable, and measured GPU/pacing/
latency meet the budget without significant regressions. Report failures, not only
averages. Keep the candidate opt-in and the PR draft until these checks are complete.
Stable additionally requires the user's agreement. The current artifact is not
evidence that installation, HDR display output or frame pacing have passed on Deck.
