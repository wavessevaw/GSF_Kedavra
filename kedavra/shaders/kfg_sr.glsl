// SPDX-License-Identifier: GPL-3.0-or-later
// Coordinates are source texels with pixel centres at n+0.5. The caller
// handles transfer decoding and the existing colour management exactly once.
// SR.3: 25 phase-specific zero-DC/zero-affine residual filters, trained on
// synthetic 5:4 downsampling with approximate CPU Focus. Phase order .9,.7,.5,.3,.1.
// Independent validation: 6 training, 5 validation, 3 held-out photos; two degradations.
// Ridge + 0.025 Laplacian-error weight. No learned runtime state, no extra pass.
const vec4 sr3Weights[100] = vec4[100](
    vec4(-0.009098, 0.029737, 0.069382, -0.037823), vec4(0.014690, 0.045086, -0.031929, -0.088242), vec4(0.024955, 0.028207, -0.028726, -0.104258), vec4(-0.026583, 0.006281, 0.015215, 0.093107),
    vec4(-0.015998, 0.054645, -0.006814, 0.044953), vec4(0.019234, 0.023917, -0.027081, -0.128587), vec4(0.013584, 0.031893, -0.031546, -0.112994), vec4(-0.013534, 0.016140, 0.021252, 0.110936),
    vec4(-0.020659, 0.082425, 0.043072, -0.005018), vec4(0.030559, -0.002370, -0.082120, -0.075125), vec4(0.020548, 0.009527, -0.040232, -0.070839), vec4(-0.007790, 0.007234, 0.035580, 0.075208),
    vec4(-0.017796, 0.064523, 0.066217, -0.038630), vec4(0.037283, 0.001888, -0.116294, -0.030087), vec4(0.005118, -0.056400, -0.022914, -0.005886), vec4(0.010726, -0.010598, 0.071161, 0.041688),
    vec4(0.015006, 0.043726, -0.042207, 0.048396), vec4(0.023331, -0.003141, -0.062898, -0.028698), vec4(-0.015873, -0.092624, -0.001423, 0.022182), vec4(0.015037, -0.021886, 0.077368, 0.023704),
    vec4(-0.015887, -0.050777, 0.106835, 0.052610), vec4(0.007935, 0.059830, -0.042451, -0.102061), vec4(0.022241, 0.002874, -0.017765, -0.140413), vec4(-0.014310, 0.004122, 0.023565, 0.103651),
    vec4(-0.069806, 0.026358, 0.067514, 0.075482), vec4(0.025582, 0.023281, -0.029708, -0.126722), vec4(0.028490, -0.012832, -0.030028, -0.124543), vec4(-0.012545, 0.014952, 0.025338, 0.119188),
    vec4(-0.068288, 0.050518, 0.130160, -0.002034), vec4(0.016618, -0.004010, -0.069588, -0.065172), vec4(0.024737, -0.046298, -0.024969, -0.061721), vec4(0.000538, 0.005346, 0.038292, 0.075870),
    vec4(-0.065608, 0.050980, 0.150288, -0.051969), vec4(0.005865, -0.001249, -0.083600, -0.014932), vec4(0.001200, -0.109641, -0.015098, 0.011456), vec4(0.029401, -0.008757, 0.065719, 0.035945),
    vec4(-0.064639, 0.088361, 0.061455, -0.012104), vec4(-0.009345, -0.021004, -0.023101, -0.001767), vec4(-0.022828, -0.130160, -0.006394, 0.045573), vec4(0.040067, -0.014588, 0.058014, 0.012459),
    vec4(-0.021818, 0.017013, 0.120274, -0.015265), vec4(0.015154, 0.041771, -0.035375, -0.061624), vec4(0.024875, -0.030232, -0.067337, -0.074777), vec4(-0.002353, 0.007398, 0.007296, 0.074998),
    vec4(-0.078388, 0.108206, 0.106916, -0.020526), vec4(0.012907, -0.016434, -0.019594, -0.059228), vec4(0.034264, -0.038631, -0.093010, -0.055719), vec4(0.001795, 0.036901, 0.009134, 0.071406),
    vec4(-0.052520, 0.105452, 0.125346, -0.076807), vec4(-0.023046, -0.021969, -0.034866, -0.009515), vec4(-0.000101, -0.051845, -0.058890, -0.009064), vec4(0.036194, 0.016133, 0.020901, 0.034596),
    vec4(0.001657, 0.091863, 0.097271, -0.081622), vec4(-0.059829, -0.007899, -0.025933, 0.000254), vec4(-0.048915, -0.071837, -0.038336, 0.026847), vec4(0.066268, 0.007131, 0.034458, 0.008620),
    vec4(-0.012632, 0.138415, 0.005802, -0.029760), vec4(-0.061998, -0.033296, 0.040733, 0.000501), vec4(-0.071875, -0.073173, -0.032073, 0.035811), vec4(0.070365, 0.007497, 0.014687, 0.000994),
    vec4(-0.003368, 0.079621, 0.081925, -0.071355), vec4(-0.009419, -0.028251, -0.016936, -0.007588), vec4(0.049851, -0.014763, -0.151443, -0.016868), vec4(0.009745, 0.070776, -0.005805, 0.033878),
    vec4(-0.045200, 0.137772, 0.051451, -0.065711), vec4(-0.013313, -0.078331, -0.003534, 0.009516), vec4(0.020566, -0.017290, -0.122659, 0.001779), vec4(0.033848, 0.070387, -0.007079, 0.027799),
    vec4(0.014613, 0.119924, 0.067077, -0.089020), vec4(-0.080537, -0.070716, -0.006655, 0.028424), vec4(-0.041297, -0.013602, -0.078242, 0.010386), vec4(0.075519, 0.055222, 0.006129, 0.002774),
    vec4(0.088258, 0.068826, 0.028915, -0.080931), vec4(-0.122209, -0.013848, 0.020771, 0.015946), vec4(-0.112192, -0.023369, -0.026141, 0.028467), vec4(0.106193, 0.023223, 0.010764, -0.012671),
    vec4(0.057627, 0.110366, -0.047207, -0.017863), vec4(-0.100115, -0.036212, 0.050002, 0.005913), vec4(-0.144740, -0.023040, -0.004513, 0.029458), vec4(0.107966, 0.023228, 0.007905, -0.018774),
    vec4(0.046051, -0.050280, 0.043520, 0.017171), vec4(-0.030332, -0.055512, 0.001789, 0.018150), vec4(0.022788, -0.003887, -0.085461, -0.017625), vec4(0.024442, 0.071402, -0.022817, 0.020598),
    vec4(-0.027899, 0.062828, 0.073225, -0.029124), vec4(-0.032897, -0.110743, 0.003102, 0.042045), vec4(-0.003490, -0.018004, -0.068034, -0.004855), vec4(0.040367, 0.073882, -0.013399, 0.012996),
    vec4(0.012406, 0.027898, 0.104239, -0.040272), vec4(-0.077458, -0.083262, -0.017652, 0.044749), vec4(-0.057176, -0.034858, -0.012865, 0.011125), vec4(0.073310, 0.046655, 0.010098, -0.006936),
    vec4(0.051100, -0.007231, 0.060616, -0.018206), vec4(-0.132336, -0.027319, 0.021536, 0.018741), vec4(-0.095670, -0.025892, 0.024575, 0.005361), vec4(0.103349, 0.017375, 0.012516, -0.008513),
    vec4(-0.043930, 0.066738, 0.041068, -0.006847), vec4(-0.081114, -0.029884, 0.040716, 0.009940), vec4(-0.104902, -0.026747, 0.036475, 0.018126), vec4(0.087950, 0.010979, 0.003959, -0.022525)
);

