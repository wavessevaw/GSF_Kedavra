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
    vec3 hx = mix(srSlope(srTexel(tex,i-X,hi).rgb,a.rgb,b.rgb,srTexel(tex,i+2*X,hi).rgb,f.x),
                  srSlope(srTexel(tex,i+Y-X,hi).rgb,c.rgb,d.rgb,srTexel(tex,i+Y+2*X,hi).rgb,f.x), f.y);
    vec3 hy = mix(srSlope(srTexel(tex,i-Y,hi).rgb,a.rgb,c.rgb,srTexel(tex,i+2*Y,hi).rgb,f.y),
                  srSlope(srTexel(tex,i+X-Y,hi).rgb,b.rgb,d.rgb,srTexel(tex,i+X+2*Y,hi).rgb,f.y), f.x);
    // On a diagonal boundary the four samples describe two triangles.
    // Prefer the triangulation whose shared edge crosses less colour change.
    vec3 weights = vec3(0.25,0.50,0.25);
    float ad = dot(abs(a.rgb-d.rgb),weights), bc = dot(abs(b.rgb-c.rgb),weights);
    float trust = abs(ad-bc)/(ad+bc+0.015);
    float joint = ad < bc ? min(f.x,f.y) : max(f.x+f.y-1.0,0.0);
    vec3 diagonal = (joint-f.x*f.y)*(a.rgb-b.rgb-c.rgb+d.rgb);
    return vec4(clamp(base.rgb + mix(hx+hy, diagonal, trust*trust),lo,top),base.a);
}
