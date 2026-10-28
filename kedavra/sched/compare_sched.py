#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare two scheduler builds on the simulator matrix and on recorded Deck timing.

  compare_sched.py --baseline OLD/kfg_sim --candidate NEW/kfg_sim [--seeds 3]
                   [--replay-baseline OLD/kfg_replay --replay-candidate NEW/kfg_replay EVENTS.csv ...]
                   [--json OUT] [--strict]

Simulator: every scenario of run_scenarios.py plus games that cannot hold the cap, fractional
caps and the stock (aligned) and render-ahead (pipelined) limiter models, each with the same
seeds for both builds (the model's random timing does not depend on the scheduler). Replay:
recorded latch/analysis timing (kfg_replay.py events) through both kfg_replay builds.

Metrics are kfg_replay.metrics (repeats, resyncs, whole-vblank jumps, speed changes, real
frames shown, latency from the latch grid) plus the model's game-to-flip latency of real
frames (kfg_pacing). A metric is a regression when the candidate is worse than the baseline
by more than the noise tolerance below, summed over the matrix and per scenario.
Origin labels: "simulator" (kfg_sim model) and "Deck trace replay" (recorded latch timing,
replayed decisions; no display measurement).
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import kfg_pacing  # noqa: E402
import kfg_replay  # noqa: E402
import run_scenarios  # noqa: E402

NEAR = "--spike-prob 0.003 --spike-ms 40"
EXTRA = [
    # Games that cannot hold the cap (the Deck status logs: 27-29 fps under cap 30).
    dict(name="x3_cap30_game29", args=f"--refresh 90 --mult 3 --cap 30 --game-ms 29 --game-p99-ms 34 {NEAR}", duration=180, mult=3),
    dict(name="x3_cap30_game28", args=f"--refresh 90 --mult 3 --cap 30 --game-ms 33 --game-p99-ms 40 {NEAR}", duration=180, mult=3),
    dict(name="x3_cap30_game25", args=f"--refresh 90 --mult 3 --cap 30 --game-ms 38 --game-p99-ms 48 {NEAR}", duration=180, mult=3),
    dict(name="x2_cap30_60_near", args=f"--refresh 60 --mult 2 --cap 30 --game-ms 29 --game-p99-ms 34 {NEAR}", duration=180, mult=2),
    dict(name="x2_cap45_near", args=f"--refresh 90 --mult 2 --cap 45 --game-ms 19 --game-p99-ms 23 {NEAR}", duration=180, mult=2),
    dict(name="x375_cap24_near", args=f"--refresh 90 --mult 3 --cap 24 --game-ms 37 --game-p99-ms 43 {NEAR}", duration=180, mult=3),
    # Fractional caps (40 and 35 at 90 Hz) held and not held.
    dict(name="x3_cap40_stable", args="--refresh 90 --mult 3 --cap 40 --game-ms 16 --game-p99-ms 21 --spike-prob 0", duration=180, mult=3),
    dict(name="x3_cap40_game34", args=f"--refresh 90 --mult 3 --cap 40 --game-ms 27 --game-p99-ms 34 {NEAR}", duration=180, mult=3),
    dict(name="x3_cap35_near", args=f"--refresh 90 --mult 3 --cap 35 --game-ms 27 --game-p99-ms 32 {NEAR}", duration=180, mult=3),
    # Game stalls (shader compilation, streaming): 1 % of frames 70-100 ms longer, rarer 300 ms.
    dict(name="x3_stalls_70ms", args="--refresh 90 --mult 3 --cap 30 --game-ms 20 --game-p99-ms 25 --spike-prob 0.01 --spike-ms 70", duration=180, mult=3),
    dict(name="x3_stalls_100ms", args="--refresh 90 --mult 3 --cap 30 --game-ms 20 --game-p99-ms 25 --spike-prob 0.01 --spike-ms 100", duration=180, mult=3),
    dict(name="x3_stalls_300ms", args="--refresh 90 --mult 3 --cap 30 --game-ms 20 --game-p99-ms 25 --spike-prob 0.002 --spike-ms 300", duration=180, mult=3),
    dict(name="x3_cap30_game29_stalls", args="--refresh 90 --mult 3 --cap 30 --game-ms 29 --game-p99-ms 36 --spike-prob 0.01 --spike-ms 60", duration=180, mult=3),
    dict(name="x2_cap45_stalls", args="--refresh 90 --mult 2 --cap 45 --game-ms 19 --game-p99-ms 23 --spike-prob 0.01 --spike-ms 70", duration=180, mult=2),
    # One long game frame (an 89 ms hitch measured on the Deck: Starfield, GPU-bound ~26 fps,
    # x3 at 90 Hz) every 15 s, with one-pass and split analysis.
    dict(name="x3_hitch89", args="--refresh 90 --mult 3 --cap 30 --game-ms 36 --game-p99-ms 44 --spike-prob 0 "
         + " ".join(f"--event {t}:gap:55" for t in range(10, 180, 15)), duration=180, mult=3),
    dict(name="x3_hitch89_me2", args="--refresh 90 --mult 3 --cap 30 --game-ms 36 --game-p99-ms 44 --spike-prob 0 --me-min-vblanks 2 "
         + " ".join(f"--event {t}:gap:55" for t in range(10, 180, 15)), duration=180, mult=3),
    # Analysis split over two vblanks (heavy scenes, 1.4.1 timing).
    dict(name="x3_cap30_near_me2", args=f"--refresh 90 --mult 3 --cap 30 --game-ms 29 --game-p99-ms 34 --me-min-vblanks 2 {NEAR}", duration=180, mult=3),
]
for base in ("x3_cap30_game29", "x3_cap40_game34", "x2_cap45_near"):
    sc = next(s for s in EXTRA if s["name"] == base)
    EXTRA.append(dict(sc, name=base + "_aligned", args=sc["args"] + " --aligned 1"))
    EXTRA.append(dict(sc, name=base + "_pipelined", args=sc["args"] + " --pipelined 1"))

# Noise tolerance per metric (worse direction): absolute.
TOL = {"freeze_pct": 0.02, "repeat_pct": 0.02, "resync_per_min": 0.05, "jump_per_min": 0.05, "speed_change_pct": 0.10,
       "real_shown": 0.002, "lat_mean": 0.3, "lat_p95": 0.5, "lat_p99": 0.5, "game_lat_p50": 0.5, "game_lat_p99": 1.0}
SIGN = {k: s for k, _, s in kfg_replay.FIELDS}
SIGN.update({"game_lat_p50": -1, "game_lat_p99": -1})
KEYS = [k for k, _, _ in kfg_replay.FIELDS] + ["game_lat_p50", "game_lat_p99"]


def sim_metrics(sim, sc, seed, d):
    trace = os.path.join(d, f"{sc['name']}_{seed}.csv")
    srcs = os.path.join(d, f"{sc['name']}_{seed}_src.csv")
    cmd = [sim] + sc["args"].split() + ["--duration", str(sc["duration"]), "--seed", str(seed), "--trace", trace, "--sources", srcs]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError(f"{cmd}: {p.stderr}")
    rows = kfg_pacing.load_trace(trace)
    m = kfg_replay.flat(kfg_replay.metrics(rows))
    rep = kfg_pacing.analyze(rows, kfg_pacing.load_sources(srcs), skip_s=2.0, exclude=sc.get("exclude", ()), expect_mult=None)
    lat = rep.get("latency_ms") or {}
    m["game_lat_p50"], m["game_lat_p99"] = lat.get("p50"), lat.get("p99")
    os.remove(trace)
    os.remove(srcs)
    return m


def mean(ms):
    out = {}
    for k in KEYS:
        v = [m[k] for m in ms if m.get(k) is not None]
        out[k] = sum(v) / len(v) if v else None
    return out


def worse(base, cand):
    bad = []
    for k in KEYS:
        b, c = base.get(k), cand.get(k)
        if b is None or c is None:
            continue
        if SIGN[k] * (c - b) < -TOL[k]:
            bad.append(f"{k} {b:.3f}->{c:.3f}")
    return bad


def fmt(m):
    return " ".join(f"{m[k]:8.3f}" if m.get(k) is not None else f"{'-':>8s}" for k in KEYS)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--candidate", required=True)
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--max-duration", type=float, default=600, help="cap scenario durations (s)")
    ap.add_argument("--only")
    ap.add_argument("--replay-baseline")
    ap.add_argument("--replay-candidate")
    ap.add_argument("--me-lag", type=int, action="append", help="replay with me_ready = latch + N (repeatable)")
    ap.add_argument("--cap", type=int, help="replay: base cap passed to the scheduler")
    ap.add_argument("events", nargs="*")
    ap.add_argument("--json")
    ap.add_argument("--strict", action="store_true", help="exit 1 on any regression")
    a = ap.parse_args()
    scenarios = [s for s in run_scenarios.SCENARIOS + EXTRA if not a.only or a.only in s["name"]]
    head = f"{'scenario':32s} {'ver':4s} " + " ".join(f"{k[:8]:>8s}" for k in KEYS)
    print("origin: simulator (kfg_sim model)\n" + head, flush=True)
    result = {"origin": {"sim": "simulator", "replay": "Deck trace replay"}, "sim": {}, "replay": {}, "regressions": []}
    allb, allc = [], []
    with tempfile.TemporaryDirectory() as d:
        for sc in scenarios:
            sc = dict(sc, duration=min(sc["duration"], a.max_duration))
            mb, mc = [], []
            for seed in range(1, sc.get("seeds", a.seeds) + 1):
                db, dc = os.path.join(d, "b"), os.path.join(d, "c")
                os.makedirs(db, exist_ok=True)
                os.makedirs(dc, exist_ok=True)
                mb.append(sim_metrics(a.baseline, sc, seed, db))
                mc.append(sim_metrics(a.candidate, sc, seed, dc))
            b, c = mean(mb), mean(mc)
            allb += mb
            allc += mc
            bad = worse(b, c)
            result["sim"][sc["name"]] = {"baseline": b, "candidate": c, "worse": bad}
            print(f"{sc['name'][:32]:32s} {'old':4s} {fmt(b)}\n{'':32s} {'new':4s} {fmt(c)}" + (f"\n{'':32s} WORSE: {', '.join(bad)}" if bad else ""), flush=True)
            if bad:
                result["regressions"].append({"sim": sc["name"], "worse": bad})
        tb, tc = mean(allb), mean(allc)
        bad = worse(tb, tc)
        result["sim_total"] = {"baseline": tb, "candidate": tc, "worse": bad}
        print(f"{'MATRIX MEAN':32s} {'old':4s} {fmt(tb)}\n{'':32s} {'new':4s} {fmt(tc)}" + (f"\n{'':32s} WORSE: {', '.join(bad)}" if bad else ""))
        if bad:
            result["regressions"].append({"sim": "total", "worse": bad})
    if a.events and a.replay_baseline and a.replay_candidate:
        print("\norigin: Deck trace replay (recorded latch timing, replayed decisions)\n" + head)
        for ev in a.events:
            for lag in a.me_lag or [None]:
                name = os.path.basename(ev) + (f" lag{lag}" if lag is not None else "")
                b = kfg_replay.flat(kfg_replay.metrics(kfg_replay.replay(a.replay_baseline, ev, lag, a.cap)))
                c = kfg_replay.flat(kfg_replay.metrics(kfg_replay.replay(a.replay_candidate, ev, lag, a.cap)))
                bad = worse(b, c)
                result["replay"][name] = {"baseline": b, "candidate": c, "worse": bad}
                print(f"{name[:32]:32s} {'old':4s} {fmt(b)}\n{'':32s} {'new':4s} {fmt(c)}" + (f"\n{'':32s} WORSE: {', '.join(bad)}" if bad else ""))
                if bad:
                    result["regressions"].append({"replay": name, "worse": bad})
    if a.json:
        with open(a.json, "w") as f:
            json.dump(result, f, indent=1)
    print(f"\nregressions: {len(result['regressions'])}")
    sys.exit(1 if a.strict and result["regressions"] else 0)


if __name__ == "__main__":
    main()