vec4 srTexel(sampler2D tex, ivec2 p, ivec2 hi)
{
    return texelFetch(tex, clamp(p, ivec2(0), hi), 0);
}

// Correction from linear interpolation to a cubic Hermite segment. Slopes
// are estimated along the row/column; no high-resolution history is needed.
vec3 srSlope(vec3 a, vec3 b, vec3 c, vec3 d, float t)
{
    return 0.5*t*(1.0-t)*((1.0-t)*(2.0*b-a-c) + t*(2.0*c-b-d));
}

vec4 kfgReconstruct(sampler2D tex, vec2 coord, ivec2 size, bool phase54)
{
    vec2 p = coord-0.5;
    ivec2 i = ivec2(floor(p)), hi = size-1;
    vec2 f = fract(p);
    const ivec2 X = ivec2(1,0), Y = ivec2(0,1);
    vec4 a = srTexel(tex,i,hi), b = srTexel(tex,i+X,hi);
    vec4 c = srTexel(tex,i+Y,hi), d = srTexel(tex,i+X+Y,hi);
    vec4 base = mix(mix(a,b,f.x),mix(c,d,f.x),f.y);
    vec3 lo = min(min(a.rgb,b.rgb),min(c.rgb,d.rgb));
    vec3 top = max(max(a.rgb,b.rgb),max(c.rgb,d.rgb));
    if (all(equal(lo,top)) || min(min(a.a,b.a),min(c.a,d.a)) < 0.999)
        return base;
    vec3 leftA=srTexel(tex,i-X,hi).rgb, rightB=srTexel(tex,i+2*X,hi).rgb;
    vec3 leftC=srTexel(tex,i+Y-X,hi).rgb, rightD=srTexel(tex,i+Y+2*X,hi).rgb;
    vec3 upA=srTexel(tex,i-Y,hi).rgb, downC=srTexel(tex,i+2*Y,hi).rgb;
    vec3 upB=srTexel(tex,i+X-Y,hi).rgb, downD=srTexel(tex,i+X+2*Y,hi).rgb;
    vec3 hx = mix(srSlope(leftA,a.rgb,b.rgb,rightB,f.x), srSlope(leftC,c.rgb,d.rgb,rightD,f.x),f.y);
    vec3 hy = mix(srSlope(upA,a.rgb,c.rgb,downC,f.y), srSlope(upB,b.rgb,d.rgb,downD,f.y),f.x);
    // A finite source pixel covers an area. Compensate its footprint using
    // the four local curvatures already in these registers. Neighbour bounds
    // and a contrast-dependent limit cap the response on thin silhouettes.
    vec3 inner = mix(mix(b.rgb+c.rgb,a.rgb+d.rgb,f.x),mix(a.rgb+d.rgb,b.rgb+c.rgb,f.x),f.y);
    vec3 outer = mix(mix(leftA+upA,rightB+upB,f.x),mix(leftC+downC,rightD+downD,f.x),f.y);
    vec3 curvature = 4.0*base.rgb-inner-outer;
    vec3 limit = 0.18*(top-lo);
    // On a diagonal boundary the four samples describe two triangles.
    // Prefer the triangulation whose shared edge crosses less colour change.
    vec3 weights = vec3(0.25,0.50,0.25);
    float ad = dot(abs(a.rgb-d.rgb),weights), bc = dot(abs(b.rgb-c.rgb),weights);
    float trust = abs(ad-bc)/(ad+bc+0.015);
    float joint = ad < bc ? min(f.x,f.y) : max(f.x+f.y-1.0,0.0);
    vec3 diagonal = (joint-f.x*f.y)*(a.rgb-b.rgb-c.rgb+d.rgb);
    // The original 12-tap path is retained for all other scale ratios.
    if (!phase54)
        return vec4(clamp(base.rgb + mix(hx+hy, diagonal, trust*trust) + clamp(0.18*curvature,-limit,limit),lo,top),base.a);

    // Low-cost phase-dependent residual on the existing stencil.
    // 5:4 coefficients fitted on synthetic photo downsampling only; experimental.
    vec3 cx = mix(mix(2.0*a.rgb-leftA-b.rgb,2.0*b.rgb-a.rgb-rightB,f.x),
                  mix(2.0*c.rgb-leftC-d.rgb,2.0*d.rgb-c.rgb-rightD,f.x),f.y);
    vec3 cy = mix(mix(2.0*a.rgb-upA-c.rgb,2.0*b.rgb-upB-d.rgb,f.x),
                  mix(2.0*c.rgb-a.rgb-downC,2.0*d.rgb-b.rgb-downD,f.x),f.y);
    vec2 phase = f*(1.0-f);
    vec3 residual = 0.022116*curvature + 0.699988*(phase.x*cx+phase.y*cy);
    vec3 bound = 0.168153*(top-lo);
    vec3 rgb = base.rgb + mix(0.342317*(hx+hy),diagonal,0.915944*trust*trust)
                        + clamp(residual,-bound,bound);
    vec3 sr2 = clamp(rgb,lo,top);
    // Four corners fill the original cross to a complete 4x4 stencil.
    vec3 tl=srTexel(tex,i-X-Y,hi).rgb, tr=srTexel(tex,i+2*X-Y,hi).rgb;
    vec3 bl=srTexel(tex,i-X+2*Y,hi).rgb, br=srTexel(tex,i+2*X+2*Y,hi).rgb;
    const vec3 luma = vec3(0.2126,0.7152,0.0722);
    ivec2 phaseId = clamp(ivec2(round((vec2(0.9)-f)*5.0)),ivec2(0),ivec2(4));
    int wi=4*(phaseId.y*5+phaseId.x);
    float detail = dot(sr3Weights[wi],vec4(dot(a.rgb,luma),dot(b.rgb,luma),dot(c.rgb,luma),dot(d.rgb,luma)))
                 + dot(sr3Weights[wi+1],vec4(dot(leftA,luma),dot(rightB,luma),dot(leftC,luma),dot(rightD,luma)))
                 + dot(sr3Weights[wi+2],vec4(dot(upA,luma),dot(downC,luma),dot(upB,luma),dot(downD,luma)))
                 + dot(sr3Weights[wi+3],vec4(dot(tl,luma),dot(tr,luma),dot(bl,luma),dot(br,luma)));
    vec3 lo16=min(lo,min(min(min(leftA,rightB),min(leftC,rightD)),min(min(upA,downC),min(upB,downD))));
    vec3 hi16=max(top,max(max(max(leftA,rightB),max(leftC,rightD)),max(max(upA,downC),max(upB,downD))));
    lo16=min(lo16,min(min(tl,tr),min(bl,br)));
    hi16=max(hi16,max(max(tl,tr),max(bl,br)));
    // Exact CPU-evaluated residual bound: expanded local extrema, maximum 0.06 code values.
    return vec4(clamp(sr2+vec3(clamp(detail,-0.06,0.06)),lo16,hi16),base.a);
}

// Standalone SR replay uses the candidate; production passes the actual scale.
vec4 kfgReconstruct(sampler2D tex, vec2 coord, ivec2 size)
{
    return kfgReconstruct(tex,coord,size,true);
}
