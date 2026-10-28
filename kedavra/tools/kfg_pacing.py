#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Frame pacing analyzer for KEDAVRA flip traces.

Reads one CSV row per display refresh, as written by the patched compositor
(times from DRM page-flip events) or by kfg_sim. Computes the acceptance
metrics of the Operation KEDAVRA directive, section 4.2:

  * P95 / P99 absolute presentation interval error between consecutive
    newly presented frames (target interval = one refresh),
  * fraction of refresh slots that did not present a new frame (missed),
  * recurring short/long oscillation,
  * temporal reordering of content,
  * latency (flip - source GPU completion) and its drift over the run,
  * generated outputs per source interval (proof that x2 shows one and x3
    shows two distinct intermediates).

The analyzer never fills in missing data: a metric whose inputs are absent is
reported as null. Whether the trace came from hardware or a model is recorded
in the report only from what the trace says about itself (`source` column or
--origin), never inferred.
"""
import argparse
import csv
import json
import math
import sys
from fractions import Fraction
from collections import Counter, defaultdict

LIMITS = {
    "p95_interval_error_ms": 1.0,
    "p99_interval_error_ms": 2.0,
    "missed_fraction": 0.005,
    "oscillation_fraction": 0.005,
    "reorders": 0,
    "latency_drift_ms": None,  # set from refresh: one refresh period
}


def pct(values, q):
    if not values:
        return None
    v = sorted(values)
    pos = (len(v) - 1) * q
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return v[lo]
    return v[lo] + (v[hi] - v[lo]) * (pos - lo)


def median(values):
    return pct(values, 0.5)


def load_trace(path):
    """Rows of a trace. A row cut short (the device lost power or the file was copied while
    recording; seen as a run of NUL bytes) is dropped instead of failing the analysis."""
    rows = []
    with open(path, newline="", errors="replace") as f:
        for r in csv.DictReader(line.replace("\0", "") for line in f):
            try:
                int(r["vblank"]), float(r["flip_ns"]), float(r["refresh_ns"])
            except (TypeError, ValueError, KeyError):
                continue
            rows.append(r)
    return rows


def load_sources(path):
    done = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            done[int(r["id"])] = float(r["done_ns"])
    return done


def in_windows(t_s, windows):
    return any(a <= t_s < b for a, b in windows)


def position(r):
    """Content position shown by a row, in vblanks: the sub-vblank pos column (GSF 1.4) when
    the trace has it, else the whole content index."""
    v = r.get("pos")
    return float(v) if v not in (None, "") else float(int(r["content"]))


def analyze(rows, sources=None, skip_s=2.0, exclude=(), expect_mult=None, only_active=True):
    if not rows:
        return {"error": "empty trace"}
    try:
        if any(not math.isfinite(float(r[k])) for r in rows for k in ("flip_ns", "refresh_ns")):
            return {"error": "nonfinite timestamp or refresh"}
        if any(float(r["refresh_ns"]) <= 0 for r in rows):
            return {"error": "nonpositive refresh"}
    except (TypeError, KeyError, ValueError):
        return {"error": "invalid timing fields"}
    t0 = float(rows[0]["flip_ns"])
    sel = []
    segment = {}
    segment_id = 0
    previous = None
    for r in rows:
        t_s = (float(r["flip_ns"]) - t0) / 1e9
        if t_s < skip_s or in_windows(t_s, exclude) or (only_active and r.get("state", "active") != "active"):
            segment_id += 1
            previous = None
            continue
        if previous is not None:
            before = (float(previous["flip_ns"]) - t0) / 1e9
            if (r["refresh_ns"] != previous["refresh_ns"]
                    or r.get("epoch") != previous.get("epoch")
                    or any(before < b and t_s >= a for a, b in exclude)):
                segment_id += 1
        segment[id(r)] = segment_id
        previous = r
        sel.append(r)
    if len(sel) < 3:
        return {"error": "too few rows in evaluation window", "rows": len(sel)}

    refresh_ns = median([float(r["refresh_ns"]) for r in sel])
    n = len(sel)
    kinds = Counter(r["kind"] for r in sel)
    reasons = Counter(r["reason"] for r in sel if r["kind"] == "repeat")
    new = [r for r in sel if r["kind"] in ("real", "gen")]

    # Interval error between consecutive newly presented frames. Consecutive rows
    # must be adjacent in the selection (an excluded window breaks the chain).
    idx = {id(r): i for i, r in enumerate(sel)}
    errors, devs = [], []
    for a, b in zip(new, new[1:]):
        dt = float(b["flip_ns"]) - float(a["flip_ns"])
        if segment[id(a)] != segment[id(b)]:
            continue  # only explicit boundaries break the measurement chain
        if dt <= 0:
            return {"error": "non-increasing flip timestamps"}
        target = float(b["refresh_ns"])
        errors.append(abs(dt - target) / 1e6)
        devs.append((dt - target) / 1e6)

    # Recurring short/long alternation: consecutive deviations of opposite sign above 1 ms.
    osc = sum(1 for x, y in zip(devs, devs[1:]) if abs(x) > 1.0 and abs(y) > 1.0 and x * y < 0)

    # Temporal order: the content position of new frames strictly increases.
    has_pos = all(r.get("pos") not in (None, "") for r in new)
    reorders = 0
    steps = Counter()
    prev = None
    prev_segment = None
    for r in new:
        c = position(r)
        if prev_segment != segment[id(r)]:
            prev = None
        prev_segment = segment[id(r)]
        if prev is not None:
            # Sub-vblank positions (pos column): a new frame must move forward. Whole content
            # indices only (traces without pos): a slowed step keeps the same index, so only a
            # step back is a reorder.
            if c < prev or (c == prev and has_pos):
                reorders += 1
            # A silk catch-up step (1.25 or 1.5 vblanks of content) counts as one step.
            steps[int(math.floor(c - prev + 0.25))] += 1
        prev = c

    # Motion as the viewer sees it, one row per vblank: how far the picture moves (in vblanks of
    # content). A repeat shows the previous picture (0). Freezes (< 0.25) and jumps (> 1.75)
    # are hitches; 1.25-1.5 is a catch-up speed-up.
    motion = Counter()
    shown = None
    prev_segment = None
    prev_row = None
    for r in sel:
        if prev_segment != segment[id(r)]:
            shown, prev_row = None, None
        prev_segment = segment[id(r)]
        here = position(r) if r["kind"] in ("real", "gen") else shown
        if (shown is not None and here is not None and prev_row is not None
                and round((float(r["flip_ns"]) - float(prev_row["flip_ns"])) / float(r["refresh_ns"])) == 1):
            step = here - shown
            if step < 0.25:
                motion["freeze"] += 1
            elif step > 1.75:
                motion["jump"] += 1
            elif step > 1.05:
                motion["speedup"] += 1
            elif step < 0.95:
                motion["slowdown"] += 1
            else:
                motion["even"] += 1
        if here is not None:
            shown = here
        prev_row = r

    # Intermediates per source interval (by later endpoint), excluding the first and last
    # intervals which may be cut by the evaluation window.
    per_pair = defaultdict(set)
    pair_spans = defaultdict(set)
    for r in new:
        if r["kind"] == "gen":
            num, den = int(r["phase_num"]), int(r["phase_den"])
            if den <= 0 or not 0 < num < den:
                return {"error": "invalid generated phase"}
            f = Fraction(num, den)
            key = (segment[id(r)], int(r["src_b"]))
            per_pair[key].add((f.numerator, f.denominator))
            pair_spans[key].add(den)
    segment_pairs = defaultdict(list)
    for key in sorted(per_pair):
        segment_pairs[key[0]].append(key)
    pairs = [key for keys in segment_pairs.values() for key in keys[1:-1]]
    gen_hist = Counter(len(per_pair[p]) for p in pairs)
    # Pairs whose source interval equals the multiplier (cadence-locked intervals).
    locked = [p for p in pairs if expect_mult and pair_spans[p] == {expect_mult}]
    locked_hist = Counter(len(per_pair[p]) for p in locked)
    phases = Counter()
    for p in pairs:
        for num, den in per_pair[p]:
            phases[f"{num}/{den}"] += 1

    lat = []
    if sources:
        for r in new:
            if r["kind"] == "real":
                sid = int(r["src_a"])
                if sid in sources:
                    lat.append(((float(r["flip_ns"]) - sources[sid]) / 1e6, (float(r["flip_ns"]) - t0) / 1e9))
    lat_ms = [x for x, _ in lat]
    drift = None
    if len(lat) >= 30:
        third = len(lat) // 3
        drift = median(lat_ms[-third:]) - median(lat_ms[:third])

    unobserved = 0
    for a, b in zip(sel, sel[1:]):
        if segment[id(a)] != segment[id(b)]:
            continue
        dt = float(b["flip_ns"]) - float(a["flip_ns"])
        if dt <= 0:
            return {"error": "non-increasing flip timestamps"}
        unobserved += max(0, int(math.floor(dt / float(b["refresh_ns"]) + 0.5)) - 1)
    # These are inferred fixed-refresh slots. Logger loss and an absent physical flip
    # cannot be distinguished without sequence/commit identity: never call this optical proof.
    missed = (kinds.get("repeat", 0) + unobserved) / (n + unobserved)
    report = {
        "rows": n,
        "duration_s": (float(sel[-1]["flip_ns"]) - float(sel[0]["flip_ns"])) / 1e9,
        "refresh_hz": 1e9 / refresh_ns,
        "kinds": dict(kinds),
        "repeat_reasons": dict(reasons),
        "missed_fraction": missed,
        "repeat_row_fraction": kinds.get("repeat", 0) / n,
        "unobserved_slots": unobserved,
        "estimated_refresh_slots": n + unobserved,
        "slot_count_basis": "fixed-refresh timestamps; lossless trace assumed",
        "interval_error_ms": {
            "p50": pct(errors, 0.5), "p95": pct(errors, 0.95), "p99": pct(errors, 0.99), "max": max(errors) if errors else None,
        },
        "oscillation_fraction": osc / max(1, len(devs) - 1),
        "reorders": reorders,
        "content_steps": {str(k): v for k, v in sorted(steps.items())},
        "generated_per_interval": {str(k): v for k, v in sorted(gen_hist.items())},
        "generated_per_locked_interval": {str(k): v for k, v in sorted(locked_hist.items())},
        "locked_interval_fraction": len(locked) / len(pairs) if pairs and expect_mult else None,
        "content_step_irregular_fraction": (sum(v for k, v in steps.items() if k != 1) / max(1, sum(steps.values()))),
        "phases": dict(phases),
        "latency_ms": {"p50": median(lat_ms), "p95": pct(lat_ms, 0.95), "p99": pct(lat_ms, 0.99)} if lat_ms else None,
        "latency_drift_ms": drift,
        "skips": sum(1 for r in sel if r.get("skipped") == "1"),
        "motion": dict(motion),
        "hitches_per_min": (motion["freeze"] + motion["jump"]) / max(1e-9, (float(sel[-1]["flip_ns"]) - float(sel[0]["flip_ns"])) / 60e9),
        "compose_late": sum(1 for r in sel if r.get("compose_late") == "1"),
    }
    checks = {
        "p95_interval_error": report["interval_error_ms"]["p95"] is not None and report["interval_error_ms"]["p95"] <= LIMITS["p95_interval_error_ms"],
        "p99_interval_error": report["interval_error_ms"]["p99"] is not None and report["interval_error_ms"]["p99"] <= LIMITS["p99_interval_error_ms"],
        "missed_below_0.5pct": missed < LIMITS["missed_fraction"],
        "no_oscillation": report["oscillation_fraction"] < LIMITS["oscillation_fraction"],
        "no_reorder": reorders == 0,
        "uniform_content_steps": report["content_step_irregular_fraction"] < 0.005,
    }
    if drift is not None:
        checks["no_latency_accumulation"] = abs(drift) <= refresh_ns / 1e6
    if expect_mult:
        want = expect_mult - 1
        total = sum(locked_hist.values())
        checks[f"x{expect_mult}_intermediates_99.5pct"] = total > 0 and locked_hist.get(want, 0) / total >= 0.995
        checks["cadence_locked_99.5pct"] = bool(pairs) and len(locked) / len(pairs) >= 0.995
    report["checks"] = checks
    report["pass"] = all(checks.values())
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("--sources", help="source CSV (id,done_ns,...) for latency")
    ap.add_argument("--skip", type=float, default=2.0, help="warm-up seconds excluded")
    ap.add_argument("--exclude", action="append", default=[], help="A:B seconds excluded (transitions)")
    ap.add_argument("--expect-mult", type=int)
    ap.add_argument("--all-states", action="store_true", help="include passthrough/degraded rows")
    ap.add_argument("--origin", default=None, help="label recorded in the report, e.g. 'model' or 'deck-kms'")
    ap.add_argument("--json", help="write report JSON here")
    ap.add_argument("--assert-pass", action="store_true", help="exit 1 unless every check passes")
    a = ap.parse_args()
    rows = load_trace(a.trace)
    sources = load_sources(a.sources) if a.sources else None
    windows = []
    for w in a.exclude:
        lo, hi = w.split(":")
        windows.append((float(lo), float(hi)))
    rep = analyze(rows, sources, a.skip, windows, a.expect_mult, not a.all_states)
    rep["origin"] = a.origin or (rows[0].get("origin") if rows and rows[0].get("origin") else "unlabelled")
    out = json.dumps(rep, indent=2, sort_keys=True)
    if a.json:
        with open(a.json, "w") as f:
            f.write(out + "\n")
    print(out)
    if a.assert_pass and not rep.get("pass"):
        sys.exit(1)


if __name__ == "__main__":
    main()
