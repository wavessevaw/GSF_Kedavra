# Silk 2 pacing (GSF 1.4.5 candidate): report

Branch `claude/project-thread-vhu876`, on top of GSF 1.4.4. Scheduler: `kedavra/sched/kfg_sched.h`.
The 1.4.2 one-pass analysis, the 1.4.3 homography warp and the 1.4.4 limiter fix are unchanged.

**Every number here is a simulation or a replay, not a measurement on a Deck.** Two methods:

- **simulator**: `kfg_sim` (game, limiter, latch and analysis model) runs both scheduler builds with the
  same seeds; `compare_sched.py` runs every `run_scenarios.py` scenario plus near-cap, fractional-cap,
  stall, hitch and limiter-model variants (39 scenarios, 3 seeds, full durations).
- **Deck trace replay**: latch and analysis timing taken from real Deck traces (`kfg_replay.py events`)
  and played through both scheduler builds (`kfg_replay`). Latch timing does not depend on the
  scheduler, so this replays the decisions; it does not reproduce the display (late compositions,
  missed flips). Traces from 1.4.4 have no latch columns, so latches are rebuilt from the shown frames
  (the analysis lag from `status.jsonl`: `analysis_one_pass=false`, two vblanks). For run B the 1.4.4
  replay reproduces the recording almost exactly (same 13.75-vblank jump, same repeats); for run D it
  starts with a delay one vblank higher than the recording did, so the absolute latency is off by
  about one vblank for both builds.

## What changed

1. **Real frames always land on a vblank** (Silk 2 planner, `kfg_sched_plan`). The content runs from
   real frame to real frame; each output covers an even share of what is left to the next real frame.
   A slow-down or a catch-up changes how many outputs an interval gets (catch-up: one fewer, at most
   1.5x, 1.75x when 3+ vblanks are owed; slow-down: no earlier than the next pair's analysis, at least
   1/4x), never where a real frame lands. Silk 1's 1.25x catch-up steps and aborted slow-downs left the
   content at x.125 offsets for long stretches, so real frames were passed and only generated frames
   shown. A catch-up spans into the next interval (passing one real frame) only when the interval is
   two vblanks or shorter.
2. **Bounded delay after a late source**: a slow-down episode keeps at most one vblank of the delay it
   added (`KFG_REANCHOR`); the rest is paid back by the catch-up. A give-back that fails right away
   (the catch-up is held by the source) backs off (doubling wait) instead of cycling.
3. **No forward jump after a game hitch**: a resynchronisation (content jumps to the newest frame)
   now needs a source gap longer than twice `max_delay` (24 vblanks, 267 ms at 90 Hz: loading,
   suspend), being twice `max_delay` behind, or a new history (warm-up). A long game frame such as the
   89 ms hitch of Deck run B is slowed into and then paid back at 1.25-1.75x. In 1.4.4 it became five
   repeats and a 13.75-vblank (153 ms) content jump.
4. **A source steadily under the cap keeps a constant delay and speed** (Deck run D: 24.8 fps under cap
   30, 3/4/5-vblank intervals, two-vblank analysis). The scheduler waits for such a source at its own
   rate (a 64-frame mean that leaves out hitches), not on the cap's grid, and the latency give-back
   never goes below the delay that at least two of the last 256 intervals needed. In 1.4.4 every
   longer interval slowed the display down, the give-back took the delay back after a calm second,
   and the next long interval started the cycle again (creep, catch-up burst, skipped source).
5. **Removed**: the long-interval quantisation of the content position and the look-ahead slow-down of
   Silk 1 (both superseded by the planner); the debt jump of the first Silk 2 drafts is off
   (`KFG_JUMP_DEBT`), because it skipped content too.
6. **Instrumentation**: the compositor trace appends `latch_id,latch_vblank,me_ready,cap_fps,decisions`
   (newest source latched since the previous row, its analysis-ready vblank, the base cap, decisions
   since the previous row), so later Deck traces replay exactly. The status JSON appends
   `real_missed, jumps, slowdowns, speedups` after `output_height`; existing fields and their order are
   unchanged. `kfg_sim` and `kfg_replay` write the same trace columns.
