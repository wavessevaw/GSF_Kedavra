# Silk second quality experiment cycle

Base: engine 62621c4f9b60fd3cb95edafb62a1a44a1bb2c21e shipped in GFG 3.2.2,
descended from Silk 1.4.0. Cancelled 1.4.1–1.4.4 branches are not used.
The previous bounded-filter experiment is not enabled in these tests.

Build with `make -C kedavra all` (C compiler, Vulkan development files and
glslangValidator required). Select lavapipe for software tests or RADV on Deck.
Create a separate shader directory for each variant, copying `kedavra/build/*.spv`.
Compile the experimental `.comp` over only its corresponding SPIR-V file:

```
glslangValidator -V --target-env vulkan1.1 -Ikedavra/shaders \
  -o VARIANT/kfg_warp.spv docs/quality-round2/experiments/adaptive/kfg_warp.comp
kedavra/build/kfg_vktest --spv VARIANT --hard --scene h_turn64 \
  --source-fps 24 --json turn24.json
kedavra/build/kfg_vktest --spv VARIANT --hard --scene h_turn64 \
  --source-fps 20 --json turn20.json
```

Use `kfg_select.spv` for variants changing selection. The no-blur experiment
compiles the original warp with `-DKFG_EXPERIMENT_NO_BLUR=1`. Per-phase selection
uses original shaders and the harness option `--per-phase`.

`--source-fps` scales analytic displacement relative to the renderer's 30 FPS
reference. It does not simulate hardware display timing or measure actual FPS.
The earlier `--lowbase` runs scale displacement by 1.25 (24 FPS equivalent).
Hard tests shown here generate both x3 intermediate phases. General suite uses
both x3 phases plus x2 and tests temporal sequences, HUD, SDR/PQ/scRGB and cuts.

The original hard suite reports diagnostic metrics rather than acceptance
assertions. Its exit status alone must not be used as evidence of improvement.
Compare every relevant metric with baseline and investigate regressions.

Predeclared block-artifact selection criterion: at least 20% reduction in
`badblk_pct`, no material temporal/HUD regression. This is a synthetic proxy,
not a claim that the same fraction of a real game's pixels contains artifacts.
A general sharpness improvement must also avoid material losses in other scenes.
Runtime changes remain outside production shader paths until accepted.
