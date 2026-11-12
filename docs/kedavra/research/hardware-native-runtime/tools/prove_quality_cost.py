"""CPU algebra/contract checks only. Not image-quality or GPU timing evidence."""
import difflib
import hashlib
import json
import pathlib
import random
import argparse

ROOT = pathlib.Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-root', type=pathlib.Path, default=ROOT/'src')
parser.add_argument('--output-dir', type=pathlib.Path, default=ROOT/'quality-cost-proposals')
args = parser.parse_args()
OUT = args.output_dir
OUT.mkdir(exist_ok=True)
sources = args.source_root / 'kedavra/shaders'
for name, blob in [('kfg_select.comp', '2e721a758e2bec2c7a4789a19e5d095392dce130'),
                   ('kfg_warp.comp', '72b04f987f62a62006e304ce117afe5c7d7ec815')]:
    data = (sources / name).read_bytes()
    assert hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest() == blob

def smooth(e):
    x = max(0.0, min(1.0, (e-.10)/.10))
    return x*x*(3-2*x)

def repair_ratio(z, error, pair, changed, fixed, t=.5):
    if changed and not (fixed and pair):
        return z+(float(t>=.5)-z)*smooth(error)
    return z

def warp_ratio(z, error, t):
    a, b = (1-t)*(1-z), t*z
    weight = b/max(a+b, 1e-6)
    return weight+(float(t>=.5)-weight)*smooth(error)

rng = random.Random(20261025)
for _ in range(20000):
    z, e, t = rng.random(), rng.random()*.3, rng.random()
    for changed in [False, True]:
        assert repair_ratio(z,e,False,changed,True,t) == repair_ratio(z,e,False,changed,False,t)
        assert repair_ratio(z,e,True,changed,True) == z
    # Isolate the contract: mirrored visibility in the pair field must not acquire
    # an unconditional preference for B during repair. Not a full pipeline symmetry test.
    z1 = repair_ratio(z,e,True,True,True)
    z2 = repair_ratio(1-z,e,True,True,True)
    assert abs(z1+z2-1)<1e-12

select = (sources/'kfg_select.comp').read_text()
old = '''\tif (best.xy != nb[4].xy)
\t\tbest.z = mix(best.z, u_t < 0.5 ? 0.0 : 1.0, smoothstep(0.10, 0.20, best.w));
\telse
\t\tbest = nb[4];'''
new = '''\tif (best.xy != nb[4].xy) {
\t\t// A pair field stores phase-independent visibility; warp applies the lean.
\t\tif ((u_flags & KFG_FLAG_PAIR) == 0)
\t\t\tbest.z = mix(best.z, u_t < 0.5 ? 0.0 : 1.0, smoothstep(0.10, 0.20, best.w));
\t} else
\t\tbest = nb[4];'''
assert select.count(old)==1
fixed = select.replace(old,new)
(OUT/'kfg_select.comp').write_text(fixed)
(OUT/'pair-repair-visibility.patch').write_text(''.join(difflib.unified_diff(select.splitlines(True),fixed.splitlines(True),fromfile='a/kedavra/shaders/kfg_select.comp',tofile='b/kedavra/shaders/kfg_select.comp')))

def weights(f):
    return [f*(-.5+f*(1-.5*f)), 1+f*f*(-2.5+1.5*f), f*(.5+f*(2-1.5*f)), f*f*(-.5+.5*f)]

max_error = 0.0
for _ in range(20000):
    axis=rng.randrange(2)
    fx,fy=(0.0,rng.random()) if axis==0 else (rng.random(),0.0)
    x,y=weights(fx),weights(fy);x12=x[1]+x[2];y12=y[1]+y[2]
    w=[x12*y[0],x[0]*y12,x12*y12,x[3]*y12,x12*y[3]]
    taps=[rng.uniform(-2,4) for _ in range(5)]
    baseline=sum(a*b for a,b in zip(w,taps))/sum(w)
    indices=[0,2,4] if axis==0 else [1,2,3]
    candidate=sum(w[i]*taps[i] for i in indices)/sum(w[i] for i in indices)
    max_error=max(max_error,abs(baseline-candidate))
    assert abs(baseline-candidate)<1e-12

warp=(sources/'kfg_warp.comp').read_text()
anchor='\tvec4 r = textureLod(s_samplers[slot], vec2(p12.x, p0.y), 0.0) * (w12.x * w0.y)'
insert='''\t// Research variant: exact axis alignment eliminates zero-weight taps.
\t// The caller already bypasses this function when both coordinates are integral.
\tif (f.x == 0.0 || f.y == 0.0) {
\t\tvec2 lo = f.x == 0.0 ? vec2(p12.x, p0.y) : vec2(p0.x, p12.y);
\t\tvec2 hi = f.x == 0.0 ? vec2(p12.x, p3.y) : vec2(p3.x, p12.y);
\t\tfloat wl = f.x == 0.0 ? w0.y : w0.x;
\t\tfloat wm = f.x == 0.0 ? w12.y : w12.x;
\t\tfloat wh = f.x == 0.0 ? w3.y : w3.x;
\t\tvec4 axis = (textureLod(s_samplers[slot], lo, 0.0) * wl
\t\t           + textureLod(s_samplers[slot], p12, 0.0) * wm
\t\t           + textureLod(s_samplers[slot], hi, 0.0) * wh) / (wl + wm + wh);
\t\treturn (u_flags & KFG_FLAG_SCRGB) != 0 ? axis : max(axis, vec4(0.0));
\t}
'''
assert warp.count(anchor)==1
fixed=warp.replace(anchor,insert+anchor)
(OUT/'kfg_warp.comp').write_text(fixed)
(OUT/'axis-sharp-three-taps.patch').write_text(''.join(difflib.unified_diff(warp.splitlines(True),fixed.splitlines(True),fromfile='a/kedavra/shaders/kfg_warp.comp',tofile='b/kedavra/shaders/kfg_warp.comp')))
report={'base_commit':'5f22938b55f9853f764e927c847d9c1a8d9a8c04',
 'kind':'CPU algebra and isolated metadata contract only',
 'repair_cases':20000,'axis_filter_cases':20000,'axis_double_max_abs_error':max_error,
 'repair_example':{'input_z':.5,'error':.15,'old_repaired_z':repair_ratio(.5,.15,True,True,False),
 'new_repaired_z':.5,'phase_one_third_old_B_weight':warp_ratio(repair_ratio(.5,.15,True,True,False),.15,1/3),
 'phase_one_third_new_B_weight':warp_ratio(.5,.15,1/3)},
 'limits':['No GPU image comparison, no native performance measurement, no energy or input latency measurement.',
 'Axis algebra uses Python double; native float/FMA results need comparison.',
 'Repair model assumes a changed winner; image fixtures must establish occurrence and quality.',
 'Existing spatial visibility inheritance and error semantics are not fixed by this proposal.']}
(OUT/'proof.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
