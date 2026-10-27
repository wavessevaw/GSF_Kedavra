#!/usr/bin/env python3
"""Повторный разбор архивной трассы; ничего не меняет в runtime."""
import argparse
import csv
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--analyzer", type=Path, required=True)
p.add_argument("--trace", type=Path, required=True)
p.add_argument("--expect-mult", type=int)
a = p.parse_args()
spec = importlib.util.spec_from_file_location("kfg_pacing_pinned", a.analyzer)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
opener = gzip.open if a.trace.suffix == ".gz" else open
rows = []
with opener(a.trace, "rt", newline="", errors="replace") as stream:
    for row in csv.DictReader(line.replace("\0", "") for line in stream):
        try:
            int(row["vblank"]), float(row["flip_ns"]), float(row["refresh_ns"])
        except (KeyError, TypeError, ValueError):
            continue
        rows.append(row)
report = module.analyze(rows, skip_s=2.0, expect_mult=a.expect_mult)
report["origin"] = "historical-deck-trace-reanalysis"
report["provenance"] = {
    "kedavra_sha": "5f22938b55f9853f764e927c847d9c1a8d9a8c04",
    "trace_file": a.trace.name,
    "trace_sha256": hashlib.sha256(a.trace.read_bytes()).hexdigest(),
    "analyzer_sha256": hashlib.sha256(a.analyzer.read_bytes()).hexdigest(),
    "new_hardware_measurement": False,
    "image_content_observed": False,
}
print(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True))
