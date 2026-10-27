import json, os, pathlib, re, subprocess, time

root = pathlib.Path(__file__).resolve().parent
bins = root / 'windows-harness'
rows = []
for scene in ('static', 'objects_hud', 'pan', 'stairs'):
    for index, version in enumerate(('baseline', 'candidate', 'baseline', 'candidate', 'baseline')):
        env = dict(os.environ, KFG_BENCH_SCENE=scene)
        # Same executable with fixed reporting and scene selection, only SPIR-V changes.
        command = [str(bins / 'candidate' / 'kfg_vktest.exe'), '--spv', str(bins / version),
                   '--scene', 'pan', '--size', '1280x800', '--bench', '128']
        started = time.monotonic()
        run = subprocess.run(command, cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, errors='replace', timeout=180)
        log = root / f'{scene}-{index}-{version}.log'
        log.write_text(run.stdout, encoding='utf-8')
        if run.returncode: raise RuntimeError(f'{scene}/{version} failed {run.returncode}')
        warp = re.findall(r'g[12]:warp\s+([0-9.]+) \(p99 ([0-9.]+)\)', run.stdout)
        if len(warp) != 2: raise RuntimeError('Missing warp timings')
        item = {'scene': scene, 'version': version, 'run': index,
                'g1_ms': float(warp[0][0]), 'g2_ms': float(warp[1][0]),
                'total_ms': sum(float(x[0]) for x in warp),
                'elapsed_s': round(time.monotonic()-started, 2)}
        rows.append(item)
        print(json.dumps(item), flush=True)
(root / 'benchmark.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
