# Strict multiplier (GSF 1.5 candidate): report

Branch `claude/project-thread-vhu876`, on top of the Silk 2 pacing work (`SILK2_PACING_1_4_5.md`).
Scheduler: `kedavra/sched/kfg_sched.h`. Compositor: `kedavra/gamescope/src/kfg/kfg.cpp`, `hooks.patch`.

**Every pacing number here comes from a simulation or a replay. None is a measurement on a Deck.**
`kfg_sim` is the game, limiter and analysis model. `kfg_replay` replays the latch and analysis timing of
Deck traces B, C and D (Starfield on 1.4.4, 2026-10-28) through the scheduler. It replays decisions and
does not reproduce the display. Image numbers come from `kfg_vktest` on lavapipe (CPU Vulkan). The
lavapipe output matches what the GPU shaders compute, but its timing is not representative.

## Rules implemented

1. **Hard limit, every mode.** At most `min(multiplier - 1, 2)` generated frames between two real
   frames. That is one for x2 and two for x3. In a longer interval (a stall or a hitch), the last
   frame shown is repeated (`KFG_MAX_GEN_PER_REAL`, `kfg_sched_gen_limit`). This also applies to fill
   mode (`kfg_strict 0`, used by Autopilot). There, an interval longer than the limit can fill uses
   the same quantised phases as strict.
2. **Strict xN (`kfg_strict`, default 1).** Interval N = `min(vblanks in the interval, multiplier, 3)`.
   The generated frames sit at phases k/N, each on the vblank nearest its time. On a tie the frame
   waits for its own vblank, so a 6-vblank interval at x3 shows images for 2, 2 and 2 vblanks, not
   1, 2 and 3. The remaining vblanks repeat the frame shown last. Real frames land on vblanks (the
   Silk 2 planner). No extra frames are generated, nothing drifts, and the content clock does not
   change.
   - `kfg_strict 2` (hold): show the next phase early, hold the last one.
   - `kfg_strict 3` (real): hold the real frame and put the generated frames at the end. This is the
     "show the real frame for the remaining vblanks" comparison.
3. **Fractional base** (Custom 35 at 90 Hz is 2.57). Intervals alternate between 2 and 3 vblanks, so
   1 and 2 generated frames, never 3 (`test_strict`). A fractional cadence now keeps every gap at
   `floor(V/P)` or `ceil(V/P)` vblanks. Before, 25 at 60 Hz drifted into 2, 2, 2, 2, 4, and the
   4-vblank gap would have needed a hold.
4. **Custom base at least refresh / 3** (`ManualBaseFPS`). **Auto stays within x2 to x3:**
   `kfg_auto_min_floor` is now refresh / 3, so `kfg_auto_floor 24` no longer gives x3.75. The plugin
   still sends `(1, 24)` for its x3.75 mode, and the engine now runs that as base 30, x3.
5. **Scene cut or unexplained pair.** Every phase repeats the real frame A (`kfg_warp.comp`). Before,
   B was shown from t ≥ 0.5, so at x2 the cut appeared one vblank early, and at x3 at t = 2/3. A cut
   pair is never blended. The scheduler never shows a generated frame without valid analysis
   (fuzz test).
6. **Status JSON (appended after `speedups`, nothing reordered).** New fields:
   - `"strict"`
   - `"shown_per_real"` and `"gen_per_real"` (over the last status period)
   - `"refresh_wanted_hz"`
   - `"steam_fps_limit"`

   The HUD and the GFG UI are unchanged.

## Refresh: how Gamescope and Steam set it (gamescope aa289ea), and what KEDAVRA does

**Atoms (Steam to Gamescope), on the X root window:**
- `GAMESCOPE_DYNAMIC_REFRESH` (internal panel) and `GAMESCOPE_DYNAMIC_REFRESH_EXTERNAL` set
  `g_nDynamicRefreshRate[screen]`.
- `GAMESCOPE_FPS_LIMIT` sets `g_nSteamCompMgrTargetFPS`, Steam's frame limiter.
- There is also a wlserver app refresh-cycle override (`steamcompmgr_set_app_refresh_cycle_override`)
  and a `debug_set_fps_limit` concommand.

