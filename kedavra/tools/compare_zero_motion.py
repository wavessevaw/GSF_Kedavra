"""Compare axis resampling on the same Vulkan image corpus; timings are not RADV."""
import json
import pathlib
import subprocess
import sys
import numpy as np
from PIL import Image

root = pathlib.Path(sys.argv[1]).resolve()
report = pathlib.Path('q1-results').resolve()
report.mkdir(exist_ok=True)
results = {}
for variant in ('baseline', 'candidate'):
    base = root / variant / 'kedavra'
    for mode, args in [('default', []), ('lowbase', ['--lowbase']), ('per-phase', ['--per-phase'])]:
        dest = report / variant / mode
        dest.mkdir(parents=True, exist_ok=True)
        with (dest / 'run.log').open('w') as log:
            run = subprocess.run([str(base / 'build/kfg_vktest'), '--spv', str(base / 'build'),
                '--size', '1280x800', '--json', str(dest / 'metrics.json'),
                '--out', str(dest), *args], stdout=log, stderr=subprocess.STDOUT, timeout=1800)
        results[variant + '/' + mode] = {'exit_code': run.returncode,
            'metrics': json.loads((dest / 'metrics.json').read_text())}
        if run.returncode:
            print(variant, mode, 'exit', run.returncode, 'failure lines:')
            for line in (dest / 'run.log').read_text().splitlines():
                if 'FAIL' in line:
                    print('  ', line)

changes = []
def compare(a, b, path):
    if isinstance(a, dict) and isinstance(b, dict):
        if a.keys() != b.keys(): raise ValueError('Schema mismatch: ' + path)
        for key in a: compare(a[key], b[key], path + '/' + key)
    elif isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b): raise ValueError('Length mismatch: ' + path)
        for i, (x, y) in enumerate(zip(a, b)): compare(x, y, path + '/' + str(i))
    elif a != b:
        entry = {'path': path, 'baseline': a, 'candidate': b}
        if type(a) in (int, float) and type(b) in (int, float): entry['delta'] = b-a
        changes.append(entry)
for mode in ('default', 'lowbase', 'per-phase'):
    compare(results['baseline/' + mode]['metrics'], results['candidate/' + mode]['metrics'], mode)
image_changes = []
for mode in ('default', 'lowbase', 'per-phase'):
    a_dir, b_dir = report / 'baseline' / mode, report / 'candidate' / mode
    a_names = {p.name for p in a_dir.glob('*.ppm')}
    if a_names != {p.name for p in b_dir.glob('*.ppm')}:
        raise ValueError('Image list mismatch: ' + mode)
    for name in sorted(a_names):
        a = np.asarray(Image.open(a_dir / name), dtype=np.int16)
        b = np.asarray(Image.open(b_dir / name), dtype=np.int16)
        if a.shape != b.shape: raise ValueError('Image shape mismatch: ' + name)
        d = np.abs(a-b)
        image_changes.append({'mode': mode, 'file': name,
                              'max_abs_channel': int(d.max()),
                              'changed_pixels': int(np.any(d != 0, axis=2).sum()),
                              'pixels': int(d.shape[0]*d.shape[1]),
                              'mean_abs_channel': float(d.mean())})
(report / 'comparison.json').write_text(json.dumps({'runs': results, 'changes': changes, 'images': image_changes}, indent=2))
print(json.dumps({'exit_codes': {k: v['exit_code'] for k,v in results.items()}, 'changes': changes,
                  'largest_images': sorted(image_changes, key=lambda x: x['mean_abs_channel'], reverse=True)[:20]}, indent=2))
if any(results[v + '/' + m]['exit_code'] for v in ('baseline', 'candidate') for m in ('default', 'lowbase')):
    sys.exit(1)
# The old per-phase path already fails the release's corpus. Demand exactly the
# same result and expose both failures in the logs; do not call it a passing test.
if results['baseline/per-phase']['exit_code'] != results['candidate/per-phase']['exit_code']:
    sys.exit(1)
if results['baseline/per-phase']['exit_code']:
    print('WARNING: per-phase returns a failure on both baseline and candidate; see logs.')
