#!/usr/bin/env python3
"""Internal response configurations; only balanced is installed."""
import argparse, shutil, subprocess
from pathlib import Path
p=argparse.ArgumentParser()
for name in ['shader','spv','out']: p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--glslang',default='glslangValidator');a=p.parse_args()
s=a.shader.read_text()
needle='vec3 budget = min(vec3(0.06)'
assert s.count(needle)==1
for name,k in [('conservative',0.7),('balanced',1.0),('strong',1.3)]:
 d=a.out/name;d.mkdir(parents=True,exist_ok=True)
 for f in a.spv.glob('*.spv'):shutil.copyfile(f,d/f.name)
 shader=d/'kfg_post.comp';shader.write_text(s.replace(needle,f'delta *= {k};\n\t'+needle))
 subprocess.run([a.glslang,'-V','--target-env','vulkan1.1','-I'+str(a.shader.resolve().parent),str(shader),'-o',str(d/'kfg_post.spv')],check=True)
