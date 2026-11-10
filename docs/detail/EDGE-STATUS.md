# Edge Enhancement investigation — candidate 1.5.1-detail.3

Base candidate: v1.5.1-detail.2; stable comparator: v1.5.0. Focus remains opt-in.
This candidate fixes a confirmed colour-channel suppression defect and the Setup
slider layout. It is **not acceptance of the full Edge Enhancement specification**.

## Proven cause and correction

The old shared additive luminance delta was clipped to the intersection of all
three channel ranges. A constant blue channel, including zero blue, gave a zero
range and cancelled red/green sharpening. Increasing strength could not fix this.
The correction now follows the RGB directional residual with a shared limiting
scale. Channels with zero correction do not veto other channels. Local extrema
bounds, noise/coherence gate, nine samples, one pass, zero bypass and HDR bypass
remain. No coefficient was increased. CAS/DLS and all FG shaders/scheduling remain.

A reproducible constant-blue soft-edge regression now changes red/green while
keeping blue exactly zero. On 170 identical prior-lab inputs, all new outputs stayed
inside local channel bounds. At 50%, the colour edge reference PSNR improved from
41.126 to 45.048 dB; the old CAS value was 42.70 dB. Portrait PSNR changed from
35.489 to 35.633 dB. Grayscale grass and noise results were unchanged. Some colour
FG frames have worse reference error: worst old/new difference -0.599 dB on a real
fast-turn frame. These are diagnostic results, not perceptual acceptance.

`edge-channel-results.json` retains per-frame old/new metrics. To include the full
Off/CAS/DLS/old Detail/new comparison, run `tools/detail/compare_edge.py --baseline
PATH_TO_DETAIL_2_COMPARISON --exe build/kfg_posttest --spv build --out RESULTS`.
The baseline must be the saved .2 comparison inputs/outputs, not regenerated with
new shaders. Source/reference PNGs are retained in that artifact. The comparison
writes identical-input outputs and selected comparison strips.

## UI and application evidence

The slider's card has explicit width/min-width bounds and the track reserves
18 pixels at each end for its handle. Values are finite and limited to 0–100%.
Focus status now comes from compositor recording, with a dedicated frame counter:
processing, waiting for game, Steam UI, unsupported source, HDR bypass or allocation
failure. Accepted configuration alone is no longer described as proof of processing.
A submitted-pass counter is not a GPU completion or image-quality measurement.

## Remaining research / acceptance gates

The existing contrast smoothstep still suppresses very low-contrast detail; it has
not been replaced by an unvalidated global gain increase. The 3x3 orientation and
coherence model is retained. A dedicated transition-width estimator, explicit
existing-ringing classification and proven universal anti-resharpening are not yet
implemented. Spatial data cannot identify the producer of an image.

No Steam Deck or captured FSR4/FSR2/FSR3/XeSS/DLSS/TSR/OptiScaler sequences are
available here. All of those **game-specific compatibility checks remain untested**.
Procedural/natural images must not be described as captures from those technologies.
No external configuration, DLL, process or API is accessed or modified. Actual
Override Sharpening negative controls remain unavailable. HDR enhancement remains
bypassed. Hardware GPU <=0.3 ms, <=110% CAS, compositor P95/P99, pacing/latency and
perceptual motion acceptance are unverified. Existing software timing is not Deck
hardware timing. No Stable promotion is justified by this candidate.
