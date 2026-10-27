import json
from pathlib import Path
import numpy as np
from PIL import Image

root = Path(__file__).resolve().parent
out = []
for a in sorted((root / 'windows-images').glob('*/baseline/*_gen.ppm')):
    b = a.parents[1] / 'candidate' / a.name
    t = a.with_name(a.name.replace('_gen.ppm', '_truth.ppm'))
    if not b.exists() or not t.exists():
        continue
    aa = np.asarray(Image.open(a), dtype=np.int32)
    bb = np.asarray(Image.open(b), dtype=np.int32)
    truth = np.asarray(Image.open(t), dtype=np.int32)
    mask = np.any(aa != bb, axis=2)
    ea = ((aa-truth)**2).sum(axis=2)
    eb = ((bb-truth)**2).sum(axis=2)
    row = {'scene': a.parents[1].name, 'frame': a.name,
           'changed_pixels': int(mask.sum()), 'improved_pixels': int(np.sum(mask & (eb < ea))),
           'worsened_pixels': int(np.sum(mask & (eb > ea))),
           'equal_error_pixels': int(np.sum(mask & (eb == ea))),
           'squared_error_delta': int((eb-ea).sum())}
    out.append(row)
(root / 'truth-comparison.json').write_text(json.dumps(out, indent=2), encoding='utf-8')
print(json.dumps([r for r in out if r['changed_pixels']], indent=2))