**Timing:**
1. `paint_all` takes the dynamic rate when `steamcompmgr_window_should_refresh_switch(focus)` is true.
   That function returns false while VRR is active; otherwise it means a game is focused, or open
   with the corresponding option. Otherwise `paint_all` takes the highest rate the panel offers.
2. Once the target has differed from the current rate for 600 ms (`g_uDynamicRefreshDelay`), it
   calls `HackTemporarySetDynamicRefresh`.
3. That leads to `drm_set_refresh`, a full modeset (`needs_modeset`) with the display's lua
   `dynamic_modegen`.

**Visibility:** a switch is a modeset, so expect a visible blank or hitch on the panel.

**Panels:**
- **OLED:** VFP-only modes for every integer rate from 45 to 90 Hz. The list Steam is offered omits
  some rates, for example 70.
- **LCD:** 40 to 60 Hz, every integer, pixel-clock modegen.

**What KEDAVRA does:**
- The refresh changes only on the Deck LCD (`kfg_refresh`, internal panel at 60 Hz or below) while
  explicit x2/x3 runs under a Steam frame limit, or while Custom runs. The refresh becomes
  N × base, from Steam's rate list (`kfg_refresh 1`).
- **The OLED stays at 90 Hz.** External displays (Dock), Auto and VRR keep Steam's choice. Nothing
  was built for the Dock.
- The hook only overrides `nTargetRefreshHz` before the existing 600 ms / modeset path, and Steam's
  atoms are left alone.
- On the OLED, x2 under a Steam limit of 35 gives 70 frames per second at 90 Hz, with holds.

## Judder at 90 Hz (simulator, two seeds × 120 s, 0.3 % 40 ms spikes)

How to read the columns:
- **dur err**: how long each image stays on screen, against an even split of the real interval
  (mean / P95, in ms).
- **judder**: how far the shown content is from the straight line between the two real frames
  (mean / P95, in ms).
- **gen max**: the most generated frames in one real-to-real interval, over the whole trace.
- **stretch** is the default, `kfg_strict 1`.
- **real-held** is `kfg_strict 3`.

| base, game | 1.4.4: gen/real, gen max, dur err | stretch: repeat %, gen/real, dur err, judder, lat mean | real-held: dur err, judder, lat mean |
|---|---|---|---|
| x2@45, game holds 45 | 1.05, 12, 0.04/0.00 | 1.4, 1.00, 0.11/0.00, 0.46/0.00, 32.0 | 0.11/0.00, 0.48/0.00, 32.0 |
| x2@40 (80 out) | 1.32, 125, 0.01/0.00 | 12.1, 1.00, 1.49/5.56, 2.47/5.56, 34.9 | 1.50/5.56, 3.65/11.11, 37.4 |
| x2@35 (70 out) | 1.69, 227, 0.01/0.00 | 23.3, 1.00, 3.33/5.56, 4.27/5.56, 35.5 | 3.31/5.56, 6.31/11.11, 39.5 |
| x2@30 (60 out) | 2.04, 7, 0.02/0.00 | 33.6, 1.00, 5.56/5.56, 5.67/5.56, 38.2 | 5.52/5.56, 8.40/11.11, 43.6 |
| x3@30, game holds 30 | 2.04, 7, 0.02/0.00 | 0.7, 1.99, 0.06/0.00, 0.18/0.00, 38.1 | 0.09/0.00, 0.26/0.00, 38.2 |
| x3@25 (75 out) | 2.65, 9, 0.01/0.00 | 16.9, 2.00, 2.96/7.41, 3.55/6.94, 46.5 | 2.98/7.41, 5.18/11.11, 49.1 |
| x3@30, game ≈ 26 fps | 3.23, 8 | 28.7, 2.00, 4.86/7.41, 5.19/6.94, 58.4 | 4.86/7.41, 7.86/15.28, 63.0 |
| x2@35, game ≈ 30 fps | 2.82, 9 | 47.1, 1.00, 1.48/5.56, 7.18/11.11, 59.0 | 1.47/5.56, 7.77/11.11, 60.2 |

