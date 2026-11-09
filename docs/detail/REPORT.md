# Detail comparison report

Status: research candidate, hardware and visual acceptance pending. The implementation
is not enabled by default and has not been promoted to Stable. No Steam Deck was available.

## Scope and method

The baseline is the published 1.5.0 CAS/DLS implementation, without algorithm changes.
`tools/detail/compare.py` processes identical RGBA inputs through real Vulkan shaders:
off, CAS 0.5, DLS 0.5:0.17, adapted RCAS 0.5, Detail 0.5 and Detail 1. Strength values
are not perceptually calibrated. RCAS uses denoise and exact guarded FP32 division;
it is a comparison adaptation, not an unmodified AMD binary. It is excluded from
the installed runtime.

Photographs cover a face/hair/clothing, fur, grass, brick and gravel. Controlled
Gaussian blur provides known references; native and pre-sharpened variants expose
overprocessing. Procedural scenes cover text, lines, cloth and metal-like surfaces,
hard/soft/color edges, Nyquist patterns and seeded noise. Cloth/metal procedures are
not substitutes for captured game materials. Actual unmodified Kedavra outputs
cover synthetic fast turns, parallax and thin lines, including real/×2/×3 phases.
The photographic pan is analytical resampling, not actual frame generation.

Outputs retain input/reference/off/CAS/DLS/RCAS/Detail PNGs, raw RGBA, manifests,
hashes, videos, and dispatch timestamp samples. Metrics include PSNR, local-window
SSIM, reference gradient error, local extrema violations, noise/contrast standard
deviation, and temporal residual/correction changes. Local extrema violations are
a halo-risk proxy; they do not prove every exceeding pixel is a visible artifact.
Temporal correction modulation is not motion-compensated perceptual flicker.
Neither increased gradients nor a PSNR improvement establishes subjective quality.

## Selection

The initial 1.5× gain prototype was rejected: although its bounded edges improved,
its analytical-pan correction modulation exceeded CAS. Its result is retained in
`rejected-gain-1.5.json`. The candidate uses 1.0× gain. Fine texture and the subjective
tradeoff against CAS remain reasons to preserve CAS/DLS and keep Detail opt-in.
No history, new flow fields, extra sharpening pass or upscaler was justified.

## Completion categories

**Implemented and software-tested:** one-pass SDR Detail, strict 0–1 parsing,
zero bypass, shared-channel bounds, alpha preservation, deferred display-frame
placement, HDR policy bypass, lab RCAS, deterministic reference downloads and
comparison/benchmark tools. See checked-in result JSON and CI for exact results.

**Implemented, requiring Steam Deck verification:** real/FG compositing under load,
per-display-frame overhead, installation/rollback, HDR passthrough in the display
pipeline, GPU ≤0.3 ms and relative cost target, compositor P95/P99, frame pacing,
latency and visually stable gameplay. Software Vulkan timings do not establish these.

**Investigated and rejected/deferred:** stronger 1.5× detail gain; replacing CAS/DLS;
production RCAS without demonstrated superiority; processing PQ/scRGB with SDR
thresholds; an additional history or upscaling pipeline without evidence.

**Not implemented:** ML/FSR4 reconstruction, semantic face detection, a new scaler,
HDR Detail enhancement, automatic external-upscaler detection, new UI presets,
automatic default enablement, Stable publication.

## Measured local software results

Device: llvmpipe (LLVM 19.1.7), Mesa 25.0.7. These are software Vulkan dispatches,
**not Steam Deck GPU benchmarks**. Source: `local-software.json`; the full lab artifact
contains 170 inputs (120 actual engine real/generated outputs, 24 analytical pan
frames, 26 static images) × six variants. Both intermediate ×3 phases and ×2 are saved.

All table sharpness controls are 0.5, with DLS denoise 0.17. Equal numeric settings
do not imply equal perceived strength.

| Controlled scenario | Off | CAS | DLS | RCAS adaptation | Detail |
| --- | ---: | ---: | ---: | ---: | ---: |
| Soft step, PSNR to sharp reference (dB) | 39.91 | 40.61 | 40.78 | 40.21 | **41.00** |
| Hard step, maximum local overshoot (8-bit codes) | 0 | 18 | 12 | 6 | **0** |
| Noise σ=3 codes, output/input standard deviation | 1.000 | 2.534 | 2.727 | 1.372 | **1.001** |
| Noise σ=8 codes, output/input standard deviation | 1.000 | 2.304 | 2.295 | 1.366 | **1.031** |
| Blurred portrait, PSNR (dB) | 33.68 | **36.88** | 36.41 | 34.85 | 35.49 |
| Blurred fur, PSNR (dB) | 37.55 | **42.44** | 41.79 | 39.67 | 38.32 |
| Blurred grass, PSNR (dB) | 26.55 | **31.54** | 30.76 | 28.36 | 27.82 |

Detail improves the controlled soft edge and suppresses noise/halo amplification
relative to the legacy filters. It is weaker at recovering fine texture. At strength
0.5 the analytical-pan correction modulation is 1.027 codes RMS (CAS 1.098, DLS
1.365, RCAS 0.502); error-difference to the moving reference is 2.220 (off 2.519,
CAS 1.913). Thus lower correction modulation is not evidence of superior reconstruction.

On actual ×3 fast-turn synthetic output, temporal reference-error delta is 20.017
codes RMS for Detail versus 19.797 off and 21.160 CAS. On ×3 thin lines: 11.094
versus 10.950 off and 12.530 CAS. **Some residual temporal error increases versus
off**, so “no flicker increase” is not established. Maximum strength should not be
assumed acceptable. These diagnostic scenes are 320×200; they are not game captures.

| Isolated pass, 1280×800, 100 samples | Mean ms | Median ms | P95 ms | P99 ms |
| --- | ---: | ---: | ---: | ---: |
| CAS 0.5 | 17.418 | 12.768 | 37.786 | 41.279 |
| DLS 0.5:0.17 | 15.661 | 12.644 | 35.935 | 37.442 |
| RCAS 0.5 | 11.775 | 9.413 | 28.466 | 32.639 |
| Detail 0.5 | 15.898 | 12.614 | 37.305 | 41.516 |

This shared CPU/software-driver run cannot validate the ≤0.3 ms Deck budget or a
≤10% hardware cost increase. Upload/readback are excluded by dispatch timestamps;
off dispatches nothing. The full compositor additionally changes filter cadence
from source rate to display rate when Detail is selected. GPU, compositor P95/P99
and latency acceptance remain unverified.

Decision: retain as an explicitly opt-in research candidate for hardware/visual
review. Do not replace CAS/DLS, enable by default, or publish Stable. The current
evidence demonstrates bounded edges and noise control, not universal superiority
or satisfaction of every acceptance criterion.
