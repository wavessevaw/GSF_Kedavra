# Focus bounded Edge candidate — 1.5.1-detail.4

This iteration replaces the installed Detail algorithm with a practical CAS-derived
balanced response. It is opt-in, experimental and SDR-only. Stable stays at 1.5.0.

## Engineering decision

The previous strict width gate erased useful texture. This implementation instead
uses CAS contrast-dependent per-channel negative-lobe correction, blends isotropic
and directional neighborhoods, smoothly attenuates high-contrast sharp steps and
local slope reversals, and bounds both total correction and excursions. Constant
channels do not veto other channels. Nine samples, one existing STAGE_SHARPEN pass,
no new full-size images, history, readbacks or upscaler dependencies.

Conservative, balanced and strong differ only in internal response scaling
(0.7 / 1.0 / 1.3). Balanced is installed; there are no additional UI modes.
The practical goal is useful detail at lower artifact intensity than CAS, not a
zero-halo metric. At 50%, the existing ringing control has halo area 143 / 146 / 151
for conservative / balanced / strong, versus 150 for CAS and both old Details.
At 100%, balanced reaches 152 versus .3's 159. This is a procedural negative control,
not an OptiScaler capture. Sharp black/white steps remain exactly unchanged.

At 50%, softened grass reference PSNR rises from .3's 27.822 to balanced 29.377 dB
(CAS 31.540); portrait rises from 35.633 to 35.962 dB (CAS 36.876). These accompany
lossless visual comparisons, not a claim that PSNR proves perceptual quality.
The selected response restores more texture than conservative while retaining lower
halo area than strong in the negative control. The maximum local excursion in those
photo cases is 6 codes, versus CAS's 33–37. Fine texture remains less aggressive than
CAS; the old colour-edge-specific .3 result can still be better on that single input.

Noise trade-off: the sigma-3 grayscale control has standard-deviation ratio 1.344
versus old Detail 1.001 and CAS 2.534. This is a deliberate, documented loss of the
old near-identity noise behavior. The filter does not denoise or infer missing detail.
Existing ringing is attenuated, not guaranteed eliminated. Strength zero is exact
bypass; default remains off. CAS/DLS and the Setup controls remain available.

## Evidence and validation

CI reproduces the .2 and .3 shaders from frozen commits, runs 170 identical inputs,
compares all three configurations, records per-frame metrics and temporal error /
correction deltas, and packages lossless PNGs plus seven review videos. The videos
show the same real and generated frames; 30 fps review playback is not a pacing test.
Software Vulkan timestamps (100 samples at 1280x800, mean/P95/P99) accompany the lab
artifact. The complete installer includes a standalone RADV benchmark for Deck.

The previous zero-excursion unit assertion was explicitly updated to the revised
bounded-excursion contract (7 quantized codes including rounding). Exact zero bypass,
alpha preservation, constant-channel and hard-step assertions remain unchanged.
No optical flow, warp, scheduler or frame-pacing implementation was changed.

No Steam Deck / physical GPU or authentic FSR, XeSS, DLSS, TSR or OptiScaler captures
are available. Their compatibility, GPU budget, compositor P95/P99, latency and
perceptual motion acceptance remain unverified. Do not interpret software timestamps
as Deck measurements or this spatial filter as FSR 4 reconstruction.

Install and roll back using README.md; install the matching runtime and reboot.
The PR remains a draft. No Stable publication or merge is authorized by these tests.

Temporal trade-off at 50% (reference-error delta RMS in encoded luma codes): fast
turn x3 is 20.394 versus .3 20.012 and CAS 21.160; moving lines x3 is 11.168 versus
11.087 and 12.530; parallax x3 is 42.783 versus 42.609 and 43.463. The photographic
pan improves from 2.210 to 2.097 (CAS 1.913). These are mixed outcomes: extra detail
also exposes some FG errors. They do not prove absence of visible flicker. Balanced
keeps these deviations below strong; device motion review is still required.
