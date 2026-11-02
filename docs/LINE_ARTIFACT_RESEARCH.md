# Straight-line/block artifact investigation (EXPERIMENTAL)

Priority from owner: minimize square artifacts on stairs, horizontal/vertical lines and repeated textures before optimizing cost. Source baseline: GSF 62621c4f9b60fd3cb95edafb62a1a44a1bb2c21e (GFG 3.2.2). Exact-reuse cost experiment is separate and is not included.

## Code-grounded hypotheses

- Selection uses a tall 6x14-pixel support intended for stair risers. Its horizontal coverage is smaller, so vertical structures do not receive equivalent support. Test symmetric support in both axes.
- Absolute colour matching can select the wrong phase of repetitive texture. Test matching signed brightness differences alongside colour; this does not resolve all periodic ambiguities.
- At a motion discontinuity, warp chooses among neighbouring vectors using one pixel's colour agreement. A matching pixel can hide a broken line around it. Test a cross-shaped neighbourhood before choosing a vector.
- Repair currently accepts low-error blocks without neighbour repair. On periodic textures low photometric error need not imply correct motion. Investigate confidence from the best/second distinct motion, forward/backward agreement, and edge-aware neighbour support next. Do not simply average opposing foreground/background vectors.

## Methods and primary sources

Brox et al., ECCV 2004: brightness plus gradient constancy and discontinuity-preserving regularization: https://lmbweb.informatik.uni-freiburg.de/Publications/2004/Bro04a/
Mozilla/Terriberry, Adaptive Motion Compensation Without Blocking Artifacts: adaptive overlapped motion compensation: https://www.mozillafoundation.org/en/research/library/adaptive-motion-compensation-without-blocking-artifacts/
These are methodological references, not proof of our implementation. This patch is a small block-matching experiment, not an implementation of the Brox variational solver.

OBMC is a later option only with visibility/confidence gating. The existing KFG_EXPERIMENT_SOFT_SELECT comment records worse thin lines/stair shimmer, so enabling the old soft blend unconditionally is rejected. A general blur/deblock pass is not the first intervention: it can hide blocks while destroying texture.

## Isolated experiments, declared before execution

1. baseline: unchanged shader branches (all experiment macros off).
2. cross: symmetric 20-sample support in selection, mean absolute error.
3. cross_gradient: same support plus 0.2 times mean signed-gradient mismatch. No extra gradient texture fetches; uses the same samples.
4. line_patch: original selection; warp candidate matching averages horizontal and vertical 3-tap errors instead of single-pixel error. Own-block bias remains unchanged.

All macros are OFF by default. Do not combine variants until each has been compared. Wider matching increases work; no GPU or latency improvement is claimed. Default output and integration are unchanged.

Run identical renderer/scene code for all variants, 1280x800, eight frames, x2/x3, temporal seeds on. Scenes: slow/fast line texture, stairs, vertical/horizontal motion of one-pixel lines, parallax fences, fast turn. Three new HARD entries reuse existing scene implementations; existing cases/assertions remain. Save JSON, raw selected frame pairs, shader hashes and logs. These are synthetic Vulkan sequences, not Steam Deck recordings.

Primary metrics: badblk_pct/badblk_max, block-boundary error blk, lineflk and temporal error; inspect line continuity and raw frames. PSNR alone cannot establish success. Screening criterion: at least 10% reduction in badblk_pct or lineflk on a protected target case; no other case's badblk_pct, lineflk, temporal or blk increases by more than 2%; worst PSNR must not fall by more than 0.1 dB. Zero baseline error permits no positive error. These are predeclared experiment filters, not hardware release thresholds. A variant failing these conditions is rejected or investigated, not silently averaged into a win. All cases and modes must be present and finite. Visual review and full SDR/HDR/regression follow before any integration; runtime/performance on Deck remains UNVERIFIED.
