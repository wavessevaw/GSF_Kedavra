#!/usr/bin/env python3
"""Compare saved raw PPM or gzip-compressed PPM outputs; fail on missing frames."""
import argparse, gzip, hashlib, json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('baseline',type=Path);p.add_argument('candidate',type=Path);a=p.parse_args()
def images(root):
 result={}
 for f in root.iterdir():
  if f.name.endswith('.ppm.gz'):
   name=f.name[:-3];data=gzip.decompress(f.read_bytes())
  elif f.suffix=='.ppm':name=f.name;data=f.read_bytes()
  else:continue
  if name in result:raise ValueError('Duplicate image '+name)
  result[name]=hashlib.sha256(data).hexdigest()
 return result
old,new=images(a.baseline),images(a.candidate)
if not old:raise SystemExit('No baseline images')
diff=[k for k in sorted(old.keys()|new.keys()) if old.get(k)!=new.get(k)]
print(json.dumps(dict(baseline=len(old),candidate=len(new),different=diff),indent=2))
raise SystemExit(bool(diff))