7. **Tools**: `kfg_replay` (C, builds against any scheduler header), `kfg_replay.py`
   (`events`, `hitch`, `metrics`, `compare`), `compare_sched.py` (matrix + replay, strict mode).
   `kfg_pacing.py`: without a `pos` column, an unchanged content index is no longer counted as a
   reorder (with `pos`, which 1.4.x traces have, the check already used the sub-vblank position).
8. `make test-sched` gains a replay smoke test (hitch timing: no resync, no jump, all real frames shown).

## Metrics

repeat % (outputs repeating the previous one), freeze % (content step below 1/4 vblank, repeats
included), resync/min, jumps/min (content step above 1.75 vblanks), speed chg % (step outside
0.8-1.2 vblank), real shown (source frames shown unmodified / sources), latency = display vblank
minus the content time shown (includes analysis lag and steady delay), game lat = the simulator's
game-to-flip latency of real frames. Noise tolerance for "worse": repeat/freeze 0.02, resync/jump
0.05/min, speed 0.10, real 0.002, latency 0.3 ms (mean), 0.5 ms (P95/P99), game latency 0.5/1.0 ms.

## Results: 1.4.4 → new

### simulator: matrix mean (39 scenarios, 3 seeds)

| | repeat % | freeze % | resync/min | jumps/min | speed chg % | real shown | lat mean ms | lat P95 ms | lat P99 ms | game lat P50 ms | game lat P99 ms |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1.4.4 → new | 0.422 → 0.328 | 0.411 → 0.317 | 0.632 → 0.137 | 2.413 → 0.137 | 1.747 → 1.462 | 0.990 → 0.999 | 45.03 → 43.47 | 57.14 → 50.87 | 153.54 → 147.03 | 52.42 → 51.55 | 74.47 → 68.14 |

### simulator: per scenario (1.4.4 → new; **bold**: worse beyond noise)

