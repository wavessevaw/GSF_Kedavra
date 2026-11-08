# Reproduction and acceptance status

## Implemented

* Five-tap reconstruction is limited to its existing local sample bounds. This
  suppresses negative-lobe ringing without extra texture reads or GPU passes.
* A Vulkan regression test exercises horizontal/vertical grey steps at 1/3,
  1/2 and 2/3. On the original stable shader: 672 out-of-range pixels, maximum
  overshoot 9/255. On the candidate: zero (software Vulkan, 64x48).
* The standalone harness saves source A/B alongside generated and truth images.
* Per-pass benchmark JSON adds mean and P95 to existing median/P99.
* `kedavra/tools/compare_quality.py` compares matched images using RGB MAE,
  windowed SSIM and gradient error; mismatched ground truth is an error.

## Reproduce

Install a C compiler, Vulkan headers/loader, glslang-tools and a Vulkan driver.
Use Mesa lavapipe for software correctness or RADV on Steam Deck for GPU timing.

```
make -C kedavra all
mkdir -p results/candidate
kedavra/build/kfg_vktest --spv kedavra/build --out results/candidate --json results/candidate/engine.json
kedavra/build/kfg_vktest --spv kedavra/build --lowbase --json results/candidate/lowbase.json
make -C kedavra test-sched test-pacing
kedavra/build/kfg_posttest --spv kedavra/build
```

Build the original engine at `62621c4f9b60fd3cb95edafb62a1a44a1bb2c21e` in a
separate checkout. Save its outputs into `results/baseline` using the same size
and options. The ringing regression should fail when the updated harness is
run with original SPIR-V. This is intentional evidence of the defect.

```
python3 kedavra/tools/compare_quality.py results/baseline results/candidate --out comparison.json
```

For timing, run the **updated harness** against each shader directory, separately
(no simultaneous runs), at identical resolution and power settings:

```
kedavra/build/kfg_vktest --spv BASELINE_SPV --scene static --bench 200 --json baseline-timing.json
kedavra/build/kfg_vktest --spv kedavra/build --scene static --bench 200 --json candidate-timing.json
```

This benchmark records per-pass timestamps, not physical scanout or input
latency. The historical `analysis_ms` and `two_outputs_ms` fields sum per-pass
medians; they are not percentiles of total pair time. Use the individual pass
mean/P95/P99 fields when comparing the changed warp.

## Steam Deck acceptance still required

Record LCD/OLED, SteamOS/Mesa versions, resolution, refresh, TDP and clocks.
Use A/B/A runs after warm-up. Test x2 and both x3 phases at 60/90 Hz, then Auto,
x3.75 and fractional/custom modes. Save raw KMS pacing traces and analyse
mean/P95/P99, missed refreshes and repeats with the existing pacing tools.
Measure input-to-photon separately; do not infer it from shader timestamps.
Include native/Proton/emulator/remote-play inputs, HUD/subtitles, particles,
transparency, lighting transitions and irregular source cadence. Exercise HDR
and change of resolution/source. Retain source, baseline, candidate and truth
(where available), not just scores.

No Deck, scanout, power, physical latency or broad real-game validation is
claimed. Software measurements cannot establish acceptable Deck performance.

## Investigated and rejected / not implemented

The main-branch 1.4.4 replacement was rejected after the stable suite reported
15 failures (including changed scene-cut endpoint policy and HUD distortion).
Its own suite passed. The candidate retains the stable scheduler and engine
branch and applies only bounded reconstruction. The camera-border hypothesis
is not implemented. New motion algorithms, history accumulation, automatic
quality controller and mandatory game motion/depth inputs are not added.
This is a focused experimental candidate, not completion of the full quality
update specification and not a new stable release.
