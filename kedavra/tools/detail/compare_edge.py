import sys,json,subprocess,os
from pathlib import Path
import numpy as np
from PIL import Image,ImageFilter
sys.path.insert(0,str(Path(__file__).resolve().parent))
from compare import metrics
import argparse
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--baseline',type=Path,required=True);p.add_argument('--exe',required=True);p.add_argument('--spv',required=True);a=p.parse_args()
root=a.out.resolve();root.mkdir(parents=True,exist_ok=True);src=a.baseline.resolve();exe=a.exe;spv=a.spv
rows=[]; cases=[]
for p in sorted(src.glob('*/input.rgba')):
 if p.parent.name=='bench':continue
 im=Image.open(p.with_name('input.png')); name=p.parent.name;out=root/(name+'.rgba');rows.append(f'{im.width} {im.height} detail=0.5 {p} {out} 1');cases.append((name,p,out))
w,h=256,64
v=np.tile(np.clip(65+125*(np.arange(w)-120)/8,65,190),(h,1)).astype('uint8')
a=np.stack([v,v,np.zeros_like(v),np.full_like(v,255)],-1);a.tofile(root/'constant-blue.rgba')
rows.append(f'{w} {h} detail=1 {root}/constant-blue.rgba {root}/constant-blue-new.rgba 1')
(root/'batch').write_text('\n'.join(rows)+'\n')
env=dict(os.environ)
with (root/'run.log').open('w') as f:subprocess.run([exe,'--spv',spv,'--batch',str(root/'batch')],env=env,stdout=f,stderr=f,check=True)
result={}
for name,p,out in cases:
 s=np.array(Image.open(p.with_name('input.png'))); ref=np.array(Image.open(p.with_name('reference.png')));o=np.fromfile(out,np.uint8).reshape((*s.shape[:2],4))[...,:3];old=np.array(Image.open(p.with_name('detail.png')))
 result[name]={m:metrics(s,np.array(Image.open(p.with_name(m+'.png'))),ref) for m in ['off','cas','dls']}
 result[name].update({'old':metrics(s,old,ref),'new':metrics(s,o,ref)})
 if name in ['astronaut_soft','color_edge','noise_3','grass_soft']:
  Image.fromarray(np.concatenate([s,old,o,ref],1)).save(root/(name+'.png'))
new=np.fromfile(root/'constant-blue-new.rgba',np.uint8).reshape(a.shape)
result['constant_blue']={'changed_red_pixels':int(np.count_nonzero(new[...,0]!=a[...,0])),'blue_max':int(new[...,2].max())}
(root/'results.json').write_text(json.dumps(result,indent=2))
print('cases',len(cases),result['constant_blue'])
for name in ['astronaut_soft','color_edge','noise_3','grass_soft']:
 print(name,{m:{k:round(v[k],4) for k in ['psnr_db','overshoot_max_codes','contrast_std_ratio']} for m,v in result[name].items()})