| scenario | repeat % | freeze % | resync/min | jumps/min | speed chg % | real shown | lat mean ms | lat P95 ms | lat P99 ms | game lat P50 ms | game lat P99 ms |
|---|---|---|---|---|---|---|---|---|---|---|---|
| x3_30to90_stable | 0.008 → 0.008 | 0.008 → 0.008 | 0.000 → 0.000 | 0.000 → 0.000 | 0.040 → 0.024 | 1.000 → 1.000 | 33.34 → 33.34 | 33.33 → 33.33 | 33.33 → 33.33 | 46.72 → 46.72 | 51.03 → 51.04 |
| x2_45to90_stable | 0.007 → 0.007 | 0.007 → 0.007 | 0.000 → 0.000 | 0.000 → 0.000 | 0.071 → 0.050 | 0.999 → 1.000 | 23.08 → 23.08 | 25.93 → 25.93 | 29.63 → 29.63 | 30.99 → 30.99 | 36.31 → 36.31 |
| x2_30to60_stable | 0.007 → 0.007 | 0.007 → 0.007 | 0.000 → 0.000 | 0.000 → 0.000 | 0.033 → 0.020 | 1.000 → 1.000 | 33.34 → 33.34 | 33.33 → 33.33 | 33.33 → 33.33 | 46.72 → 46.72 | 51.04 → 51.04 |
| x2_40to80_stable | 0.008 → 0.008 | 0.008 → 0.008 | 0.000 → 0.000 | 0.000 → 0.000 | 0.051 → 0.033 | 1.000 → 1.000 | 25.83 → 25.83 | 29.17 → 29.17 | 33.33 → 33.33 | 34.38 → 34.38 | 37.94 → 37.94 |
| x3_30to90_30min | 0.007 → 0.007 | 0.007 → 0.007 | 0.000 → 0.000 | 0.000 → 0.000 | 0.037 → 0.022 | 1.000 → 1.000 | 33.34 → 33.34 | 33.33 → 33.33 | 33.33 → 33.33 | 46.71 → 46.71 | 51.01 → 51.03 |
| x2_45to90_30min | 0.011 → 0.011 | 0.011 → 0.011 | 0.000 → 0.000 | 0.000 → 0.000 | 0.078 → 0.052 | 0.999 → 1.000 | 22.65 → 22.65 | 22.22 → 22.22 | 33.33 → 33.33 | 30.98 → 30.98 | 34.56 → 34.56 |
| x3_game_spikes | 0.087 → 0.088 | 0.087 → 0.088 | 0.000 → 0.000 | 0.000 → 0.000 | 1.426 → 0.933 | 0.992 → 1.000 | 39.03 → 37.47 | 59.26 → 48.15 | 62.96 → 53.24 | 50.64 → 50.45 | 78.16 → 67.27 |
| x3_heavy_analysis | 0.008 → 0.008 | 0.008 → 0.008 | 0.000 → 0.000 | 0.000 → 0.000 | 0.048 → 0.029 | 1.000 → 1.000 | 33.35 → 33.35 | 33.33 → 33.33 | 33.33 → 33.33 | 44.57 → 44.57 | 49.02 → 49.04 |
| x3_scene_cuts | 0.008 → 0.008 | 0.008 → 0.008 | 0.000 → 0.000 | 0.000 → 0.000 | 0.040 → 0.024 | 1.000 → 1.000 | 33.34 → 33.34 | 33.33 → 33.33 | 33.33 → 33.33 | 46.72 → 46.72 | 52.43 → 52.63 |
| x3_fps_drop | 0.006 → 0.006 | 0.006 → 0.006 | 0.000 → 0.000 | 0.000 → 0.000 | 0.246 → 0.117 | 0.998 → 1.000 | **34.69 → 35.17** | 55.56 → 55.56 | 55.56 → 55.56 | 46.73 → 46.73 | 51.00 → 51.01 |
| toggle_off_on | 0.025 → 0.025 | 0.003 → 0.003 | 0.000 → 0.000 | 0.000 → 0.000 | 0.013 → 0.008 | 0.999 → 1.000 | 33.33 → 33.33 | 33.33 → 33.33 | 33.33 → 33.33 | 46.76 → 46.76 | 51.08 → 51.08 |
| switch_x3_x2 | 0.008 → 0.002 | 0.008 → 0.002 | 0.000 → 0.000 | 0.000 → 0.000 | 0.042 → 0.031 | 0.999 → 1.000 | 29.65 → 29.65 | 33.33 → 33.33 | 33.33 → 33.33 | 52.46 → 52.46 | 57.29 → 57.30 |
| refresh_90_to_60 | 0.081 → 0.081 | 0.072 → 0.072 | 0.435 → 0.435 | 0.435 → 0.435 | 0.081 → 0.081 | 0.999 → 0.999 | 26.94 → 26.94 | 33.33 → 33.33 | 33.33 → 33.33 | 46.72 → 46.72 | 50.99 → 50.99 |
| insufficient_compute | 0.395 → 0.395 | 0.005 → 0.005 | 0.000 → 0.000 | 0.000 → 0.000 | **0.489 → 0.864** | 0.991 → 0.993 | 34.30 → 34.21 | 33.33 → 33.33 | 77.78 → 76.85 | 46.76 → 46.76 | 51.09 → 51.09 |
| suspend_resume | 2.795 → 2.795 | 2.795 → 2.795 | 0.337 → 0.337 | 0.337 → 0.337 | 2.837 → 2.828 | 1.000 → 1.000 | 102.47 → 102.47 | 33.33 → 33.33 | 3211.11 → 3211.11 | 46.73 → 46.73 | 51.12 → 51.12 |
| uncapped_40fps_x3_informative | 0.003 → 0.003 | 0.003 → 0.003 | 0.000 → 0.000 | 0.000 → 0.000 | 0.644 → 0.418 | 0.996 → 1.000 | 34.77 → 34.76 | 44.44 → 44.44 | 48.15 → 48.15 | 41.58 → 41.59 | 58.35 → 58.34 |
| x3_cap30_game29 | 0.085 → 0.087 | 0.085 → 0.087 | 0.000 → 0.000 | 0.000 → 0.000 | 1.390 → 0.976 | 0.992 → 1.000 | 46.71 → 45.47 | 66.67 → 55.56 | 74.07 → 62.96 | 49.47 → 49.35 | 78.76 → 69.00 |
| x3_cap30_game28 | 0.085 → 0.085 | 0.085 → 0.085 | 0.000 → 0.000 | 0.000 → 0.000 | 1.472 → 1.184 | 0.991 → 1.000 | 51.77 → 50.40 | 70.37 → 59.26 | 77.78 → 68.06 | 59.76 → 58.12 | 85.66 → 75.07 |
| x3_cap30_game25 | 0.069 → 0.067 | 0.069 → 0.067 | 0.000 → 0.000 | 0.000 → 0.000 | 1.153 → 0.864 | 0.993 → 1.000 | 57.39 → 56.66 | 74.07 → 66.67 | 77.78 → 72.69 | 63.02 → 62.93 | 89.06 → 80.56 |
| x2_cap30_60_near | 0.006 → 0.003 | 0.006 → 0.003 | 0.000 → 0.000 | 0.000 → 0.000 | 1.364 → 1.283 | 0.989 → 0.997 | 54.82 → 52.35 | 83.33 → 66.67 | 83.33 → 81.94 | 55.30 → 55.05 | 100.46 → 86.83 |
| x2_cap45_near | 0.121 → 0.129 | 0.121 → 0.129 | 0.000 → 0.000 | 0.000 → 0.000 | 1.981 → 1.405 | 0.989 → 0.997 | 36.96 → 34.89 | 55.56 → 44.44 | 64.81 → 51.85 | 37.90 → 37.70 | 69.55 → 58.00 |
| x375_cap24_near | 0.092 → 0.094 | 0.092 → 0.094 | 0.000 → 0.000 | 0.000 → 0.000 | 1.457 → 1.168 | 0.990 → 1.000 | 57.70 → 56.35 | 77.78 → 66.67 | 85.19 → 74.07 | 62.30 → 62.16 | 90.94 → 80.18 |
| x3_cap40_stable | 0.000 → 0.000 | 0.000 → 0.000 | 0.000 → 0.000 | 0.000 → 0.000 | 0.350 → 0.404 | 0.988 → 0.998 | 33.26 → 33.29 | 33.33 → 33.33 | 33.33 → 33.33 | 50.86 → 50.85 | 55.10 → 55.09 |
| x3_cap40_game34 | 0.110 → 0.119 | 0.110 → 0.119 | 0.000 → 0.000 | 0.000 → 0.000 | 1.917 → 1.288 | 0.989 → 1.000 | 47.70 → 45.97 | 66.67 → 55.56 | 77.78 → 66.20 | 51.57 → 51.36 | 83.07 → 71.38 |
| x3_cap35_near | 0.085 → 0.089 | 0.085 → 0.089 | 0.000 → 0.000 | 0.000 → 0.000 | 1.578 → 1.101 | 0.991 → 1.000 | 46.28 → 44.87 | 66.67 → 55.56 | 66.67 → 56.48 | 50.66 → 50.52 | 76.63 → 65.41 |
| x3_stalls_70ms | **0.832 → 1.113** | **0.832 → 1.113** | 0.000 → 0.000 | 0.000 → 0.000 | 7.409 → 5.763 | 0.953 → 0.999 | 52.95 → 44.52 | 96.30 → 74.07 | 104.63 → 92.59 | 53.12 → 51.82 | 117.27 → 93.43 |
| x3_stalls_100ms | 3.804 → 2.083 | 3.804 → 2.083 | 18.655 → 0.000 | 36.523 → 0.000 | **5.542 → 7.823** | 0.990 → 0.999 | **35.56 → 45.66** | **35.19 → 74.07** | 124.07 → 118.52 | **46.75 → 51.91** | **57.79 → 91.43** |
| x3_stalls_300ms | 2.275 → 2.275 | 2.275 → 2.275 | 4.383 → 4.383 | 4.383 → 4.383 | 2.766 → 2.733 | 0.999 → 1.000 | 36.47 → 36.47 | 33.33 → 33.33 | 200.00 → 200.00 | 46.74 → 46.74 | 51.53 → 51.54 |
| x3_cap30_game29_stalls | **0.703 → 0.889** | **0.703 → 0.889** | 0.000 → 0.000 | 0.000 → 0.000 | 7.005 → 5.259 | 0.960 → 1.000 | 59.31 → 51.43 | 97.22 → 79.63 | 103.70 → 98.15 | 57.23 → 53.45 | 111.67 → 98.59 |
| x2_cap45_stalls | 3.864 → 1.509 | 3.865 → 1.509 | 0.000 → 0.000 | 49.222 → 0.000 | 11.398 → 8.566 | 0.937 → 0.979 | 62.19 → 44.13 | 100.00 → 77.78 | 136.11 → 96.30 | 61.87 → 43.99 | 113.74 → 93.11 |
| x3_hitch89 | 0.050 → 0.050 | 0.050 → 0.050 | 0.000 → 0.000 | 0.000 → 0.000 | 1.126 → 0.693 | 0.993 → 1.000 | 56.95 → 56.38 | 70.37 → 66.67 | 77.78 → 66.67 | 63.25 → 63.23 | 87.53 → 77.48 |
| x3_hitch89_me2 | 0.006 → 0.023 | 0.006 → 0.023 | 0.000 → 0.000 | 0.000 → 0.000 | 1.261 → 0.801 | 0.991 → 1.000 | 69.20 → 67.50 | 92.59 → 77.78 | 107.41 → 77.78 | 74.48 → 74.34 | 115.34 → 88.55 |
| x3_cap30_near_me2 | 0.008 → 0.012 | 0.008 → 0.012 | 0.000 → 0.000 | 0.000 → 0.000 | 1.417 → 1.059 | 0.990 → 1.000 | 59.32 → 56.59 | 88.89 → 66.67 | 88.89 → 74.07 | 60.70 → 60.46 | 100.04 → 80.60 |
| x3_cap30_game29_aligned | 0.075 → 0.087 | 0.075 → 0.087 | 0.000 → 0.000 | 0.000 → 0.000 | 2.522 → 2.125 | 0.986 → 1.000 | 68.94 → 67.44 | 88.89 → 77.78 | 100.00 → 87.96 | 71.68 → 71.51 | 122.26 → 110.75 |
| x3_cap30_game29_pipelined | 0.006 → 0.010 | 0.006 → 0.010 | 0.000 → 0.000 | 0.000 → 0.000 | 0.793 → 0.712 | 0.995 → 1.000 | 41.06 → 36.05 | 55.56 → 44.44 | 59.26 → 51.85 | 70.35 → 64.82 | 89.15 → 84.03 |
| x3_cap40_game34_aligned | 0.075 → 0.077 | 0.075 → 0.077 | 0.000 → 0.000 | 0.000 → 0.000 | 1.143 → 0.878 | 0.993 → 1.000 | 57.50 → 56.37 | 77.78 → 66.67 | 77.78 → 66.67 | 79.52 → 78.69 | 107.16 → 96.43 |
| x3_cap40_game34_pipelined | 0.058 → 0.056 | 0.058 → 0.056 | 0.000 → 0.000 | 0.000 → 0.000 | 1.584 → 1.265 | 0.990 → 0.998 | 46.77 → 45.14 | 66.67 → 55.56 | 66.67 → 60.19 | 52.83 → 52.48 | 79.26 → 69.32 |
| x2_cap45_near_aligned | 0.008 → 0.008 | 0.008 → 0.008 | 0.000 → 0.000 | 0.000 → 0.000 | 1.804 → 1.043 | 0.987 → 0.997 | 47.05 → 45.88 | 66.67 → 55.56 | 66.67 → 64.35 | 48.87 → 48.77 | 89.32 → 78.51 |
| x2_cap45_near_pipelined | **0.031 → 0.054** | **0.031 → 0.054** | 0.000 → 0.000 | 0.000 → 0.000 | 1.259 → 1.205 | 0.991 → 0.996 | 34.04 → 28.77 | 55.56 → 40.74 | 55.56 → 51.85 | 51.94 → 47.11 | 78.54 → 66.72 |

