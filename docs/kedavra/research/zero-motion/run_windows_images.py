import json
import pathlib
import subprocess
import numpy as np
from PIL import Image

root = pathlib.Path(__file__).resolve().parent
bins = root / 'windows-harness'
out = root / 'windows-images'
rows = []
for scene in ('static', 'pan', 'objects_hud', 'occlusion', 'stairs', 'near_object'):
    dirs = {}
    for version in ('baseline', 'candidate'):
        dest = out / scene / version
        dest.mkdir(parents=True, exist_ok=True)
        dirs[version] = dest
        run = subprocess.run([str(bins / version / 'kfg_vktest.exe'), '--spv', str(bins / version),
            '--scene', scene, '--size', '1280x800', '--out', str(dest)], cwd=root,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', timeout=180)
        (dest / 'run.log').write_text(run.stdout, encoding='utf-8')
        if run.returncode: raise RuntimeError(f'{scene}/{version} failed {run.returncode}')
    names = {p.name for p in dirs['baseline'].glob('*.ppm')}
    if names != {p.name for p in dirs['candidate'].glob('*.ppm')}: raise RuntimeError('Image list differs')
    for name in sorted(names):
        a = np.asarray(Image.open(dirs['baseline'] / name), dtype=np.int16)
        b = np.asarray(Image.open(dirs['candidate'] / name), dtype=np.int16)
        delta = np.abs(a-b)
        rows.append({'scene': scene, 'file': name, 'changed_pixels': int(np.any(delta != 0, axis=2).sum()),
            'pixels': int(delta.shape[0]*delta.shape[1]), 'max_channel_diff': int(delta.max()),
            'mean_channel_diff': float(delta.mean())})
    print(json.dumps({'scene': scene, 'images': [r for r in rows if r['scene'] == scene]}), flush=True)
(root / 'windows-images.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