- **Clean cadences** are x2@45 and x3@30 at 90 Hz, and x2 at 2 × base on the LCD (60/30, 70/35,
  80/40). There the duration error is about 0.1 ms and judder below 0.5 ms (spikes only).
- **Uneven output** (60, 70, 80 or 75 frames per second at 90 Hz) gives images of 1 and 2 vblanks.
  The duration error is up to 5.56 ms (half a vblank) and judder 2.5 to 5.7 ms. This is the expected
  cost; nothing was added to hide it.
- **Stretch against real-held:** stretch is never worse. It has lower judder (x2@30: 5.67 against
  8.40 ms) and 2.5 to 5.5 ms less latency.

## Strict x2 pacing (simulator, `kfg_pacing.py`, 120 s, game 19 ms ± near the 45 cap)

| | 1.4.4 | strict x2 |
|---|---|---|
| generated frames per interval (count) | 1: 3990, 2: 850, 3: 13, 4: 13, 5: 2 | 1: 4850, never more |
| repeats (holds) | 0.11 % | 8.6 % (the 3-vblank intervals of a game missing 45) |
| new-image interval error P95 / P99 | 0.003 / 0.004 ms | 11.1 / 11.1 ms (a hold is one vblank more) |
| latency P95 / P99 | 60.6 / 70.8 ms | 49.1 / 56.6 ms |
| resyncs, reorders | 0, 0 | 0, 0 |

Real and generated frames alternate exactly (real, gen, real, gen at 45; real, gen, hold at 30).

**Deck trace replays, strict.** x2 on the x3 traces uses `kfg_replay --mult 2`.

| trace | build | gen max | repeat % | resync/min | jumps/min | judder mean/P95 | dur err mean/P95 | lat mean/P99 |
|---|---|---|---|---|---|---|---|---|
| B (x3, ~26 fps) | 1.4.4 | 8 (621 intervals > 2) | 0.1 | 1.04 | 1.0 | 0.03/0.00 | 0.02/0.00 | 55.7/66.7 |
| B | strict x3 | 2 | 11.8 | 0 | 7.3 | 2.54/6.94 | 1.94/7.41 | 58.7/77.8 |
| B | strict x2 | 1 | 41.1 | 0 | 1252 | 6.51/11.11 | 3.38/5.56 | 60.5/83.3 |
| C (x2/45, ~27 fps) | 1.4.4 | 9 (1570 > 1) | 0.0 | 0 | 0 | 0.00/0.00 | 0.00/0.00 | 55.6/55.6 |
| C | strict x2 | 1 | 41.3 | 0 | 1300 | 6.49/11.11 | 3.26/5.56 | 58.3/66.7 |
| D (x3 motion, ~25 fps) | 1.4.4 | 9 (1806 > 2) | 0.0 | 0 | 0 | 0.03/0.00 | 0.00/0.00 | 62.4/75.0 |
| D | strict x3 | 2 | 17.2 | 0 | 9.2 | 3.59/6.94 | 3.00/7.41 | 65.3/73.6 |
| D | strict x2 | 1 | 44.7 | 0 | 1809 | 6.90/11.11 | 2.26/5.56 | 67.9/77.8 |

A "jump" means the content moved more than 1.75 vblanks from one vblank to the next. Under strict x2
on a game at 25 to 27 fps (intervals of 3 and 4 vblanks), each 4-vblank interval moves 2 vblanks
per image. That is the cadence of the game itself, not a skip.

## Image quality per phase (lavapipe, 1280×800, gen = generated against the true intermediate)

PSNR and SSIM (luma, 8×8 windows) against the analytic truth at the same phase; *dup* is the nearer
source frame. The engine shaders are 1.4.4's except the cut path, so these are also 1.4.4's numbers.