### Deck trace replay (2026-10-23 trace: rc.1-era capture, me lag forced to 1 or 2; 2026-10-28 runs B, C, D: Starfield on 1.4.4)

| trace | repeat % | freeze % | resync/min | jumps/min | speed chg % | real shown | lat mean ms | lat P95 ms | lat P99 ms |
|---|---|---|---|---|---|---|---|---|---|
| rc.1 Deck trace, deck_g0.csv lag1, cap 40 | 0.046 → 0.036 | 0.045 → 0.035 | 0.136 → 0.068 | 0.000 → 0.000 | 3.616 → 2.819 | 0.959 → 0.988 | **44.46 → 44.95** | 66.67 → 66.67 | 66.67 → 66.67 |
| rc.1 Deck trace, deck_g0.csv lag2, cap 40 | 0.041 → 0.029 | 0.038 → 0.028 | 0.341 → 0.068 | 0.068 → 0.000 | 3.669 → 2.983 | 0.960 → 0.988 | 55.37 → 55.64 | 77.78 → 77.78 | 77.78 → 77.78 |
| 2026-10-28 run B-x3, cap 30 | 0.096 → 0.000 | 0.096 → 0.000 | 1.036 → 0.000 | 1.036 → 0.000 | **0.481 → 0.635** | 0.994 → 0.999 | **55.72 → 57.88** | **55.56 → 66.67** | **66.67 → 77.78** |
| 2026-10-28 run D-x3-motion, cap 30 | 0.000 → 0.000 | 0.000 → 0.000 | 0.000 → 0.000 | 0.000 → 0.000 | 1.295 → 0.671 | 0.987 → 1.000 | **62.37 → 64.17** | 66.67 → 66.67 | **75.00 → 77.78** |
| 2026-10-28 run C-x2-45, cap 45 | 0.000 → 0.000 | 0.000 → 0.000 | 0.000 → 0.000 | 0.000 → 0.000 | 0.346 → 0.192 | 0.993 → 0.999 | 55.57 → 55.57 | 55.56 → 55.56 | 55.56 → 55.56 |

