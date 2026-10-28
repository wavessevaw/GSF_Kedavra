// SPDX-License-Identifier: GPL-3.0-or-later
//
// KEDAVRA frame generation: shared declarations.
//
// The descriptor interface is a subset of Gamescope's compute layout
// (src/shaders/descriptor_set.h): binding 0 = uniform constants (scalar),
// binding 1 = storage target, binding 3 = sampled textures. The same SPIR-V
// therefore runs inside Gamescope and in the standalone Vulkan test harness.

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_scalar_block_layout : require

#define KFG_SAMPLER_SLOTS 16

layout(binding = 1) writeonly uniform image2D dst;
layout(binding = 3) uniform sampler2D s_samplers[KFG_SAMPLER_SLOTS];

// One constant block for every pass; unused fields are ignored.
layout(binding = 0, scalar) uniform kfg_params_t {
	ivec2 u_dstSize;   // dispatch domain (texels of dst)
	ivec2 u_srcSize;   // size of slot-0 input in texels
	ivec2 u_fullSize;  // full-resolution source size in pixels
	float u_t;         // generated phase in (0, 1)
	int u_level;       // pyramid level of this pass (texel = 2^level pixels)
	int u_radius;      // search radius in texels at this level
	int u_flags;
	float u_lambda;    // motion smoothness weight
	float u_cutThreshold;
	vec2 u_scale;      // warp: source pixels per analysis pixel
};

#define KFG_FLAG_COARSEST 1
#define KFG_FLAG_DEBUG_MARKER 2
#define KFG_FLAG_TEMPORAL 16
#define KFG_FLAG_GLOBAL 32
#define KFG_FLAG_REPAIR 64
#define KFG_FLAG_PQ 128
#define KFG_FLAG_SCRGB 256
#define KFG_FLAG_PAIR 512
#define KFG_FLAG_RESAMPLE 1024

// Pair statistics image (7x1): texel 0 = cut and match statistics; texels 1 to 4 = camera
// motion as two plane projective maps (homographies) over image coordinates centred on the
// image and divided by half its width: texels 1-2 forward A->B, 3-4 backward B->A, each
// (h0 - 1, h1, h2, h3), (h4 - 1, h5, h6, h7) of x' = (h0 x + h1 y + h2) / (h6 x + h7 y + 1),
// y' = (h3 x + h4 y + h5) / (h6 x + h7 y + 1). A turning camera moves the picture exactly
// so (the sides twice as fast as the centre at 90 degrees, along curves); a translation and
// a zoom are special cases. Stored relative to the identity so half floats keep the precision.
// Texels 5-6: the log of the forward map scaled to determinant 1 (l00, l01, l02, l10),
// (l11, l12, l20, l21), for the warp's map at phase t; zero when there is none.
#define KFG_GLOBAL_TEXEL ivec2(1, 0)
#define KFG_STATS_WIDTH 7

float kfg_luma(vec3 c)
{
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// SMPTE ST 2084 (PQ) encoding of linear light in units of 10000 nits.
float kfg_pq_encode(float y)
{
	const float m1 = 0.1593017578125, m2 = 78.84375;
	const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	float p = pow(clamp(y, 0.0, 1.0), m1);
	return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

// Luma of an HDR colour for motion analysis, 0..1 like SDR luma: PQ code value of the
// luminance, normalised so 1000 nits is 1.0 (brighter highlights clip; they carry little
// motion detail and would compress everything else). PQ input is already encoded and is
// weighted with BT.2020 coefficients; scRGB is linear BT.709 with 1.0 = 80 nits.
#define KFG_PQ_1000_NITS 0.7518
float kfg_hdr_luma(vec3 c, int flags)
{
	float pq;
	if ((flags & KFG_FLAG_SCRGB) != 0)
		pq = kfg_pq_encode(max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 0.0) * (80.0 / 10000.0));
	else
		pq = dot(c, vec3(0.2627, 0.6780, 0.0593));
	return clamp(pq * (1.0 / KFG_PQ_1000_NITS), 0.0, 1.0);
}

// Sample a texture at a texel-space position with the sampler's (linear) filter.
vec4 kfg_tap(int slot, vec2 texel_pos, ivec2 size)
{
	vec2 uv = (texel_pos + 0.5) / vec2(size);
	return textureLod(s_samplers[slot], uv, 0.0);
}

// Camera motion at a full-resolution pixel position: xy forward, zw backward, pixels.
vec2 kfg_homography_motion(vec4 a, vec4 b, vec2 u, float hx)
{
	float den = b.z * u.x + b.w * u.y + 1.0;
	vec2 m = vec2((a.x + 1.0) * u.x + a.y * u.y + a.z, a.w * u.x + (b.x + 1.0) * u.y + b.y);
	m = abs(den) > 0.05 ? m / den : u;   // a degenerate map (never fitted so) means no motion
	return (m - u) * hx;
}

vec4 kfg_global_at(int slot, vec2 pix)
{
	vec2 h = 0.5 * vec2(u_fullSize);
	vec2 u = (pix - h) / h.x;
	vec4 fa = texelFetch(s_samplers[slot], KFG_GLOBAL_TEXEL, 0);
	vec4 fb = texelFetch(s_samplers[slot], KFG_GLOBAL_TEXEL + ivec2(1, 0), 0);
	vec4 ba = texelFetch(s_samplers[slot], KFG_GLOBAL_TEXEL + ivec2(2, 0), 0);
	vec4 bb = texelFetch(s_samplers[slot], KFG_GLOBAL_TEXEL + ivec2(3, 0), 0);
	return vec4(kfg_homography_motion(fa, fb, u, h.x), kfg_homography_motion(ba, bb, u, h.x));
}
