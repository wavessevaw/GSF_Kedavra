# Quality update audit — 2026-11-08

## Provenance and release distinction

GFG stable v3.2.2: `92b557f678d118ae97948673c26e2498b7a28467`.
Its release workflow pins Kedavra `62621c4f9b60fd3cb95edafb62a1a44a1bb2c21e`.
GFG main observed: `500b19b` (3.3.x development).
Kedavra main observed: `2afabbcb806b6244eeac9c743bff08232cbf354d`
(1.4.4 code at `f13e66c`, then documentation). GitHub latest published stable
release points to v1.4.0, `6176caf141f2c908cea12ac7f22609cbd21eed33`.
These are distinct baselines. The 3.2.2 pin has changes on a divergent branch;
replacing it with main is not a simple forward upgrade.

## Architecture verified in source

GFG's Python backend controls Gamescope through its Kedavra control adapter;
Decky renders the TypeScript UI. The release workflow pins and builds the engine,
Gamescope patch and standalone benchmark, then bundles a runtime into the Decky ZIP.
The engine is not a runtime Python dependency or an injected game Vulkan layer.

`kedavra/engine/kfg_plan.h` is the common pass definition for the standalone
Vulkan harness and `kedavra/gamescope/src/kfg/kfg.cpp` host:

* Four source/pair ring slots, four R8 luma pyramid levels (half to 1/16 size).
* Bidirectional RGBA16F motion at levels 4, 3 and 2; coarse-to-fine refinement.
* Coarse and fine global camera fits; seven RGBA16F statistics texels in main
  versus four in the stable pin. Scene cut classification follows motion analysis.
* Selection at 4x4 block resolution, two spatial repair passes; pair selection
  is reused for all intermediate phases. Confidence/residual controls source weights.
* Main: 13 analysis passes per new pair (4 reduce, 3 ME, 2 global, 1 stats,
  1 select, 2 repair), one full-resolution warp per generated output.
* Warp: motion-edge candidate selection, projective camera path on main,
  occlusion/source weights, boundary visibility, five-tap sharp reconstruction,
  optional fast-motion blur. Colour stays in its original encoding.
* Temporal reuse seeds motion search. It is not recursive interpolation of
  synthetic images. Filters and Gamescope overlays follow generation.
* Analysis resolution is capped at 1536x960; final warp uses source resolution.

Scheduler chooses real/generated/repeat at vblank. Completion tracking protects
image lifetime. GPU timestamp budget, overload handling, history reset and Auto
already exist. New quality adaptation and additional history buffers would
need evidence beyond the current audit; none are added here.

## Preserved GFG baseline surface

Off/Auto/x2/x3/x3.75/custom (fractional up to x4), SDR/HDR, profiles, migration,
Governor/Autopilot, TDP and CPU/GPU controls, battery/dock/menu handling, post
filters, HUD, telemetry, pacing capture, installation and automatic recovery.
The development branch starts at v3.2.2, not at GFG main. No UI or settings
migration changes are needed for a shader-only candidate.

## Findings and experiment scope

Existing mechanisms already address coarse-to-fine motion, bidirectional
consistency, occlusions, motion edges, camera models and temporal seeds. Adding
parallel implementations is unjustified. The latest branch also drops parts of
the stable pin's hard-scene harness: the stable suite must remain a regression
oracle, with its statistics allocation adapted when running the new engine.

Sharp reconstruction uses negative filter lobes. They can invent values beyond
local colour extrema (halos), including on grey edges where UNORM saturation
cannot hide them. Candidate bounds the result by its five existing samples;
no extra image, texture fetch or dispatch. ALU/register cost still needs timing.

Camera photometric evidence may include clamped out-of-frame samples. This is
an audit observation, not a confirmed fix or accepted quality improvement.

## Validation limits

This executor has no /dev/dri. Vulkan uses Mesa 25.0.7 llvmpipe/lavapipe,
LLVM 19.1.7. Software timestamp values must never be labelled Steam Deck GPU
cost, scanout pacing or end-to-end latency. No native games, Proton, emulators,
remote-play clients, LCD/OLED scanout or Deck power measurements ran here.

## Selection decision

The 1.4.4 candidate fails 15 checks from the stable 3.2.2 harness, including
HUD distortion beside thin lines and scene-cut endpoint semantics. Its own
suite passes. Wholesale integration is rejected; the selected experiment is
a local reconstruction fix on the exact 3.2.2 engine pin. This preserves the
existing scheduler and motion algorithms. No claim of complete acceptance.
