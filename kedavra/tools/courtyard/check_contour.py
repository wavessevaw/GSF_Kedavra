"""Compare full-engine images from the same real pairs against the rendered phase."""
import json
import subprocess
import sys
from pathlib import Path

before, after = sys.argv[1:]
for scene in ("h_parallax", "h_parallax_reverse", "h_parallax_vertical"):
    def evaluate(shaders):
        output = Path("/tmp") / (scene + "-contour-score.json")
        log = subprocess.check_output(["kedavra/build/kfg_vktest", "--hard", "--size", "480x300",
                                       "--scene", scene, "--spv", shaders, "--json", str(output)], text=True)
        counts = {}
        for line in log.splitlines():
            if line.startswith("rails "):
                mode, phase, frame, ghosts, bg, missing, lit = map(int, line.split()[1:])
                key = mode, phase
                counts[key] = counts.get(key, 0) + ghosts
        assert set(counts) == {(2,1), (3,1), (3,2)}
        return json.loads(output.read_text())["hard"], counts

    old, ghost_old = evaluate(before)
    new, ghost_new = evaluate(after)
    for a, b in zip(old, new, strict=True):
        assert a["mode"] == b["mode"] and b["temporal"] < a["temporal"], (scene,a,b)
        assert b["severe4_pct"] < a["severe4_pct"], (scene,a,b)
    print(scene, ghost_old, ghost_new)
    if scene == "h_parallax":
        assert all(ghost_new[key] < ghost_old[key] for key in ghost_old)
