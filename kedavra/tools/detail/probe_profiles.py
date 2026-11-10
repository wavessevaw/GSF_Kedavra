#!/usr/bin/env python3
"""Identical-input soft-edge / ringing-proxy control. No upscaler is invoked.

--variant NAME=SPV_DIRECTORY may be repeated. Every variant runs Detail at 50/100%.
CAS and DLS use --spv. The negative control is an analytical sigmoid processed
with Pillow UnsharpMask; it is NOT an OptiScaler capture or a game image.
"""
import argparse
import json
from pathlib import Path
import subprocess
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

p = argparse.ArgumentParser()
p.add_argument('--exe', required=True)
p.add_argument('--spv', required=True)
p.add_argument('--variant', action='append', default=[])
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
root = a.out.resolve(); root.mkdir(parents=True, exist_ok=True)
x = np.arange(256)
base = 65+125/(1+np.exp(-(x-127.5)/1.2))
im = Image.fromarray(np.tile(np.rint(base).astype('uint8'), (64,1))).convert('RGB')
cases = {'soft': im, 'ringing_proxy': im.filter(ImageFilter.UnsharpMask(radius=2, percent=300, threshold=0))}
variants = dict(v.split('=',1) for v in a.variant)
modes = {'cas': (a.spv, 'cas=0.5'), 'dls': (a.spv, 'dls=0.5:0.17')}
for name, spv in variants.items():
    for strength in [0.5, 1]: modes[f'{name}_{strength}'] = (spv, f'detail={strength}')
for name, image in cases.items():
    image.save(root/f'{name}-input.png')
    np.asarray(image.convert('RGBA')).tofile(root/f'{name}-input.rgba')
for mode, (spv, spec) in modes.items():
    batch = root/f'{mode}.batch'
    batch.write_text(''.join(f'256 64 {spec} {root}/{n}-input.rgba {root}/{n}-{mode}.rgba 1\n' for n in cases))
    with (root/f'{mode}.log').open('w') as log:
        subprocess.run([a.exe, '--spv', spv, '--batch', str(batch)], stdout=log, stderr=log, check=True)
result = {'scope': 'analytical controls; not game/upscaler captures', 'cases': {}}
for name, image in cases.items():
    rows = []; result['cases'][name] = {}
    for mode in ['input', *modes]:
        rgb = np.asarray(image) if mode == 'input' else np.fromfile(root/f'{name}-{mode}.rgba', np.uint8).reshape(64,256,4)[...,:3]
        v = rgb[32,:,1].astype(float)
        core = (x>=120)&(x<=136)
        p20,p50,p80 = [float(np.interp(65+t*125,v[core],x[core])) for t in [.2,.5,.8]]
        result['cases'][name][mode] = dict(width20_80_px=p80-p20, position50_px=p50,
            overshoot_codes=float(max(0,v.max()-190)), undershoot_codes=float(max(0,65-v.min())),
            halo_area_code_pixels=float(np.maximum(v-190,0).sum()+np.maximum(65-v,0).sum()))
        row = Image.new('RGB',(256,80)); row.paste(Image.fromarray(rgb),(0,16))
        ImageDraw.Draw(row).text((2,2),mode,fill='white'); rows.append(row)
    matrix = Image.new('RGB',(256,80*len(rows)))
    for i,row in enumerate(rows): matrix.paste(row,(0,80*i))
    matrix.save(root/f'{name}-matrix.png')
(root/'profiles.json').write_text(json.dumps(result,indent=2)+'\n')
print(root/'profiles.json')
