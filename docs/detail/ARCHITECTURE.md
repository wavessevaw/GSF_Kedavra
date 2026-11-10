# Detail architecture and baseline audit

Base: tag `v1.5.0`, commit `00bd846a9a92629fd85e2cce3276d9a653ed90ce`.
Cancelled 1.4.1–1.4.4 branches and the unrelated current main are not the base.
The candidate leaves motion estimation, warp math and frame scheduling unchanged.

## Actual 1.5.0 paths

`kfg_post.h` parses comma-separated effects and plans grade → sharpen → AA → finish.
Two existing post images alternate; sharpen is one dispatch. CAS and DLS read 3×3
neighborhoods in encoded SDR. CAS has per-channel contrast adaptation and global
[0,1] clipping, but no local extrema limiter or explicit noise gate. DLS applies a
luminance correction with contrast/brightness/denoise weights and global clipping.
The legacy sharpness value zero is not necessarily a bypass; Detail deliberately
defines its own zero as bypass without changing those legacy semantics.

Despite outdated comments, `RecordGenerate` in 1.5.0 calls `RecordPost` **before**
storing source frames in its ring. Motion estimation and synthetic reconstruction
therefore see legacy filtered inputs. This saves passes at ×2/×3, but a generated
frame's sharpness may differ from the input because warping resamples it. With FG
off, post operates directly on the displayed layer. Steam HUD/overlays in separate
layers remain outside the game image filter; HUD baked into the game texture does not.

`kfg_warp.comp` reconstructs from optical flow and samples original ring textures.
Subpixel sampling and ambiguity fallback can soften detail. Its 1.5.0 selective
fast-motion blur is preserved byte-for-byte. Sharpening cannot recover occluded
information or repair incorrect motion geometry. Source blur/noise and interpolation
blur are distinct; the comparison suite contains both controlled photographic blur
and actual unmodified engine outputs with known synthetic references.

`kfg_vk.c` is the standalone Vulkan test driver. Its upload/download and wait calls
are test I/O, not the installed Gamescope pipeline. Gamescope uses `RecordPlan` with
its own descriptor/resources and command submission. HDR uses float ring/output
images; the existing transfer policy excludes SDR grades from PQ and bypasses all
post effects for scRGB. The stale gamescope README's SDR-only claim is incomplete.

## Candidate

The shader computes three horizontal and three vertical differences from a 3×3
neighborhood. Weighted sums estimate orientation and gradient agreement. A
directional second difference supplies detail. Gain is strength × agreement² ×
a smooth contrast gate (0.025–0.10 encoded SDR luminance). A quarter-span bound and
intersection of per-channel local extrema constrain a **shared** RGB correction.
This avoids independent channel clipping and keeps flat or already hard boundaries
stable. It is a heuristic, not semantic discrimination between texture and noise.

One center RGBA fetch (also supplies unchanged alpha) and eight neighbors; one
dispatch, existing full-resolution images, no extra fields or temporal storage.
The input texture geometry and output resolution do not change. Border sampling
uses the existing clamp-to-edge sampler. No new synchronization is introduced.

`detail=S` replaces the selected sharpener. Its whole post chain is deferred until
after real/generated frame selection. Unfiltered ring/output references are retained
for repeats; the already filtered post target is never sharpened recursively.
Switching placement resets source history to avoid old prefiltered frames leaking
into the new path. Updating intensity within Detail does not reset history.
Legacy CAS/DLS-only configurations retain their original placement and formulas.

The existing PQ/scRGB policy is factored into a pure, tested helper. Detail is
explicitly bypassed on PQ and scRGB until a suitable transfer-aware implementation
is validated. HDR limitation status includes the skipped sharpener, not just grades.

## Cost and open limits

An isolated Detail pass is comparable in sample count to CAS. With FG on, Detail
runs at displayed cadence while legacy CAS is usually applied at source cadence.
Thus a comparable per-pass timing does **not** establish comparable total cost.
Measure both isolated dispatch and compositor behavior at 60/90 Hz before release.
No half-precision change, temporal reuse or upscaler is introduced without evidence.
