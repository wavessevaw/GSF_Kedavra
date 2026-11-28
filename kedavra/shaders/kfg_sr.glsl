// SPDX-License-Identifier: GPL-3.0-or-later
// Coordinates are source texels with pixel centres at n+0.5. The caller
// handles transfer decoding and the existing colour management exactly once.
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

vec4 kfgReconstruct(sampler2D tex, vec2 coord, ivec2 size)
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
    return vec4(clamp(base.rgb + mix(hx+hy, diagonal, trust*trust) + clamp(0.18*curvature,-limit,limit),lo,top),base.a);
}