| scene | x2 t=0.5: PSNR / SSIM (dup) | x3 t=1/3 | x3 t=2/3 | border band PSNR t=0.5 |
|---|---|---|---|---|
| pan | 33.86 / 0.991 (15.09 / 0.260) | 33.51 / 0.990 | 35.21 / 0.992 | 26.49 |
| fast pan (72 px) | 29.14 / 0.956 (16.19 / 0.518) | 29.02 / 0.955 | 29.03 / 0.954 | 22.85 |
| rotation (4°/frame, new) | 30.87 / 0.973 (15.59 / 0.327) | 31.03 / 0.976 | 31.14 / 0.973 | 24.76 |
| zoom (dolly) | 34.21 / 0.989 (16.27 / 0.417) | 33.15 / 0.982 | 31.84 / 0.982 | 28.52 |
| occlusion / disocclusion | 37.43 / 0.993 (18.50 / 0.657) | 35.11 / 0.989 | 35.16 / 0.988 | 43.32 |
| HUD + objects | 36.65 / 0.994 (28.20 / 0.974), HUD err 0.00 | 36.51 / 0.993 | 36.48 / 0.994 | 99 |
| thin lines + HUD (new) | 27.73 / 0.944 (14.85 / 0.206), HUD err 0.22 (after the follow-up: 27.75, HUD 0.00) | 32.66 / 0.985 | 32.66 / 0.984 | 25.37 |
| stairs | 25.17 / 0.954 | 22.82 / 0.918 | 22.91 / 0.919 | 20.38 |
| near object | 31.68 / 0.979 | 31.22 / 0.977 | 31.55 / 0.977 | 32.27 |
| static | exact | exact | exact | exact |
| scene cut | = A exactly (no blend) | = A | = A (1.4.4: B) | - |

**Phase-specific checks:**
- **Warp scale:** `pa = p - t·v`, `pb = p + (1 - t)·v`, with the mid-phase field shifted by
  (t - 0.5)·v. It is exact at t = 0.5.
- **Blend weights:** `(1 - t)(1 - z)` against `t·z`, an unreliable match leaning to the nearer frame.
  At t = 0.5 an unreliable match leans to B. Leaning to A instead was tried; it made no difference
  on occlusion or HUD and cost 0.2 dB on near object, so it was not kept.
- **Occlusion fill:** this is the same code at every phase. Disocclusion is better at 0.5 (37.4 dB)
  than at 1/3 (35.1 dB).
- **No x2-specific bug was found.**
- **Thin lines at t = 0.5 are 5 dB under 1/3 and 2/3.** Speed sweeps (9,3), (10,4), (9,0) and
  (4.5,1.5) px per frame show this follows whether the phase offset `t·v` lands on whole pixels,
  not the phase. At 10,4 px/frame, t = 0.5 is the best phase (35.0 dB) and 1/3 the worst (27.2 dB).
  See the follow-up below: a sharper resampler does not help.

### Follow-up: the three problems the x2 tests found (on top of e10d039)

**1. Thin lines at t = 0.5 come from the source, not the resampler. Nothing changed.**

The ceiling was measured with the exact motion on the CPU (`KFG_IDEAL=1`): A and B are shifted by the
true vector and resampled, and the interior (HUD and borders left out) is compared with the truth.

| thin lines, interior PSNR | bilinear | Catmull-Rom 4×4 | Lanczos-2 4×4 | engine (5-tap Catmull-Rom) |
|---|---|---|---|---|
| 9,3 px/frame, t = 0.5 (half-pixel offsets) | 27.52 | 28.69 | 28.69 | 28.44 |
| 10,4 px/frame, t = 1/3 | 27.01 | 27.59 | 27.67 | 27.64 |

- With the exact motion and any kernel, half-pixel positions top out at 28.7 dB. The engine is
  0.25 dB under that ceiling.
- The cause is the scene itself: one-pixel lines rendered with box-filtered supersampling. No
  interpolation can rebuild them at a half-pixel shift.
- The engine was also tried with each kernel in the warp (`KFG_RESAMPLE`, 9 bilinear taps):

| kernel in the warp | thin lines, whole frame (1/3, 2/3, 0.5) | pan | lavapipe warp time |
|---|---|---|---|
| 5-tap Catmull-Rom (shipped) | 32.66 / 32.66 / 27.73 | unchanged | 26–31 ms |
| full Catmull-Rom | 32.65 / 32.65 / 27.67 | ±0.01 | +1–4 ms |
| Lanczos-2 | 32.62 / 32.62 / 27.70 | ±0.01 | about the same |

  The Deck estimate for the 9-tap kernels is +0.05 to 0.1 ms per output. With no quality gain,
  neither was kept.

