"""Deterministic CPU checks of two shader proposals; not GPU performance tests."""
import argparse
import difflib
import hashlib
import itertools
import json
import pathlib
import random
import struct

ROOT = pathlib.Path(__file__).resolve().parent
OUT = ROOT / "proposals"


def f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]


def predictor_choice(preds, costs, cached):
    base = preds[0]
    winner, origin, best, reads = (0, 0), base, 1e9, 0
    base_cost = costs[base]
    if cached:
        reads += 1
    for i, v in enumerate(preds):
        o = v if i == len(preds) - 1 else base
        if cached and v == base:
            raw = base_cost
        else:
            raw = costs[v]
            reads += 1
        penalty = f32(f32(0.02 * 0.1) * f32(abs(v[0]-o[0]) + abs(v[1]-o[1])))
        c = f32(raw + penalty)
        if c < best:
            winner, origin, best = v, o, c
    return (winner, origin, best), reads


def warp_choice(candidates, own, photo, mode):
    best, selected = 1e9, candidates[own]
    for i, c in enumerate(candidates):
        duplicate = i != own and c[:2] == candidates[own][:2]
        valid, value = photo[c[:2]]
        if duplicate and (mode == "release" or (mode == "fixed" and valid)):
            continue
        e = f32((value if valid else c[3]) - (0.02 if i == own else 0))
        if e < best:
            best, selected = e, c
    return selected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=pathlib.Path, default=ROOT / 'src')
    args = parser.parse_args()
    shaders = args.source_root / 'kedavra/shaders'
    for name, expected in {'kfg_me.comp': '01f3e7e92eeeae205ed1b22320e7b08e279a4bee',
                           'kfg_warp.comp': '72b04f987f62a62006e304ce117afe5c7d7ec815'}.items():
        data = (shaders / name).read_bytes()
        actual = hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
        if actual != expected:
            raise ValueError(f'{name}: source blob {actual} does not match official baseline {expected}')
    OUT.mkdir(exist_ok=True)
    rng = random.Random(14120261025)
    me_cases = 0
    for n in range(1, 7):
        for _ in range(5000):
            palette = [(rng.randrange(-8, 9)/2, rng.randrange(-8, 9)/2) for _ in range(6)]
            preds = [rng.choice(palette) for _ in range(n)]
            costs = {v: f32(rng.choice([0, 0.004, 0.1, rng.random()])) for v in preds}
            assert predictor_choice(preds, costs, False)[0] == predictor_choice(preds, costs, True)[0]
            me_cases += 1
    # Exhaust duplicate patterns and exact ties, including the special last-origin rule.
    for preds in itertools.product(((0, 0), (1, 0), (-1, 0)), repeat=6):
        for cost in (0, 0.004, 0.25):
            costs = {v: f32(cost) for v in preds}
            assert predictor_choice(preds, costs, False)[0] == predictor_choice(preds, costs, True)[0]
            me_cases += 1
    scenarios = {}
    for name, preds in {
        "all_equal": [(1, 0)]*6,
        "five_equal": [(1, 0)]*5+[(2, 0)],
        "all_unique": [(i, 0) for i in range(6)],
    }.items():
        costs = {v: f32(0.1) for v in preds}
        scenarios[name] = {"release_sad_calls": predictor_choice(preds, costs, False)[1],
                           "proposal_sad_calls": predictor_choice(preds, costs, True)[1]}

    candidates = [(64, 0, .9, .19), (64, 0, .1, .01), (0, 0, .5, .1), (0, 0, .5, .1)]
    # At phase .5 there is no pair-field projection; this pixel really has own=0.
    fr = [((coordinate + .5) / 4 - .5) % 1 for coordinate in (2, 102)]
    assert (0 if fr[0] < .5 else 1) + (0 if fr[1] < .5 else 2) == 0
    assert 2 - .5 * 64 < 0 <= 2 + .5 * 64 < 1280
    photo = {(64, 0): (False, 0), (0, 0): (True, .1)}
    old = warp_choice(candidates, 0, photo, "release")
    new = warp_choice(candidates, 0, photo, "fixed")
    reference = warp_choice(candidates, 0, photo, "reference")
    assert old[:2] == (0, 0) and new[:2] == (64, 0) and new == reference
    warp_cases = 0
    for _ in range(50000):
        palette = [(rng.randrange(-2, 3)*16, rng.randrange(-2, 3)*16) for _ in range(4)]
        cand = [(*rng.choice(palette), rng.random(), rng.choice([.01, .1, .19, .5])) for _ in range(4)]
        own = rng.randrange(4)
        photo = {v: (bool(rng.randrange(2)), rng.choice([0, .1, .19, .5])) for v in palette}
        assert warp_choice(cand, own, photo, "fixed") == warp_choice(cand, own, photo, "reference")
        warp_cases += 1

    me = (shaders / "kfg_me.comp").read_text(encoding="utf-8")
    anchor = "\t\tvec2 origin = base;\n\t\tfor (int i = 0; i < npred; i++) {"
    replacement = "\t\tvec2 origin = base;\n\t\tfloat base_sad = sad(sa, sb, q, base, size);\n\t\tfor (int i = 0; i < npred; i++) {"
    assert me.count(anchor) == 1
    updated = me.replace(anchor, replacement)
    anchor = "\t\t\tfloat c = sad(sa, sb, q, preds[i], size) + u_lambda * 0.1 * (abs(preds[i].x - o.x) + abs(preds[i].y - o.y));"
    replacement = "\t\t\tfloat raw_sad = base_sad;\n\t\t\tif (i != 0 && any(notEqual(preds[i], base)))\n\t\t\t\traw_sad = sad(sa, sb, q, preds[i], size);\n\t\t\tfloat c = raw_sad + u_lambda * 0.1 * (abs(preds[i].x - o.x) + abs(preds[i].y - o.y));"
    assert updated.count(anchor) == 1
    updated = updated.replace(anchor, replacement)
    variants = {"kfg_me.comp": (me, updated)}
    warp = (shaders / "kfg_warp.comp").read_text(encoding="utf-8")
    anchor = "\t\t\t// The own block wins identical motion by its bias. Avoid two colour\n\t\t\t// reads for duplicates, common on clamped borders and straight edges.\n\t\t\tif (i != own && cand[i].xy == cand[own].xy)\n\t\t\t\tcontinue;\n"
    assert warp.count(anchor) == 1
    fixed = warp.replace(anchor, "")
    anchor = "\t\t\tfloat e = cand[i].w;\n\t\t\tif (edge_distance(a_pos, size) >= 0.0 && edge_distance(b_pos, size) >= 0.0) {"
    replacement = "\t\t\tbool photo_valid = edge_distance(a_pos, size) >= 0.0 && edge_distance(b_pos, size) >= 0.0;\n\t\t\t// Equal geometry shares photometric error, but not the stored OOB residual.\n\t\t\tif (photo_valid && i != own && cand[i].xy == cand[own].xy)\n\t\t\t\tcontinue;\n\t\t\tfloat e = cand[i].w;\n\t\t\tif (photo_valid) {"
    assert fixed.count(anchor) == 1
    variants["kfg_warp.comp"] = (warp, fixed.replace(anchor, replacement))
    hashes = {}
    for name, (baseline, proposal) in variants.items():
        path = OUT / name
        path.write_text(proposal, encoding="utf-8", newline="\n")
        patch = ''.join(difflib.unified_diff(baseline.splitlines(True), proposal.splitlines(True),
                         fromfile="a/kedavra/shaders/"+name, tofile="b/kedavra/shaders/"+name))
        (OUT / (name+".patch")).write_text(patch, encoding="utf-8", newline="\n")
        hashes[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    result = {"baseline": "5f22938b55f9853f764e927c847d9c1a8d9a8c04", "kind": "CPU selection model, not GPU execution",
              "seed": 14120261025, "me_equivalence_cases": me_cases, "warp_reference_cases": warp_cases,
              "sad_call_models": scenarios, "oob_counterexample": {"release_vector": old[:2], "proposal_vector": new[:2],
              "reference_vector": reference[:2], "pixel": [2, 102], "phase": .5, "size": [1280, 800],
              "note": "For v64, A.x=-30 is outside and B.x=34 inside. Third distinct vector wins only in release model."},
              "proposal_sha256": hashes, "gpu_speedup": "Not measured", "power_delta": "Not measured",
              "input_latency_delta": "Not measured", "pixel_quality_delta": "Not measured"}
    (ROOT / "candidate-proof.json").write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