### Deck trace replay method on synthetic timing (`kfg_replay.py hitch`): one 89 ms frame every 10 s, ~26 fps source, x3 at 90 Hz; lagN = analysis lag, lateN = the two sources after the hitch analysed N vblanks later

| events | repeat % | freeze % | resync/min | jumps/min | speed chg % | real shown | lat mean ms | lat P95 ms | lat P99 ms |
|---|---|---|---|---|---|---|---|---|---|
| h_lag1_late0_cap30_s1 | 0.217 → 0.217 | 0.217 → 0.217 | 0.000 → 0.000 | 0.000 → 0.000 | 2.331 → 1.590 | 0.986 → 0.999 | 48.66 → 46.42 | 66.67 → 55.56 | 66.67 → 62.50 |
| h_lag1_late0_cap30_s2 | 0.208 → 0.208 | 0.208 → 0.208 | 0.000 → 0.000 | 0.000 → 0.000 | 2.340 → 1.590 | 0.987 → 1.000 | 47.74 → 46.04 | 66.67 → 55.56 | 77.78 → 66.67 |
| h_lag1_late4_cap0_s1 | 0.678 → 0.651 | 0.678 → 0.651 | 0.488 → 0.000 | 0.488 → 0.000 | 3.867 → 2.837 | 0.973 → 0.999 | 48.58 → 47.03 | 66.67 → 55.56 | 102.78 → 95.83 |
| h_lag1_late4_cap0_s2 | 0.651 → 0.651 | 0.651 → 0.651 | 0.000 → 0.000 | 0.000 → 0.000 | 4.012 → 2.864 | 0.972 → 0.999 | 48.28 → 47.42 | 66.67 → 55.56 | 102.78 → 95.83 |
| h_lag1_late4_cap30_s1 | 0.687 → 0.587 | 0.687 → 0.587 | 0.488 → 0.000 | 0.488 → 0.000 | 3.867 → 2.647 | 0.974 → 0.999 | **49.15 → 54.70** | 66.67 → 66.67 | 102.78 → 95.83 |
| h_lag1_late4_cap30_s2 | 0.714 → 0.641 | 0.714 → 0.642 | 0.976 → 0.000 | 0.976 → 0.000 | 3.696 → 2.873 | 0.976 → 1.000 | 48.21 → 48.16 | 66.67 → 55.56 | 100.00 → 95.83 |
| h_lag2_late0_cap30_s1 | **0.000 → 0.108** | **0.000 → 0.108** | 0.000 → 0.000 | 0.000 → 0.000 | 2.448 → 1.707 | 0.981 → 0.999 | 64.58 → 57.55 | 100.00 → 66.67 | 100.00 → 73.61 |
| h_lag2_late0_cap30_s2 | **0.000 → 0.099** | **0.000 → 0.099** | 0.000 → 0.000 | 0.000 → 0.000 | 2.467 → 1.726 | 0.981 → 0.999 | 63.77 → 57.16 | 100.00 → 66.67 | 100.00 → 77.78 |
| h_lag2_late4_cap30_s1 | 0.894 → 0.479 | 0.894 → 0.479 | 5.854 → 0.000 | 5.854 → 0.000 | **2.132 → 2.764** | 0.991 → 0.999 | **56.09 → 65.83** | **55.56 → 77.78** | **66.67 → 106.94** |
| h_lag2_late4_cap30_s2 | 0.922 → 0.533 | 0.922 → 0.533 | 5.855 → 0.000 | 5.855 → 0.000 | **2.006 → 3.000** | 0.992 → 0.999 | **56.01 → 59.28** | **55.56 → 66.67** | **66.67 → 106.94** |