**2. False scene cut on slow motion: fixed** (`kfg_stats.comp`).

- **Cause:** the cut test called a pair "unexplained" when the motion residual exceeded 0.015 and
  0.26 × the frame difference. A slow pan of 2 to 3 px/frame has a small frame difference.
- **Not only thin lines:** plain pans at 0,2, 0,3 and 2,2 px/frame were also repeated as cuts. So
  was the `slow_pan` temporal sequence: its PSNR is 29.18 → 34.43 dB now that it is interpolated.
- **Fix:** a moderate residual now counts only with a scattered motion field, or when the residual is
  most of the difference (above 0.85 ×, a cut to or from a flat picture). A block is scattered when
  its vector differs from its right or lower neighbour by more than 4 px.
- **Field scatter by scene:** a cut between two textures scatters 63 % of blocks. Pans, turns, the
  stairs and the dolly scatter 2–25 %.
- **New cut scenes, both caught:** `cut_to_flat` (a cut to a flat, dark picture) and `cut_stairs`
  (from the stairs to an unrelated texture). The existing `scene_cut` is still caught.
- **Sweep, thin lines and pan:** 0,1, 0,2, 0,3, 2,2, 0,7, 3,0, 9,3 and 24,6 px/frame now have no
  false cuts.
- **Cost:** two more flow reads per level-2 texel. The stats pass goes from 1.24 to 1.75 ms on
  lavapipe. The estimate for the Deck is +0.05 to 0.13 ms. That uses the measured ratio of Deck to
  lavapipe analysis time (6.07 ms in Deck run B against about 59 ms). The 0.13 ms bound assumes
  this single-workgroup pass scales worse.

**3. HUD error at 12 px/frame horizontal: fixed** (`kfg_warp.comp`).

- **Cause:** the HUD's first and last text rows share 4×4 blocks with the world moving behind them,
  and took its vector.
- **Fix:** the warp now keeps a pixel still when a still block is among its four neighbours, the
  pixel lies on a motion boundary, it is the same in both frames, and along one axis zero motion
  explains it at least as well as the vector.
- **Why both conditions are needed:** without the still-block and motion-boundary conditions,
  objects passing over a static background lost 2 dB.
- **Cost:** two reads gate the test, and only pixels on a motion boundary next to a still block pay
  for it. No difference was measurable on lavapipe; the Deck estimate is under 0.03 ms.
- **Motion blur:** a pixel kept still is also left out of the motion blur. At 15 px/frame
  (`--lowbase`) the blur had still smeared the HUD rows, with a HUD error of 2.5–3.1.

**Before → after (e10d039 → this change).** `thin_lines_v3` and `thin_lines_h12` are now gated
scenes.

| scene | t | PSNR before → after | SSIM before → after | HUD err |
|---|---|---|---|---|
| thin_lines_v3 (0,3) | 1/3 | 20.79 → 35.71 (was cut, repeated) | 0.802 → 0.995 | |
| | 2/3 | 17.72 → 35.98 | 0.592 → 0.993 | |
| | 1/2 | 18.82 → 31.24 | 0.666 → 0.976 | |
| thin_lines_h12 (12,0) | 1/3 | 35.45 → 37.70 | 0.9975 → 0.9981 | 2.89 → 0.24 |
| | 2/3 | 36.21 → 39.43 | 0.9971 → 0.9977 | 3.22 → 0.38 |
| | 1/2 | 35.14 → 37.78 | 0.9969 → 0.9976 | 3.48 → 0.30 |
| thin_lines (9,3) | 1/3, 2/3, 1/2 | 32.66 / 32.66 / 27.73 → 32.70 / 32.74 / 27.75 | equal | 0.11–0.22 → 0.00 |
| slow_pan temporal | sequence | 29.18 → 34.43 | | |
| camera / turn sequence | mean | 28.17 / 27.56 → 28.42 / 27.72 | | HUD max 3.85 / 4.36 → 2.53 / 4.00 |
| all other scenes, every phase | | within −0.03 … +0.00 dB | within −0.0001 | |

