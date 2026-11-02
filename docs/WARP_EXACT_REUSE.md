# Exact motion-candidate reuse — experiment, not a release

Baseline: GSF 62621c4f9b60fd3cb95edafb62a1a44a1bb2c21e, the engine of GFG 3.2.2.
Product hypothesis: reduce reconstruction work at moving-object boundaries without changing any selected motion, visibility weight, sampling filter, phase or presentation schedule.

The warp previously sampled A and B four times to score four neighbouring vectors even when vectors were exactly equal. Reuse only the raw photometric error for exact duplicates. Apply each candidate's ownership bias separately; preserve its full z/w payload and original tie order. Experimental soft selection reuses the corresponding colour, preserving per-candidate positional weights. No approximate vector merging, extra frame buffering, new dispatch or synchronization is introduced.

At a two-vector boundary the scoring path requests four source texture samples instead of eight; at three distinct vectors six instead of eight. Four distinct vectors still request eight and incur extra comparisons/cache storage. This is a source-level operation count, NOT a measured GPU speedup or whole-frame reduction. Register pressure/divergence may outweigh the saved reads: reject the experiment if hardware costs regress.

Predeclared correctness gate: execute the unchanged baseline Vulkan tester and renderer with original and candidate shaders, normal and lowbase modes, full default 1280x800 resolution. Every saved PPM must be byte-identical; JSON metrics must be exactly equal; all original assertions must pass. Missing/added frames and empty output block admission. SDR pixel identity is checked; HDR uses the tester's numeric metrics, not full-float image identity. Preserve logs, metrics and per-frame SHA256.

Performance gate for later hardware qualification: same Steam Deck, fixed resolution/refresh/clocks and thermal conditions, 30 warmup pairs and five independent alternating A/B sessions with at least 200 measured pairs each. Record analysis/warp/post GPU timestamps separately, P50/P95/P99 and paired-session uncertainty, plus output pacing and input-to-display where instrumentation exists. A 95% paired confidence interval must exclude zero for a speedup claim; a slowdown beyond premeasured baseline repeatability rejects the change. Do not extrapolate lavapipe timings to Deck GPU or compute savings to input latency. No claim about visual improvement: this experiment must preserve images exactly.

Status: EXPERIMENTAL. No GFG integration, Stable tag, dependency update or UI change.
