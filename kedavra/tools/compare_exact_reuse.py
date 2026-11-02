"""Require byte-identical rendered frames and equal metrics for exact reuse."""
import hashlib
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
report = {"status": "PASS", "hardware_status": "UNVERIFIED", "modes": {}}
for mode in ("normal", "lowbase"):
    def hashes(which):
        folder = root / which / mode
        return {str(p.relative_to(folder)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sorted(folder.rglob("*.ppm"))}
    before, after = hashes("baseline"), hashes("candidate")
    missing = sorted(before.keys() - after.keys())
    added = sorted(after.keys() - before.keys())
    changed = sorted(k for k in before.keys() & after.keys() if before[k] != after[k])
    a = json.loads((root / "baseline" / (mode + ".json")).read_text())
    b = json.loads((root / "candidate" / (mode + ".json")).read_text())
    ok = bool(before) and not (missing or added or changed) and a == b and a["failures"] == 0
    report["modes"][mode] = {"pass": ok, "frames": len(before), "missing": missing,
        "added": added, "changed": changed, "metrics_equal": a == b,
        "baseline_hashes": before, "candidate_hashes": after}
    if not ok:
        report["status"] = "BLOCKED"
(root / "comparison.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({"status": report["status"], "modes": {k: {a: b for a, b in v.items()
    if not a.endswith("hashes")} for k, v in report["modes"].items()}}, indent=2))
sys.exit(report["status"] != "PASS")
