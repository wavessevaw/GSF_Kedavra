#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay recorded source timing through scheduler versions and compare pacing.

  kfg_replay.py events  TRACE -o EVENTS.csv      latch/analysis timing of a trace: the
                                                 latch_id/me_ready columns (GSF 1.4.5 traces,
                                                 kfg_sim, kfg_replay), else a best-effort
                                                 reconstruction from shown frames (older traces)
  kfg_replay.py hitch   -o EVENTS.csv [...]      synthetic timing: a GPU-bound source with one
                                                 long frame every few seconds (the 89 ms Deck
                                                 hitch), optional late analysis after it
  kfg_replay.py metrics TRACE [--json]           pacing metrics of a trace
  kfg_replay.py compare --bin LABEL=PATH ... EVENTS.csv ... [--me-lag N] [--json OUT]
                                                 replay EVENTS through each kfg_replay binary

Metrics (decision rows in the active state, one per compositor vblank):
  repeat_pct        rows that repeat the previous output
  resync_per_min    resynchronisations (the delay exceeded max_delay: a hard jump)
  jump_per_min      whole-vblank jumps: content advanced more than 1.75 vblanks in one vblank
  speed_change_pct  vblanks whose content step is outside 0.8-1.2 vblank (repeats included)
  freeze_pct        vblanks whose content step is below 1/4 vblank (repeats included)
  real_shown        source frames shown unmodified / sources latched while generating
  latency_ms        display vblank minus the content time shown (vblanks of the latch grid,
                    so it includes analysis lag and the steady delay): mean, p95, p99

