# Reproduced GSF Native FG3 async LMC GPU validation failure

**Date:** 2026-10-18. **Environment:** GitHub Actions Ubuntu 22.04, Mesa lavapipe software Vulkan, Xvfb, 96×64 test images. **Test PR:** [#4](https://github.com/wavessevaw/GSF/pull/4), workflow [run 39101728491](https://github.com/wavessevaw/GSF/actions/runs/39101728491).

The new GSF repository's basic Vulkan and Temporal Engine CI is green, but it originally **did not include** the inherited stringent `Native FG3 GPU validation (software only)` test from `GFG-Extreme/.github/workflows/gfg-native-no-dll.yml`. That workflow was archived as `.github/legacy-workflows/gfg-native-no-dll.yml` during extraction. The new independent `.github/workflows/gsf-native-lmc-gpu-validation.yml` restores its **original 8-real, 14-expected-synthetic, verify-every-2 and no-skips criteria** in both layer orders, without carrying GFG Decky/package dependencies.

## Exact new results

| Metric | GSF first-layer native-first | GSF validation-first |
| --- | ---: | ---: |
| Source frames | 8 | 8 |
| Accepted synthetic WSI presents | 12 | 10 |
| Synthetic skipped | 2 | 4 |
| WSI image acquire unavailable | 0 | 0 |
| Temporal/ring busy | 0 / 0 | 0 / 0 |
| Errors | 0 | 0 |
| Verified intermediate pairs | 2 | 1 |
| Distinct/ordered pixel verification | 2 / 2 | 1 / 1 |
| Output Worker stale jobs | 2 | 3 |
| History-classified skipped outputs | 2 | 4 |
| First logged generated-pair GPU readiness | 12,792.374 ms | 20,849.837 ms |
| First logged motion-model GPU stage | 6,303.221 ms | 9,559.308 ms |
| Later generated-pair first-output readiness | ~19.7–19.9 ms | ~67–73.8 ms |
| Result | FAIL zero unexpected skips | FAIL zero unexpected skips |

These are **software-renderer CI timings**, not predictions of Steam Deck RDNA2 GPU performance. The variation between two layer orders could also reflect different CPU contention on the hosted runners; it is not established that loader order causes the difference.

Compare previous GFG #154/#155 results on 2026-10-17: `real=8 synthetic=8 skipped=6 verify_checks=0`. In the new reproduction, verification can be **positive** (pixel content differs and is ordered) even when the test still fails because of skips.

## Diagnosis grounded in the logs

1. The Vulkan native bridge created and operated successfully (`errors=0`, no acquire-not-ready, no TE/ring-busy).
2. GPU pixel checker recorded `distinct=1 ordered=1` for actual G1/G2 pairs, with substantial nonzero image differences (for example native-first `mad_g1g2=18.161`).
3. The first observed pair had extremely long readiness latency (12.8s or 20.85s); later pairs were much faster.
4. The Output Worker has `stale_after_ms=66.667`, retires 2 or 3 stale jobs, and logs history-only skips. This supports a working hypothesis that **cold-start GPU compilation / initial resource work / software GPU scheduling causes stale-history suppression**, not a proven WSI image-count failure.
5. Precise attribution of the first-pair delay to shader JIT vs queued GPU work vs submission/scheduler still requires separate timestamps. The logs alone do not prove the cause.
6. The old requirement `st.skipped_synthetic == 0` in an **unwarmed, tight-deadline, shared-host software Vulkan test** mixes deadline/performance acceptance with correctness/verified-pixel acceptance; this is fragile. Do not simply remove it.

## Required technical split (proposed; not yet implemented)

**A. Cold startup and recovery regression:** deterministically force an unavailable/slow first GPU pair, require every real frame presented, classify synthetic skips, no deadlock, correct semaphores, and eventual pixel-confirmed regeneration. This test *allows only classified startup drops*, and must assert those exact counts or classes. Measure startup pipeline creation vs first dispatch vs first timeline completion.

**B. Warm steady-state pixel-correctness regression:** fully initialize all pipelines and prewarm GPU LMC with a deterministic sample pair **before opening the measured 8-frame acceptance window**. Require `real=8 synthetic=14 skipped=0` and `verify_checks>0` / `verify_distinct==verify_checks` under an explicit time budget appropriate for lavapipe. A warm-up that silently loses frames inside the measured window is not accepted. Log warmup separately from acceptance and test image IDs.

**C. Separate deadline/pacing test:** fixed fake clock or explicitly instrumented time windows, with expected and intentionally injected misses; test both 2× and 3×, both layer orders, no real-frame loss and correct restoration after stale history.

**D. Steam Deck hardware profile:** only measured RADV times and panel observation may determine actual viable 2×/3× clock budgets. Do not treat a software GPU at 96×64 as a Deck frame-rate benchmark.

## Reproduction configuration

```sh
# From the root of standalone wavessevaw/GSF, after build of
# engine/gfg-temporal lib and engine/gfg-native build-triage/wsi-gpu-smoke.
GFG_NATIVE_FG=1 GFG_NATIVE_GPU_BRIDGE=1 GFG_NATIVE_ASYNC=1 \
GFG_NATIVE_GPU_SYNC=same-device GFG_NATIVE_ALGO=lmc-gpu \
GFG_TEST_EXPECT_GPU=1 GFG_TEST_VALIDATION=1 GFG_TEST_PROTON_CHAIN=1 \
GFG_TEST_FRAMES=8 GFG_NATIVE_GPU_BRIDGE_VERIFY=1 \
GFG_NATIVE_GPU_BRIDGE_VERIFY_EVERY=2 \
VK_INSTANCE_LAYERS=VK_LAYER_GFG_native:VK_LAYER_KHRONOS_validation \
VK_LAYER_PATH="$PWD/engine/gfg-native/build-triage:/usr/share/vulkan/explicit_layer.d" \
xvfb-run -a engine/gfg-native/build-triage/wsi-gpu-smoke
```

**Immediate priority:** retain the **red** CI as an actionable correctness/performance boundary until cold-vs-warm behavior is separated. Resist removing assertions or marking red builds as successful. This test has not proven Deck hardware scanout.
