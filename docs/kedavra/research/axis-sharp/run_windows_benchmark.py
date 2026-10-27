import json
import pathlib
import re
import subprocess
import time

root = pathlib.Path(__file__).resolve().parent
bin_root = root / 'windows-harness'
data = []
for index, version in enumerate(('baseline', 'candidate', 'baseline', 'candidate', 'baseline')):
    logpath = root / ('windows-bench-%d-%s.log' % (index, version))
    command = [str(bin_root / version / 'kfg_vktest.exe'), '--spv', str(bin_root / version),
               '--scene', 'pan', '--size', '1280x800', '--bench', '128']
    start = time.monotonic()
    run = subprocess.run(command, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, errors='replace', timeout=180)
    logpath.write_text(run.stdout, encoding='utf-8')
    if run.returncode: raise RuntimeError(f'{version} failed: {run.returncode}; see {logpath}')
    m = re.search(r'analysis ([0-9.]+) ms, two outputs ([0-9.]+) ms', run.stdout)
    if not m: raise RuntimeError('GPU timings missing; see ' + str(logpath))
    warp = re.findall(r'g[12]:warp\s+([0-9.]+) \(p99 ([0-9.]+)\)', run.stdout)
    if len(warp) != 2: raise RuntimeError('Warp timings missing')
    row = {'run': index, 'version': version, 'analysis_ms': float(m[1]),
           'two_outputs_ms': float(m[2]), 'warp_median_p99_ms': warp,
           'elapsed_s': round(time.monotonic() - start, 2)}
    data.append(row)
    print(json.dumps(row), flush=True)
(root / 'windows-benchmark.json').write_text(json.dumps(data, indent=2), encoding='utf-8')