The 1.4.4 Deck recordings themselves (kfg_replay.py metrics on the recorded trace), for comparison with
the 1.4.4 replay rows above: B 0.096 % repeats, 1.04 resync/min, 0.46 % speed changes, real 0.993;
C 0.35 % speed changes, real 0.993; D 2.0 % speed changes, real 0.981. The recorded D also has 22
missed compositor paints that the replay keeps (one vblank without a decision).

## What got worse (beyond noise), and why

The strict "no metric worse" rule is **not met**. Each remaining regression is the price of one of the
requested behaviours, so it cannot be removed without bringing that defect back:

| where | metric | cause |
|---|---|---|
| sim x3_stalls_100ms (1 % of frames 100 ms longer) | latency mean +10 ms, P95 35→74 ms, game P99 58→91 ms, speed changes 5.5→7.8 % | 1.4.4 resynchronises 18.7 times a minute here (each a 9+ vblank content skip, 36.5 jumps/min); the new build skips nothing and pays the debt back at up to 1.75x, so the delay stays higher for ~15 vblanks after each stall. |
| replay B (one 13-vblank game frame) | latency mean +2.2 ms, P95 +11 ms, P99 +11 ms, speed changes 0.48→0.64 % | the same trade: 1.4.4 showed 5 repeats and a 153 ms content jump; the new build shows no repeat and no jump, and catches up over ~14 vblanks. |
| replay D (source under the cap) | latency mean +1.8 ms, P99 75→78 ms | the constant delay is the one the 5-vblank intervals need (6 vblanks with two-vblank analysis); 1.4.4 dipped to 5 between its slow-down/catch-up cycles. Speed changes halve (1.30→0.67 %), real shown 0.987→1.000. |
| synthetic hitch with 4 vblanks late analysis | latency P99 67→107 ms, speed changes +0.6-1.0 | 1.4.4 resynchronises 5.9 times a minute; new: none, catch-up instead. |
| sim x3_stalls_70ms, x3_cap30_game29_stalls, x2_cap45_near_pipelined; synthetic hitch with two-vblank analysis | repeat % +0.02-0.28 | the new build runs about one vblank less delay in these scenarios (latency mean -5 to -8 ms), so a stall runs out of content one vblank sooner. |
| sim x3_fps_drop, replay deck_g0 lag1 | latency mean +0.5 ms | the give-back floor for a source under the cap. |
| sim insufficient_compute | speed changes 0.49→0.86 % | only inside the degraded window (analysis 20x too slow, 14 degrade/recover switches), excluded from the scenario's own checks. |

