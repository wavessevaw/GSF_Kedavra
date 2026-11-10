#!/usr/bin/env python3
"""Build rejected Edge experiments outside the runtime shader directory.

These are evidence, not production algorithms. Use compare_edge.py on each
output SPV directory and probe_profiles.py for the halo negative control.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--shader', type=Path, required=True)
p.add_argument('--spv', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--glslang', default='glslangValidator')
a = p.parse_args()
source = a.shader.read_text()
needle = 'float gain = strength * coherence * coherence * smoothstep(0.025, 0.10, range);'
assert source.count(needle) == 1
profile = '''float jump = max(max(abs(le-lb), abs(le-lh)), max(abs(le-ld), abs(le-lf)));
    float steepness = jump / max(range, 1e-5);
    float soft = 1.0 - smoothstep(0.60, 0.95, steepness);
    vec2 monotonic = vec2(abs(lf-ld)/max(abs(lf-le)+abs(le-ld),1e-5),
                          abs(lh-lb)/max(abs(lh-le)+abs(le-lb),1e-5));
    float profileTrust = dot(monotonic*monotonic, direction);
    float gain = strength * coherence * coherence * (range/(range+0.04)) * soft * profileTrust;'''
outer = '''ivec2 axis = abs(gradient.x) >= abs(gradient.y) ? ivec2(1,0) : ivec2(0,1);
    float outerLo = dot(px(-2*axis), LUMA709), outerHi = dot(px(2*axis), LUMA709);
    float innerLo = axis.x != 0 ? ld : lb, innerHi = axis.x != 0 ? lf : lh;
    vec4 slopes = vec4(innerLo-outerLo, le-innerLo, innerHi-le, outerHi-innerHi);
    float variation = dot(abs(slopes), vec4(1));
    float monotonic = abs(outerHi-outerLo)/max(variation,1e-5);
    float sharpFraction = max(max(abs(slopes.x),abs(slopes.y)),max(abs(slopes.z),abs(slopes.w)))/max(variation,1e-5);
    float profileGate = smoothstep(0.98,1.0,monotonic)*(1.0-smoothstep(0.60,0.95,sharpFraction));
    float gain = strength * coherence * coherence * smoothstep(0.025, 0.10, range) * profileGate;'''
variants = {'profile3': profile, 'outer11': outer,
            'width11': outer.replace('smoothstep(0.60,0.95,sharpFraction)', 'smoothstep(0.28,0.42,sharpFraction)')}
for name, replacement in variants.items():
    root = a.out/name
    root.mkdir(parents=True, exist_ok=True)
    for shader in a.spv.glob('*.spv'):
        shutil.copyfile(shader, root/shader.name)
    shader = root/'kfg_post.comp'
    shader.write_text(source.replace(needle, replacement))
    subprocess.run([a.glslang, '-V', '-I'+str(a.shader.resolve().parent), str(shader),
                    '-o', str(root/'kfg_post.spv')], check=True)
