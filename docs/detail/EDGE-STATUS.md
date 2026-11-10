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
HDR bypass is identified from Gamescope's own layer colour-space metadata, including
when HDR generation is disabled. Unsupported SDR formats are reported separately;
the legacy generic post-limited flag must not relabel them as HDR.

## Remaining research / acceptance gates

**Decision: reject all three new Edge variants.** Keep the existing opt-in Detail
algorithm with the colour bug fix. This candidate does not solve the full requested
anti-halo behavior and must not be marketed as Edge Enhancement or FSR 4 quality.

The negative control is a 65-to-190 analytical sigmoid (sigma 1.2 pixels), with
Pillow UnsharpMask radius 2 / 300% producing an existing ringing profile. This is
a reproducible proxy, **not a capture of OptiScaler Override Sharpening**.
At 50% processing, the integrated halo area (sum of deviations outside the two
known plateau levels, one scanline, code-value × pixels) was:

| Filter | Halo area | Soft-edge 20–80% width, pixels |
| --- | ---: | ---: |
| Off | 134 | 3.429 |
| CAS | 150 | 3.214 |
| DLS | 152 | 3.071 |
| Detail .2 | 150 | 3.154 |
| Detail colour fix | 150 | 3.154 |
| Experimental 3x3 profile gate | 140 | 3.154 |
| Experimental 11-sample monotonic profile | 146 | 3.154 |
| Experimental 11-sample strict width gate | 134 | 3.308 |

The colour fix preserves the old grayscale behavior, including this limitation:
**local extrema bounds do not guarantee that existing halo area will not grow**.
At 100%, old/fixed halo area increases to 159. Leave Focus off on visibly ringing
inputs. No universal protection against repeated sharpening is claimed.

The strict width gate avoids additional halo area in this control but removes
almost all useful correction on the softened portrait (33.675 dB input → 33.721 dB,
compared with 35.633 dB for the colour fix and 36.876 dB for CAS). It also leaves
the colour edge unchanged. The weaker profile gates retain some correction but
fail the halo criterion. These prototypes are not installed or selected by `detail=`.
All tested soft-edge 50% positions remained at 127.5 pixels; this single symmetric
control does not establish geometry preservation for arbitrary lines or angles.

Reproduction: `edge_variants.py` creates separate SPV directories; run
`compare_edge.py` for each against the same frozen .2 corpus, then
`probe_profiles.py` with each `--variant NAME=SPV_DIRECTORY`. `edge_evidence.py`
packages all input/reference/filter PNGs, metrics and labeled review videos.
The CI job records the complete commands. `edge-profiles.json` includes both
50% and 100%; `edge-experiments.json` contains selected natural/control comparisons.
The public Edge-Evidence ZIP includes all 170 frame comparisons. Its 30 fps videos
are repeated short review sequences, not recordings or measurements of frame pacing.

Software Vulkan tests cover the actual ×2/×3 generated images and both ×3 phases.
Compared with old Detail, the colour fix changes temporal reference-error RMS
from 20.0174 to 20.0123 codes in the fast-turn ×3 sequence and from 42.6073 to
42.6089 in the parallax ×3 sequence. These small changes are mixed, not proof of
eliminated flicker. Live Auto, unstable sub-30 fps input and 30→60/90 presentation
still require the device procedure in DECK-VALIDATION.md.

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

`edge-software-timestamps.json` records 40 randomized-order samples per mode at
1280x800 on llvmpipe, including mean/P95/P99 and every raw sample. The shared CPU
executor was also packaging evidence during the baseline run. Consequently these
runs are smoke evidence only: neither their absolute times nor an apparent speedup
establish GPU performance. The CI lab separately stores 100-sample timestamp data.
The installed `bench/bench_detail.py` supports repeatable RADV measurements on Deck;
full compositor and presentation measurements remain separate requirements.