Everything else is equal or better; on the matrix mean every metric improves (resyncs 0.63→0.14/min,
jumps 2.41→0.14/min, real shown 0.990→0.999, latency P95 57→51 ms).

## Open risks

- Simulator and replay only. No new Deck run exists for this build; the gamescope part (trace columns,
  status fields) was not compiled here (no gamescope build environment), only reviewed.
- Replay of 1.4.4 traces rebuilds latch times; for run D the replay starts one vblank higher than the
  recording. Traces from this build carry the latch columns and replay exactly.
- Missed compositor paints (Deck: about 0.2 %) still cost a slow-down and a catch-up each.
- A source that hovers just above the cap can switch between cap mode and under-cap mode; the switch
  uses a 64-frame mean, so it should not flap, but it is only simulated.
- Recovery from a hitch takes up to ~15 vblanks of 1.25-1.75x motion instead of one jump.
  `-DKFG_STALL_GAP=1 -DKFG_JUMP_DEBT=8` restores a jump-based recovery (lower latency after stalls,
  visible content skips); `-DKFG_NEED_N=0 -DKFG_UNDERCAP=0` disables the under-cap mode.

## Analysis cost: measured options (lavapipe, CPU Vulkan; relative only)

`kfg_vktest --size 1280x800 --bench 30` (the Deck's 1280x800), GPU timestamps on lavapipe. Absolute
times are CPU-rendered and say nothing about the Deck (Deck: ~6.4-8 ms per pair); the split between
passes and the relative change are what counts. Quality: generated-frame PSNR against the true
intermediate over the 33 test phases of `kfg_vktest`.

| analysis size | analysis (lavapipe) | of which motion search | mean PSNR | phases worse than full size | worst phase | engine test |
|---|---|---|---|---|---|---|
| full 1280x800 (current) | 174.3 ms | 101.4 ms (58 %) | 43.47 dB | - | - | PASS |
| 768 000 px (~1108x693) | 140.7 ms (-19 %) | 81.4 ms | 42.88 dB | 19 of 33 | -3.87 dB | PASS |
| 512 000 px | 96.0 ms (-45 %) | 56.3 ms | 39.64 dB | 25 of 33 | -14.8 dB | 10 FAIL |
| 360 000 px | 75.3 ms (-57 %) | 41.6 ms | 39.62 dB | 26 of 33 | -14.8 dB | 10 FAIL |

Pass split at full size: motion search 58 % (three levels: 26, 31, 44 ms), repair 22 %, selection 18 %,
reductions and global fit 4 %. A lower analysis resolution is **not** admissible under "no quality
regression": even 0.75x area loses up to 3.9 dB on some phases. Options not measured yet: a smaller
search radius on the finest motion level, skipping the finest motion level when the global (homography)
fit explains the frame, one repair pass instead of two, and fusing selection into repair. Each needs
code and the same PSNR gate; none is in this branch.