- **Acceptance:** no scene at any x2 or x3 phase is more than 0.03 dB worse. The worst is
  objects_hud at t = 0.5, 36.65 → 36.62. SSIM is within 0.0001.
- **Tests:** HDR checks, border checks, the sequence and `kfg_posttest` pass. The scheduler and
  pacing tests are unchanged.

## Acceptance against 1.4.4 at the same settings (`compare_sched.py`, 39 scenarios × 3 seeds, B/C/D replays)

**Hard limit:**
- 1.4.4 exceeds the limit in 3946 intervals in the worst scenario, with up to 136 generated frames
  in one interval (warm-up and stalls).
- Even the clean "stable" scenarios exceed it 4 to 24 times, with up to 7 generated frames.
- The Deck traces exceed it 621, 1570 and 1806 times, with up to 9.
- Strict exceeds it 0 times in every scenario and trace. Fill mode (Autopilot) also exceeds it 0
  times.

**Matrix mean, 1.4.4 → strict:**

| metric | 1.4.4 | strict |
|---|---|---|
| repeat % | 0.42 | 7.06 |
| resync/min | 0.63 | 0.14 |
| jumps/min | 2.41 | 61.3 |
| speed change % | 1.75 | 22.4 |
| real shown | 0.990 | 0.999 |
| latency mean | 45.0 ms | 43.9 ms |
| latency P95 | 57.1 ms | 51.1 ms |
| latency P99 | 153.5 ms | 149.1 ms |
| game-to-flip P99 | 74.5 ms | 68.1 ms |
| judder mean | 3.17 ms | 4.33 ms |
| duration error mean | 0.04 ms | 0.99 ms |
| generated frames per real | 1.98 | 1.71 |

- **No-regression result:** 42 regressions. 38 of the 39 scenarios are worse on at least one metric
  (only `refresh_90_to_60` is not), plus the matrix mean and all three Deck replays. There are 0
  hard-limit failures. **The strict no-regression gate against 1.4.4 does not pass, and it cannot
  pass under the hard limit.**
- **Why the gate cannot pass:** every worse metric is a hold that the limit requires. That covers
  repeat, freeze, speed change, jumps (2- to 3-vblank image steps in 4- to 6-vblank intervals),
  judder and duration error. Where 1.4.4 filled a stall or a game under the cap with 3 to 136
  generated frames, strict shows 1 or 2 and repeats the rest.
- **Where the game holds its cadence**, the two builds are equal within noise. These are the
  `*_stable` and `*_30min` scenarios, `heavy_analysis`, `scene_cuts` and `switch_x3_x2`: the same
  repeats (0.01 to 0.03 %) and the same latency. Their only regression is 0.2 to 0.8 jumps a minute.
  Each one is a catch-up after a late composition: with the phases fixed at k/3, losing one vblank in
  a 3-vblank interval means one 2-vblank step. 1.4.4 instead used off-grid phases there, with up to
  7 generated frames in one interval.
- **What got better:** resyncs, latency P95 and P99, game-to-flip latency and real frames shown.
- **Latency costs:**
  - The stall scenario `x3_stalls_100ms` is 10 ms higher in mean latency (35.6 to 46.0 ms). 1.4.4
    jumped forward 36 times a minute there.
  - The round-half-down hold adds 1 to 3 ms of mean latency on the Deck replays (it waits for the
    phase's own vblank).

**Tests:** `make test-sched` (with the new `test_strict`, a hard-limit check in the fuzz test, and
the replay smoke test, which now also checks the limit), `make test-pacing` and `make test-engine`
are green.

Changed test expectations, each because of the hard limit:
- 24 fps at 90 Hz (x3.75) is replaced by 30.
- 25 at 60 Hz allows 3 % speed changes instead of 2 % (2.0 % now, 1.1 % in 1.4.4).
- Silk catch-up allows one freeze per late frame.
- The hitch replay allows up to 3 image steps per 89 ms hitch.
- The Auto floor is refresh / 3.

`kfg.cpp` and the `steamcompmgr.cpp` hooks pass `g++ -fsyntax-only` against gamescope aa289ea. The
only remaining errors there come from header versions in this container (libdrm `hdmi.h`, a newer
wlroots, the scripting option). It has not been built or run on a Deck.
