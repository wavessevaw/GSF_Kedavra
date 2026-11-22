"""Compare x2 and both x3 phases over the same eight real inputs."""
from pathlib import Path
import json
import sys

root = Path(sys.argv[1])
for scene in ("h_grain_pan", "h_lines_slow", "h_parallax"):
    def rows(side):
        return json.loads((root / f"{scene}-{side}.json").read_text())["hard"]

    before, after = rows("before"), rows("after")
    assert {row["mode"] for row in after} == {2, 3}
    for a, b in zip(before, after, strict=True):
        assert a["mode"] == b["mode"]
        if scene == "h_grain_pan":
            assert b["errgrad"] < a["errgrad"] and b["sharpflk_pct"] < a["sharpflk_pct"], (a,b)
        else:
            assert a == b, (a,b)
