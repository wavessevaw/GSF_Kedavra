#!/usr/bin/env python3
"""Package identical-input comparisons and review videos, without raw duplication."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import zipfile
import numpy as np
from PIL import Image, ImageDraw

p = argparse.ArgumentParser()
p.add_argument('--baseline', type=Path, required=True)
p.add_argument('--fixed', type=Path, required=True)
p.add_argument('--experiment', action='append', default=[], help='NAME=compare_edge output directory')
p.add_argument('--controls', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args(); a.out.mkdir(parents=True, exist_ok=True)
variants = {'fixed': a.fixed, **{n:Path(d) for n,d in (s.split('=',1) for s in a.experiment)}}
inputs = sorted(a.baseline.glob('*/input.png'))
groups = {}; temporal = {}; provenance = {}; images = {}
modes = ['off','cas','dls','old','fixed']
def png(rgb):
    buffer = io.BytesIO(); Image.fromarray(rgb).save(buffer, format='PNG'); return buffer.getvalue()
def luma(rgb): return np.dot(rgb.astype(float),[.2126,.7152,.0722])
with zipfile.ZipFile(a.out/'Edge-Evidence.zip','w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
    z.write(Path(__file__).with_name('references.json'),'reference-provenance.json')
    for inp in inputs:
        name = inp.parent.name
        if name == 'bench': continue
        s = np.asarray(Image.open(inp)); images[name] = {}
        provenance[name] = hashlib.sha256(inp.read_bytes()).hexdigest()
        for mode,file in [('input','input.png'),('reference','reference.png'),('off','off.png'),('cas','cas.png'),('dls','dls.png'),('old','detail.png')]:
            path=inp.with_name(file); images[name][mode]=np.asarray(Image.open(path))
            z.write(path,f'frames/{name}/{mode}.png')
        for mode,path in variants.items():
            rgb = np.fromfile(path/(name+'.rgba'),np.uint8).reshape((*s.shape[:2],4))[...,:3]
            images[name][mode] = rgb
            z.writestr(f'frames/{name}/{mode}.png',png(rgb))
        if name.startswith('pan_'): group='pan'
        elif name.startswith('fg_'): group=name.rsplit('_f',1)[0]
        else: continue
        groups.setdefault(group,[]).append(name)
    for group,names in groups.items():
        if len(names)<10: continue
        names=sorted(names); temporal[group]={}
        for mode in [*modes,*[v for v in variants if v!='fixed']]:
            errors=[luma(images[n][mode])-luma(images[n]['reference']) for n in names]
            correction=[luma(images[n][mode])-luma(images[n]['input']) for n in names]
            temporal[group][mode]={
                'reference_error_delta_rms_codes':float(np.mean([np.sqrt(np.mean((b-c)**2)) for c,b in zip(errors,errors[1:])])),
                'correction_delta_rms_codes':float(np.mean([np.sqrt(np.mean((b-c)**2)) for c,b in zip(correction,correction[1:])]))}
        frames=[]
        for n in names:
            strips=[]
            for mode in modes:
                rgb=images[n][mode]; im=Image.new('RGB',(rgb.shape[1],rgb.shape[0]+24))
                im.paste(Image.fromarray(rgb),(0,24)); ImageDraw.Draw(im).text((4,4),mode,fill='white'); strips.append(im)
            frame=Image.new('RGB',(sum(im.width for im in strips),strips[0].height));x=0
            for im in strips: frame.paste(im,(x,0));x+=im.width
            frames.append(frame.tobytes())
        video=a.out/(group+'.mp4')
        proc=subprocess.Popen(['ffmpeg','-v','error','-y','-f','rawvideo','-pixel_format','rgb24','-video_size',f'{frame.width}x{frame.height}',
            '-framerate','30','-i','pipe:0','-c:v','libx264','-crf','12','-pix_fmt','yuv420p',str(video)],stdin=subprocess.PIPE)
        for _ in range(5):
            for data in frames: proc.stdin.write(data)
        proc.stdin.close()
        if proc.wait(): raise RuntimeError('ffmpeg failed')
        z.write(video,'videos/'+video.name)
    report={'hardware_validated':False,'video_playback':'30 fps review, five repeats, not a frame pacing measurement',
            'source':'procedural + licensed reference photos; no named upscaler or OptiScaler captures',
            'input_png_sha256':provenance,'temporal':temporal}
    (a.out/'temporal.json').write_text(json.dumps(report,indent=2)+'\n')
    z.write(a.out/'temporal.json','temporal.json')
    for name,path in variants.items(): z.write(path/'results.json',f'metrics/{name}.json')
    for path in a.controls.iterdir():
        if path.suffix in ['.json','.png']: z.write(path,'controls/'+path.name)
    z.writestr('README.txt','Opt-in Detail bug-fix evidence. Edge experiments were rejected.\n'
        'old = v1.5.1-detail.2; fixed = colour-channel correction.\n'
        'profile3/outer11/width11 are REJECTED experiments and are not installed.\n'
        'Per-frame PNGs are lossless; MP4 is a lossy review aid, not timing evidence.\n'
        'Ring control is procedural Pillow UnsharpMask, not OptiScaler.\n'
        'No Steam Deck GPU or game/upscaler compatibility validation.\n')
path=a.out/'Edge-Evidence.zip'
path.with_suffix('.zip.sha256').write_text(hashlib.sha256(path.read_bytes()).hexdigest()+'  '+path.name+'\n')
print(path)
