# GFG 4.0 research — not a release

## Pinned reference

GFG release 3.4.4: `4141b45b4152e77932eaf288a75ad7e65aa8ae3a`.
Kedavra release 1.6.2: `b063186c0218ec457a15433055df1c6268e25922`.
The motion shader used for this experiment was compared byte-for-byte against
that published Kedavra commit before modification and matched.

## Competitor evidence

MAKO renderer tag `render-v5.0.0`, model card V1.24 (2026-11-12):
https://github.com/eugeniosegala/MAKO/blob/render-v5.0.0/engine/docs/mako-fg/MODEL_CARD.md

The card describes coarse-to-fine motion, 211 learned blending parameters,
a separate HUD stage and thin-object treatment at 4x+. Motion-period modes
reuse/advect motion between solves. The integration source `mako_fg.cpp`
uses a separate host process, shared Vulkan resources, socket requests and
failure recovery. Runtime/model implementation source is not published.
Weights/shaders are CC BY-NC 4.0; runtime has separate noncommercial terms.
No competitor weights or runtime were imported into Kedavra.
The author's timing evidence is NAVI33, not Steam Deck, and is not our measurement.
No direct MAKO quality or performance comparison has been run.

## Existing Kedavra architecture

Four luma pyramid levels, bidirectional coarse-to-fine matching, temporal and
camera predictors, scene statistics, shared pair selection and two repairs,
then one full-resolution warp per generated phase. Selection is already reused
across phases; static and near-perfect predictors already skip refinement.
Therefore shared pair work and static skipping are not new proposals.

## Hypotheses

1. Bound motion-candidate SAD evaluation by the incumbent score. Implemented
   prototype, no additional buffers or passes. Nonnegative partial sums allow
   rejecting a losing candidate; a 1e-6 margin retains near ties. Same candidate
   ordering, windows and penalties. GPU divergence may erase savings.
2. Sub-block boundary reconstruction: investigate selection ambiguity before
   increasing spatial resolution; not implemented or claimed yet.
3. Joint workload/admission control: avoid GPU work whose outputs cannot meet
   presentation deadlines. Audit existing scheduler before any implementation.
   Not implemented or claimed yet.

## Evidence so far

On lavapipe, 174 saved 320x200 images matched byte-for-byte between baseline and
prototype. BOTH runs reported 30 failures against the existing fixed quality
thresholds at that small size: this is an equivalence experiment, NOT a passing
quality suite. At 1280x800 the baseline pan scene passed all three phases.
Full-size prototype and software timings are separate artifacts.
No real-game captures, Deck GPU timings, power savings, latency improvement or
competitive superiority have been established. Five timing repetitions are
exploratory, not reliable tail-latency estimates.

This is not sufficient to select the final 4.0 architecture or publish a build.

1280x800 pan passed for both variants. Five-repetition lavapipe exploratory analysis
sum-of-pass-medians: baseline 36.580 ms, pruned 37.950 ms. These are NOT hardware
measurements or statistically supported speedups. Candidate is not selected;
shader is kept only under research/prototypes, outside the active engine.
