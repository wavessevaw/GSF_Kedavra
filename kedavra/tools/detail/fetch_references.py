#!/usr/bin/env python3
"""Download versioned test images, verifying every byte against the checked-in hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

p = argparse.ArgumentParser()
p.add_argument("out", type=Path)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=True)
manifest = json.loads(Path(__file__).with_name("references.json").read_text())
for name, digest in manifest["sha256"].items():
    dest = a.out/name
    data = dest.read_bytes() if dest.exists() else urllib.request.urlopen(manifest["base_url"]+name, timeout=60).read()
    if hashlib.sha256(data).hexdigest() != digest:
        raise SystemExit("SHA256 mismatch: "+name)
    dest.write_bytes(data)
    print(name, digest)