Read input traces as data only. The origin of every number is the replay of recorded latch
timing through the scheduler header; it is not a display measurement.
"""
import argparse
import csv
import json
import math
import os
import subprocess
import sys
import tempfile
from fractions import Fraction


def load(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def pct(values, q):
    if not values:
        return None
    v = sorted(values)
    return v[min(len(v) - 1, int(math.ceil(q * len(v))) - 1)]


def events_from_columns(rows):
    """Replay input from the latch columns of a trace (GSF 1.4.5 compositor, kfg_sim, kfg_replay).

    latch_id/latch_vblank/me_ready: the newest source latched since the previous row (a
    decision that was not presented has no row, its latch moves to the next one); decisions:
    scheduler decisions since the previous row, this one included (0: the row re-shows a late
    composition, 2+: decisions in between were not presented)."""
    ev = {}
    prev_k = None
    for r in rows:
        k = int(r["vblank"])
        meta = (int(r["mult"]), int(r.get("cap_fps") or 0), round(1e9 / float(r["refresh_ns"])))
        n = int(r.get("decisions") or 1)
        lo = prev_k + 1 if prev_k is not None else k
        for j in range(max(lo, k - n + 1), k + 1):
            ev.setdefault(j, [0, 0, *meta, 1])[5] = 1
        lid = int(r.get("latch_id") or 0)
        if lid:
            lk = int(r.get("latch_vblank") or k)
            e = ev.setdefault(lk, [0, 0, *meta, 0])
            e[0], e[1] = lid, int(r.get("me_ready") or lk + 1)
        prev_k = k
    return [(k, *v) for k, v in sorted(ev.items())]


def events_reconstructed(rows, me_lag=2, fill_gaps=0):
    """Best effort for traces without latch columns (GSF 1.4.1-rc.1 .. 1.4.4).

    Every source frame that appears in a row is located on the content timeline: a real row
    shows it at its content index, a generated row at pos = T_a + phase * (T_b - T_a), which
    gives the other endpoint when one is known. The latch is that time, moved earlier where
    the frame was used before it could have been analysed (a latch snapped forward onto the
    cap grid is at most one vblank early), and me_ready = latch + me_lag (1.4.1: 2).
    Sources that never appear in a row (content passed over them entirely) are lost.
    """
    T = {}
    first_use = {}
    first_pair = {}
    gens = []
    for r in rows:
        k = int(r["vblank"])
        if r["kind"] == "real":
            a = int(r["src_a"])
            T.setdefault(a, Fraction(int(r["content"])))
            first_use.setdefault(a, k)
        elif r["kind"] == "gen":
            a, b = int(r["src_a"]), int(r["src_b"])
            first_use.setdefault(a, k)
            first_use.setdefault(b, k)
            first_pair.setdefault(b, k)
            pos = Fraction(r["pos"]).limit_denominator(64) if r.get("pos") else Fraction(int(r["content"]))
            gens.append((a, b, Fraction(int(r["phase_num"]), int(r["phase_den"])), pos))
    for _ in range(64):
        changed = False
        for a, b, ph, p in gens:
            if a in T and b not in T:
                T[b] = T[a] + (p - T[a]) / ph
                changed = True
            elif b in T and a not in T:
                T[a] = (p - ph * T[b]) / (1 - ph)
                changed = True
        if not changed:
            break
    latch = {}
    prev = None
    for i in sorted(T):
        t = T[i]
        if t.denominator != 1:
            continue  # quantised phase of a long interval: position not recoverable
        L = int(t)
        if i in first_pair:
            L = min(L, first_pair[i] - me_lag)
        L = min(L, first_use[i])
        L = max(L, int(t) - 1)
        if prev is not None and L <= prev:
            L = prev + 1
        latch[i] = L
        prev = L
    by_k = {L: i for i, L in latch.items()}
    out = []
    ks = [int(r["vblank"]) for r in rows]
    meta = {int(r["vblank"]): r for r in rows}
    allk = set(ks) | set(by_k)
    if fill_gaps:
        # A decision that was not presented has no row; the scheduler still made it.
        srt = sorted(allk)
        for x, y in zip(srt, srt[1:]):
            if 1 < y - x <= fill_gaps:
                allk.update(range(x + 1, y))
    allk = sorted(allk)
    last = rows[0]
    for k in allk:
        r = meta.get(k, last)
        last = r
        i = by_k.get(k, 0)
        out.append((k, i, k + me_lag if i else 0, int(r["mult"]), int(r.get("cap_fps") or 0),
                    round(1e9 / float(r["refresh_ns"])), 1))
    return out


def write_events(events, path):
    with open(path, "w") as f:
        f.write("vblank,latch_id,me_ready,mult,cap_fps,refresh_hz,decide\n")
        for e in events:
            f.write(",".join(str(x) for x in e) + "\n")


def metrics(rows, skip_s=2.0):
    """Pacing metrics of one trace (see the module docstring)."""
    if not rows:
        return {"error": "empty"}
    has_latch = "latch_id" in rows[0]
    k0 = int(rows[0]["vblank"])
    refresh_ns = float(rows[0]["refresh_ns"])
    skip_k = k0 + int(skip_s * 1e9 / refresh_ns)
    n = rep = resync = jumps = speed = steps = freeze = 0
    lat = []
    sources = set()
    shown_real = set()
    seen_ids = set()
    shown = None
    prev_k = None
    vblanks = 0
    for r in rows:
        k = int(r["vblank"])
        active = r["state"] == "active" and k >= skip_k
        lid = int(r.get("latch_id") or 0) if has_latch else 0
        if lid and active:
            sources.add(lid)
        if not active:
            shown, prev_k = None, None
            continue
        n += 1
        kind = r["kind"]
        pos = float(r["pos"]) if r.get("pos") not in (None, "") else float(int(r["content"]))
        if kind == "repeat":
            rep += 1
            here = shown
        else:
            here = pos
            if kind == "real":
                shown_real.add(int(r["src_a"]))
            if not has_latch:
                seen_ids.add(int(r["src_a"]))
                if kind == "gen":
                    seen_ids.add(int(r["src_b"]))
        if r.get("skipped") == "1":
            resync += 1
        if prev_k is not None:
            vblanks += k - prev_k
        if shown is not None and here is not None and prev_k == k - 1:
            step = here - shown
            steps += 1
            if step > 1.75:
                jumps += 1
            if step < 0.8 or step > 1.2:
                speed += 1
            if step < 0.25:
                freeze += 1
        if here is not None:
            lat.append((k - here) * refresh_ns / 1e6)
            shown = here
        prev_k = k
    minutes = max(1e-9, vblanks * refresh_ns / 60e9)
    src = sources if has_latch else seen_ids
    return {
        "rows": n,
        "minutes": minutes,
        "repeat_pct": 100.0 * rep / max(1, n),
        "resync_per_min": resync / minutes,
        "jump_per_min": jumps / minutes,
        "speed_change_pct": 100.0 * speed / max(1, steps),
        "freeze_pct": 100.0 * freeze / max(1, steps),
        "real_shown": len(shown_real & src) / max(1, len(src)),
        "sources": len(src),
        "latency_ms": {"mean": sum(lat) / max(1, len(lat)), "p95": pct(lat, 0.95), "p99": pct(lat, 0.99)},
    }


FIELDS = [("repeat_pct", "repeat %", -1), ("resync_per_min", "resync/min", -1), ("jump_per_min", "jumps/min", -1),
          ("speed_change_pct", "speed chg %", -1), ("freeze_pct", "freeze %", -1), ("real_shown", "real shown", 1),
          ("lat_mean", "lat mean ms", -1), ("lat_p95", "lat p95 ms", -1), ("lat_p99", "lat p99 ms", -1)]


def flat(m):
    out = {k: m.get(k) for k, _, _ in FIELDS if k in m}
    lat = m.get("latency_ms") or {}
    out["lat_mean"], out["lat_p95"], out["lat_p99"] = lat.get("mean"), lat.get("p95"), lat.get("p99")
    return out


def replay(binary, events_path, me_lag=None, cap=None):
    with tempfile.TemporaryDirectory() as d:
        trace = os.path.join(d, "replay.csv")
        cmd = [binary, "--events", events_path, "--trace", trace]
        if me_lag is not None:
            cmd += ["--me-lag", str(me_lag)]
        if cap is not None:
            cmd += ["--cap", str(cap)]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode:
            raise RuntimeError(f"{cmd}: {p.stderr}")
        return load(trace)


def table(results, labels):
    head = f"{'input':34s} {'version':10s} " + " ".join(f"{h:>11s}" for _, h, _ in FIELDS)
    lines = [head]
    for name, per in results.items():
        for lab in labels:
            f = flat(per[lab])
            lines.append(f"{name[:34]:34s} {lab[:10]:10s} " + " ".join(
                f"{f[k]:11.3f}" if f.get(k) is not None else f"{'-':>11s}" for k, _, _ in FIELDS))
    return "\n".join(lines)


def events_hitch(seed=1, frame_ms=38.0, jitter_ms=3.0, me_lag=1, cap=30, hitch_ms=89.0, period_s=10.0,
                 late_after=0, duration_s=125.0, mult=3, hz=90):
    """Synthetic decisions at every vblank: a source slower than the cap is latched on the
    vblank after it is done; every period_s one frame takes hitch_ms; the analysis of the two
    sources after a hitch is late_after vblanks later (contention with the recovering game)."""
    import random
    r = random.Random(seed)
    vb = 1000.0 / hz
    cap_ms = 1000.0 / cap if cap else 0.0
    t, next_h, last_k, i = 0.0, period_s * 1000.0, -10, 0
    lat, late = {}, set()
    while t < duration_s * 1000.0:
        hitch = t >= next_h
        d = hitch_ms if hitch else max(cap_ms, r.gauss(frame_ms, jitter_ms))
        if hitch:
            next_h += period_s * 1000.0
        t += d
        k = max(last_k + 1, math.ceil(t / vb))
        i += 1
        lat[k], last_k = i, k
        if hitch:
            late.update((i, i + 1))
    return [(k, lat.get(k, 0), k + me_lag + (late_after if lat.get(k) in late else 0) if k in lat else 0, mult, cap, hz, 1)
            for k in range(last_k + 1)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("events")
    e.add_argument("trace")
    e.add_argument("-o", "--out", required=True)
    e.add_argument("--me-lag", type=int, default=2, help="reconstruction only: me_ready = latch + N")
    e.add_argument("--fill-gaps", type=int, default=0, help="reconstruction only: add decisions in index gaps up to N")
    h = sub.add_parser("hitch")
    h.add_argument("-o", "--out", required=True)
    for k, v in (("seed", 1), ("frame-ms", 38.0), ("jitter-ms", 3.0), ("me-lag", 1), ("cap", 30), ("hitch-ms", 89.0),
                 ("period-s", 10.0), ("late-after", 0), ("duration-s", 125.0)):
        h.add_argument("--" + k, type=type(v), default=v)
    m = sub.add_parser("metrics")
    m.add_argument("trace")
    m.add_argument("--json", action="store_true")
    c = sub.add_parser("compare")
    c.add_argument("--bin", action="append", required=True, help="LABEL=path to a kfg_replay build")
    c.add_argument("--me-lag", type=int)
    c.add_argument("--cap", type=int)
    c.add_argument("--json")
    c.add_argument("events", nargs="+")
    a = ap.parse_args()
    if a.cmd == "events":
        rows = load(a.trace)
        ev = events_from_columns(rows) if "latch_id" in rows[0] else events_reconstructed(rows, a.me_lag, a.fill_gaps)
        write_events(ev, a.out)
        print(f"{len(ev)} decisions, {sum(1 for x in ev if x[1])} latches -> {a.out}")
    elif a.cmd == "hitch":
        ev = events_hitch(a.seed, a.frame_ms, a.jitter_ms, a.me_lag, a.cap, a.hitch_ms, a.period_s, a.late_after, a.duration_s)
        write_events(ev, a.out)
        print(f"{len(ev)} decisions, {sum(1 for x in ev if x[1])} latches -> {a.out}")
    elif a.cmd == "metrics":
        r = metrics(load(a.trace))
        print(json.dumps(r, indent=1) if a.json else table({os.path.basename(a.trace): {"trace": r}}, ["trace"]))
    else:
        bins = [b.split("=", 1) for b in a.bin]
        results = {}
        for ev in a.events:
            results[os.path.basename(ev)] = {lab: metrics(replay(path, ev, a.me_lag, a.cap)) for lab, path in bins}
        print(table(results, [lab for lab, _ in bins]))
        if a.json:
            with open(a.json, "w") as f:
                json.dump({"origin": "replay of recorded latch timing through the scheduler header",
                           "results": results}, f, indent=1)


if __name__ == "__main__":
    main()
