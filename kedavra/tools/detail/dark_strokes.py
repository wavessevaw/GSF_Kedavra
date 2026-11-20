"""Compare two actual SDR Focus dispatches on known moving strokes at full strength."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--exe', type=Path, required=True)
parser.add_argument('--base', type=Path, required=True)
parser.add_argument('--candidate', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--photos', type=Path)
parser.add_argument('--fg', type=Path)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
cases = {}
def add(name, image):
    cases[name] = image.convert('RGB')

def gray(array):
    return Image.fromarray(np.rint(array).clip(0,255).astype('uint8'))

y,x = np.indices((56,144))
for angle in (0,30,45,75):
    c,s = np.cos(np.deg2rad(angle)),np.sin(np.deg2rad(angle))
    for phase in range(16):
        distance = (x-71.5)*c + (y-27.5)*s - phase/16
        add(f'line_{angle}_{phase:02}',gray(151-52*np.exp(-0.5*(distance/1.1)**2)))
rng = np.random.default_rng(12345)
for name, array in {
    'noise': 120+rng.normal(0,5,(128,192)),
    'step': np.where(x<72,54,185),
    'dot': np.where((x==72)&(y==28),100,151),
    'broad_shadow': 151-89*np.exp(-0.5*((x-71.5)/12)**2),
    'black_line': np.where(x==72,0,151),
    'flat': np.ones_like(x)*120,
    'ringing': np.array([65]*64+[62,63,57,53,44,46,63,103,152,192,209,211,202,198,192,193]+[190]*64)[None,:].repeat(56,0),
}.items():
    add(name,gray(array))
# A flat blue channel with spatially varying red and green.
v=np.asarray(cases['line_30_05']).copy()
v[...,2]=86
add('constant_blue',Image.fromarray(v))

# Rasterise a curved seam and running stitches at 4x, then soften at display size.
cloth = Image.new('RGB',(768,384),(145,130,109))
pen = ImageDraw.Draw(cloth)
seam = [(i, int(4*(45+10*np.sin(i/150.0)))) for i in range(768)]
pen.line(seam,fill=(51,44,40),width=7)
for j in range(0,720,38):
    pen.line([(xx,yy+20) for xx,yy in seam[j:j+21]],fill=(75,63,53),width=5)
cloth = cloth.resize((192,96),Image.Resampling.LANCZOS)
cloth.save(args.out/'seam-reference.png')
add('seam',cloth.filter(ImageFilter.GaussianBlur(0.95)))
label = Image.new('RGB',(160,56),(170,156,135))
ImageDraw.Draw(label).text((16,15),'KEDAVRA FOCUS  30 / 90',fill=(63,58,54))
label.save(args.out/'text-reference.png')
add('text',label.filter(ImageFilter.GaussianBlur(0.8)))

if args.photos:
    for name,box in [('astronaut',(148,63,308,183)),('chelsea',(135,29,295,149))]:
        original = Image.open(args.photos / (name+'.png')).convert('RGB')
        add(name,original.filter(ImageFilter.GaussianBlur(1.0)).crop(box))
        original.crop(box).save(args.out/(name+'-reference.png'))
        native=name+'_native'
        add(native,original.crop(box))
        original.crop(box).save(args.out/(native+'-reference.png'))
if args.fg:
    for path in sorted(args.fg.glob('h_courtyard_reversal_x3*_gen.ppm')):
        im=Image.open(path)
        box=(im.width//2-128,im.height//2-80,im.width//2+128,im.height//2+80)
        add(path.stem,im.crop(box))
        truth=Image.open(path.with_name(path.name.replace('_gen','_truth'))).crop(box)
        truth.save(args.out/(path.stem+'-reference.png'))
outputs={}
with tempfile.TemporaryDirectory() as scratch:
    folder=Path(scratch)
    for name,im in cases.items():
        np.asarray(im.convert('RGBA')).tofile(folder/(name+'.rgba'))
    for mode,shaders in [('base',args.base),('candidate',args.candidate)]:
        manifest=folder/'batch.txt'
        manifest.write_text(''.join(f'{im.width} {im.height} detail=1 {folder}/{name}.rgba {folder}/{name}.out 1\n' for name,im in cases.items()))
        with (args.out/(mode+'.log')).open('w') as log:
            subprocess.run([str(args.exe.resolve()),'--spv',str(shaders.resolve()),'--batch',str(manifest)],stdout=log,stderr=subprocess.STDOUT,check=True)
        outputs[mode]={n:np.fromfile(folder/(n+'.out'),np.uint8).reshape(im.height,im.width,4)[...,:3] for n,im in cases.items()}

def luma(v):
    return np.asarray(v,dtype=float)@np.array([.2126,.7152,.0722])

def width_center(row):
    minimum = row.argmin()
    half=(151+row[minimum])/2
    def cross(j):
        return j+(half-row[j])/(row[j+1]-row[j])
    left=cross(np.flatnonzero((row[:-1]>=half)&(row[1:]<half))[-1])
    right=cross(np.flatnonzero((row[:-1]<=half)&(row[1:]>half))[0])
    return right-left,(right+left)/2

results={'angles':{},'checks':{}}
for angle in (0,30,45,75):
    c,s=np.cos(np.deg2rad(angle)),np.sin(np.deg2rad(angle))
    results['angles'][angle]={}
    for mode,images in [('input',cases),*outputs.items()]:
        widths,centers,depths=[],[],[]
        for phase in range(16):
            a=luma(images[f'line_{angle}_{phase:02}'])
            for k in range(23,33):
                width,center=width_center(a[k])
                widths.append(c*width)
                centers.append((center-71.5)*c+(k-27.5)*s-phase/16)
                depths.append(151-a[k].min())
        results['angles'][angle][mode] = dict(mean_depth=np.mean(depths),mean_fwhm_px=np.mean(widths),
            max_center_error_px=np.max(np.abs(centers)),depth_sd=np.std(depths))

for name in cases:
    b=outputs['base'][name].astype(int)
    v=outputs['candidate'][name].astype(int)
    d=luma(b)-luma(v)
    assert not np.any(v>b),name
    if name.startswith('line_'):
        continue
    row=dict(mean_darker_codes=d.mean(),max_darker_codes=d.max(),pct_3_plus=100*(d>=3-1e-6).mean())
    if name=='constant_blue':
        row['max_blue_change']=int(abs(v[...,2]-b[...,2]).max())
    results['checks'][name]=row
    if name in ('astronaut','chelsea','astronaut_native','chelsea_native') or name in ('seam','text') or name.startswith('h_courtyard'):
        cases[name].save(args.out/(name+'-input.png'))
        for mode,images in outputs.items():
            Image.fromarray(images[name]).save(args.out/(name+'-'+mode+'.png'))
        ref=luma(Image.open(args.out/(name+'-reference.png')))
        row['reference_mae']={mode:float(np.mean(abs(luma(images[name])-ref))) for mode,images in outputs.items()}
for row in results['angles'].values():
    base, candidate = row['base'], row['candidate']
    assert candidate['mean_depth'] > 1.09*base['mean_depth'],row
    assert abs(candidate['mean_fwhm_px']-base['mean_fwhm_px']) < .25,row
    assert candidate['max_center_error_px'] < .12,row
for name in ('dot','black_line','step','broad_shadow','ringing','flat'):
    assert results['checks'][name]['max_darker_codes'] == 0,name
assert results['checks']['noise']['mean_darker_codes'] < .05
assert results['checks']['constant_blue']['max_blue_change'] == 0
# Temporal difference of the error to true rendered phases (fixed screen coordinates).
fg_names=sorted(n for n in cases if n.startswith('h_courtyard'))
if len(fg_names)>1:
    results['fg_temporal_rms']={}
    for mode,images in outputs.items():
        errors=[luma(images[n])-luma(Image.open(args.out/(n+'-reference.png'))) for n in fg_names]
        results['fg_temporal_rms'][mode]=np.mean([np.mean((e-p)**2)**.5 for e,p in zip(errors[1:],errors[:-1])])
(args.out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps({'angles':results['angles'],'controls':{n:results['checks'][n] for n in ('noise','dot','ringing','broad_shadow','constant_blue')}},indent=1))
