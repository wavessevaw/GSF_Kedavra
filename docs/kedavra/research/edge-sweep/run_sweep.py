import json, pathlib, subprocess
import numpy as np
from PIL import Image

root = pathlib.Path(__file__).resolve().parent
bins = root / 'windows-harness'
outputs = root / 'images'
rows = []
for scene in ('pan', 'stairs', 'near_object', 'occlusion'):
    for version in ('base', '0.002', '0.005', '0.01'):
        dest = outputs / scene / version
        dest.mkdir(parents=True, exist_ok=True)
        run = subprocess.run([str(bins / 'kfg_vktest.exe'), '--spv', str(bins / version),
            '--scene', scene, '--size', '1280x800', '--out', str(dest)], cwd=root,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', timeout=180)
        (dest / 'run.log').write_text(run.stdout, encoding='utf-8')
        if run.returncode: raise RuntimeError(f'{scene}/{version} failed: {run.returncode}')
        print(f'{scene}/{version}: completed', flush=True)
    base = outputs / scene / 'base'
    for source in sorted(base.glob('*_gen.ppm')):
        truth = source.with_name(source.name.replace('_gen.ppm', '_truth.ppm'))
        if not truth.exists(): raise RuntimeError('Missing truth for '+source.name)
        a = np.asarray(Image.open(source), dtype=np.int32)
        t = np.asarray(Image.open(truth), dtype=np.int32)
        ea = ((a-t)**2).sum(axis=2)
        for version in ('0.002','0.005','0.01'):
            b = np.asarray(Image.open(outputs / scene / version / source.name), dtype=np.int32)
            eb = ((b-t)**2).sum(axis=2)
            changed = np.any(a != b,axis=2)
            rows.append({'scene': scene, 'frame': source.name, 'variant': version,
                'changed': int(changed.sum()), 'improved': int(np.sum(changed & (eb<ea))),
                'worsened': int(np.sum(changed & (eb>ea))), 'delta_sse': int((eb-ea).sum())})
(root / 'sweep-results.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
for version in ('0.002','0.005','0.01'):
    x = [r for r in rows if r['variant']==version and r['changed']]
    print(json.dumps({'variant':version,'frames':len(x),'improved_frames':sum(r['delta_sse']<0 for r in x),
        'worsened_frames':sum(r['delta_sse']>0 for r in x),'changed':sum(r['changed'] for r in x),
        'improved_pixels':sum(r['improved'] for r in x),'worsened_pixels':sum(r['worsened'] for r in x),
        'stairs': [r for r in x if r['scene']=='stairs']}),flush=True)
