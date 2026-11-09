// SPDX-License-Identifier: GPL-3.0-or-later
//
// KEDAVRA post-generation filters: configuration parser, pass plan and the shaders
// themselves on a real Vulkan device (lavapipe in CI, the Deck's GPU for timing).
//   * colour looks that work per pixel are compared with a CPU port of the same formulas;
//   * neighbourhood effects (sharpening, antialiasing, deband, clarity, cartoon) must leave a
//     flat image unchanged and act on edges the way they are meant to;
//   * --bench N times the presets the plugin offers at the given size.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kfg_post.h"
#include "kfg_smaa_tex.h"
#include "kfg_vk.h"

static int g_fail = 0;

#define CHECK(cond, ...)                                  \
	do {                                              \
		if (!(cond)) {                            \
			printf("FAIL: " __VA_ARGS__);     \
			printf("\n");                     \
			g_fail++;                         \
		}                                         \
	} while (0)

// ---- parser and plan ----------------------------------------------------------------------

static void test_parse(void)
{
	kfg_post_cfg c;
	char err[128];
	CHECK(kfg_post_parse("", &c, err, sizeof(err)) == 0 && !kfg_post_active(&c), "empty string is off");
	CHECK(kfg_post_parse(NULL, &c, err, sizeof(err)) == 0 && !kfg_post_active(&c), "NULL is off");
	CHECK(kfg_post_parse("vibrance,vignette,cas=0.5,smaa", &c, err, sizeof(err)) == 0, "vivid parses: %s", err);
	CHECK(c.grade == KFG_GRADE_VIBRANCE && c.finish == KFG_FINISH_VIGNETTE && c.sharpen == KFG_SHARPEN_CAS &&
	      fabsf(c.sharpness - 0.5f) < 1e-6f && c.aa == KFG_AA_SMAA, "vivid fields");
	CHECK(kfg_post_parse(" dls=0.5:0.2 , fxaa", &c, err, sizeof(err)) == 0 && c.sharpen == KFG_SHARPEN_DLS &&
	      fabsf(c.denoise - 0.2f) < 1e-6f && c.aa == KFG_AA_FXAA, "dls with denoise and spaces");
	CHECK(kfg_post_parse("dls=0.5", &c, err, sizeof(err)) == 0 && fabsf(c.denoise - 0.17f) < 1e-6f, "dls default denoise");
	CHECK(kfg_post_parse("vibrance,bloom", &c, err, sizeof(err)) == -1 && !kfg_post_active(&c), "unknown effect rejected");
	CHECK(kfg_post_parse("cas=1.5", &c, err, sizeof(err)) == -1, "sharpness above 1 rejected");
	CHECK(kfg_post_parse("cas=", &c, err, sizeof(err)) == -1, "empty sharpness rejected");
	CHECK(kfg_post_parse("cas=nan", &c, err, sizeof(err)) == -1, "nan sharpness rejected");
	CHECK(kfg_post_parse("detail=0.6", &c, err, sizeof(err)) == 0 && c.sharpen == KFG_SHARPEN_DETAIL &&
	      fabsf(c.sharpness - 0.6f) < 1e-6f, "detail intensity");
	CHECK(kfg_post_after_generation(&c), "detail runs after generation");
	kfg_post_cfg hdr = kfg_post_for_transfer(c, KFG_TRANSFER_PQ);
	CHECK(!kfg_post_active(&hdr), "detail bypasses PQ until validated");
	hdr = kfg_post_for_transfer(c, KFG_TRANSFER_SCRGB);
	CHECK(!kfg_post_active(&hdr), "scRGB bypass retains signed HDR values");
	CHECK(kfg_post_parse("detail=0", &c, err, sizeof(err)) == 0 && !kfg_post_active(&c), "detail zero is off");
	const char *invalid[] = { "detail=nan", "detail=inf", "detail=-0.1", "detail=1.1", "detail=", "detail=0.5junk", "detail=0.5:0.2" };
	for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++)
		CHECK(kfg_post_parse(invalid[i], &c, err, sizeof(err)) == -1 && !kfg_post_active(&c), "reject %s", invalid[i]);
	kfg_post_parse("vibrance,cas=0.5,fxaa,vignette", &c, err, sizeof(err));
	hdr = kfg_post_for_transfer(c, KFG_TRANSFER_PQ);
	CHECK(hdr.grade == 0 && hdr.sharpen == KFG_SHARPEN_CAS && hdr.aa == c.aa && hdr.finish == c.finish,
	      "legacy PQ effects preserved");
	char longs[600];
	memset(longs, 'a', sizeof(longs) - 1);
	longs[sizeof(longs) - 1] = 0;
	CHECK(kfg_post_parse(longs, &c, err, sizeof(err)) == -1, "overlong string rejected");
	// Every effect name parses alone.
	for (size_t i = 0; i < KFG_POST_EFFECT_COUNT; i++)
		CHECK(kfg_post_parse(KFG_POST_EFFECTS[i].name, &c, err, sizeof(err)) == 0 && kfg_post_active(&c), "effect %s", KFG_POST_EFFECTS[i].name);
}

static void test_plan(void)
{
	kfg_plan p;
	kfg_post_cfg c;
	char err[64];
	kfg_post_parse("", &c, err, sizeof(err));
	CHECK(kfg_plan_post(&p, 64, 64, &c, 0.0f) == -1 && p.n == 0, "off: no passes");

	kfg_post_parse("vibrance,technicolor2,cas=0.4,smaa,vignette,film_grain", &c, err, sizeof(err));
	int ring = kfg_plan_post(&p, 64, 48, &c, 0.25f);
	// grade, sharpen, smaa edge/weight/blend, finish
	CHECK(p.n == 6, "full chain has 6 passes, got %u", p.n);
	CHECK(p.pass[0].slots[0].role == KFG_ROLE_POST_IN, "first pass reads POST_IN");
	CHECK(p.pass[0].dst.role == KFG_ROLE_POST && p.pass[0].dst.ring == 0, "grade -> POST[0]");
	CHECK(p.pass[1].slots[0].ring == 0 && p.pass[1].dst.ring == 1, "sharpen POST[0] -> POST[1]");
	CHECK(p.pass[2].shader == KFG_SHADER_SMAA_EDGE && p.pass[2].slots[0].ring == 1, "smaa edge reads POST[1]");
	CHECK(p.pass[3].nearest_mask == (1u << 2), "search texture sampled nearest");
	CHECK(p.pass[4].slots[0].ring == 1 && p.pass[4].dst.ring == 0, "smaa blend POST[1] -> POST[0]");
	CHECK(p.pass[5].dst.ring == 1 && ring == 1, "finish -> POST[1], result ring 1");
	CHECK(fabsf(p.pass[5].params.t - 0.25f) < 1e-6f, "grain phase passed");
	for (uint32_t i = 0; i < p.n; i++) {
		// No pass may read the image it writes.
		for (uint32_t s = 0; s < p.pass[i].nslots; s++)
			CHECK(!(p.pass[i].slots[s].role == p.pass[i].dst.role && p.pass[i].slots[s].ring == p.pass[i].dst.ring),
			      "pass %u reads its own target", i);
		CHECK(p.pass[i].groups[0] == 8 && p.pass[i].groups[1] == 6, "pass %u covers 64x48", i);
	}
	kfg_post_parse("fxaa", &c, err, sizeof(err));
	CHECK(kfg_plan_post(&p, 64, 64, &c, 0.0f) == 0 && p.n == 1, "fxaa alone: one pass into POST[0]");
	kfg_post_parse("detail=0", &c, err, sizeof(err));
	CHECK(kfg_plan_post(&p, 67, 49, &c, 0.0f) == -1 && p.n == 0, "detail zero dispatches nothing");
	kfg_post_parse("cas=0.4,detail=0.6", &c, err, sizeof(err));
	CHECK(kfg_plan_post(&p, 67, 49, &c, 0.0f) == 0 && p.n == 1 && p.pass[0].params.flags == KFG_SHARPEN_DETAIL,
	      "detail replaces CAS, never stacks");
	CHECK(p.pass[0].groups[0] == 9 && p.pass[0].groups[1] == 7, "odd dimensions covered");
}

// ---- GPU resources ------------------------------------------------------------------------

typedef struct post_env {
	kfg_vk *vk;
	uint32_t w, h;
	kfg_vk_image in, post[2], edges, weights, area, search;
} post_env;

static const kfg_vk_image *resolve(void *user, kfg_image_ref r)
{
	post_env *e = user;
	switch (r.role) {
	case KFG_ROLE_POST_IN: return &e->in;
	case KFG_ROLE_POST: return &e->post[r.ring & 1];
	case KFG_ROLE_SMAA_EDGES: return &e->edges;
	case KFG_ROLE_SMAA_WEIGHTS: return &e->weights;
	case KFG_ROLE_SMAA_AREA: return &e->area;
	case KFG_ROLE_SMAA_SEARCH: return &e->search;
	default: return NULL;
	}
}

static int env_init(post_env *e, kfg_vk *vk, uint32_t w, uint32_t h)
{
	memset(e, 0, sizeof(*e));
	e->vk = vk;
	e->w = w;
	e->h = h;
	VkFormat rgba = kfg_vk_format(KFG_FORMAT_RGBA8);
	if (kfg_vk_image_create(vk, &e->in, rgba, w, h) || kfg_vk_image_create(vk, &e->post[0], rgba, w, h) ||
	    kfg_vk_image_create(vk, &e->post[1], rgba, w, h) || kfg_vk_image_create(vk, &e->edges, rgba, w, h) ||
	    kfg_vk_image_create(vk, &e->weights, rgba, w, h) ||
	    kfg_vk_image_create(vk, &e->area, rgba, KFG_SMAA_AREA_W, KFG_SMAA_AREA_H) ||
	    kfg_vk_image_create(vk, &e->search, kfg_vk_format(KFG_FORMAT_R8), KFG_SMAA_SEARCH_W, KFG_SMAA_SEARCH_H))
		return -1;
	uint8_t *area = malloc((size_t)KFG_SMAA_AREA_W * KFG_SMAA_AREA_H * 4);
	kfg_smaa_area_rgba8(area);
	int r = kfg_vk_upload_raw(vk, &e->area, area, (size_t)KFG_SMAA_AREA_W * KFG_SMAA_AREA_H * 4);
	free(area);
	return r || kfg_vk_upload_raw(vk, &e->search, kfg_smaa_search_r8(), (size_t)KFG_SMAA_SEARCH_W * KFG_SMAA_SEARCH_H);
}

static void env_destroy(post_env *e)
{
	kfg_vk_image *imgs[] = { &e->in, &e->post[0], &e->post[1], &e->edges, &e->weights, &e->area, &e->search };
	for (size_t i = 0; i < sizeof(imgs) / sizeof(imgs[0]); i++)
		kfg_vk_image_destroy(e->vk, imgs[i]);
}

// Runs spec on img (RGBA8, w*h) in place. Returns -1 on error; *times gets per-pass ns.
static int run_post(post_env *e, const char *spec, float phase, uint8_t *img, double *total_ns)
{
	kfg_post_cfg c;
	char err[128];
	if (kfg_post_parse(spec, &c, err, sizeof(err))) {
		printf("bad spec %s: %s\n", spec, err);
		return -1;
	}
	kfg_plan p;
	int ring = kfg_plan_post(&p, e->w, e->h, &c, phase);
	if (ring < 0) {
		if (total_ns) *total_ns = 0;
		return 0;
	}
	if (kfg_vk_upload_rgba8(e->vk, &e->in, img))
		return -1;
	kfg_vk_begin(e->vk);
	if (kfg_vk_record(e->vk, &p, resolve, e) || kfg_vk_submit_wait(e->vk))
		return -1;
	if (total_ns) {
		double t[KFG_MAX_PASSES];
		uint32_t n = kfg_vk_pass_times(e->vk, t, KFG_MAX_PASSES);
		*total_ns = 0;
		for (uint32_t i = 0; i < n; i++)
			*total_ns += t[i];
	}
	return kfg_vk_download_rgba8(e->vk, &e->post[ring], img);
}

// ---- CPU references (same formulas as kfg_post.comp) --------------------------------------

typedef struct { float r, g, b; } v3;

static v3 mk(float r, float g, float b) { v3 v = { r, g, b }; return v; }
static float sat(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static v3 sat3(v3 c) { return mk(sat(c.r), sat(c.g), sat(c.b)); }
static float dot3(v3 a, v3 b) { return a.r * b.r + a.g * b.g + a.b * b.b; }
static v3 add(v3 a, v3 b) { return mk(a.r + b.r, a.g + b.g, a.b + b.b); }
static v3 sub(v3 a, v3 b) { return mk(a.r - b.r, a.g - b.g, a.b - b.b); }
static v3 mul(v3 a, v3 b) { return mk(a.r * b.r, a.g * b.g, a.b * b.b); }
static v3 scl(v3 a, float s) { return mk(a.r * s, a.g * s, a.b * s); }
static v3 spl(float s) { return mk(s, s, s); }
static v3 mixv(v3 a, v3 b, float t) { return add(a, scl(sub(b, a), t)); }
static float maxc(v3 c) { return fmaxf(c.r, fmaxf(c.g, c.b)); }
static float minc(v3 c) { return fminf(c.r, fminf(c.g, c.b)); }
static const v3 LUMA709 = { 0.2126f, 0.7152f, 0.0722f };

static v3 ref_levels_plus(v3 c)
{
	return sat3(scl(sub(c, spl(0.02f)), 1.0f / 0.96f));
}

static v3 ref_curves(v3 c)
{
	float l = dot3(LUMA709, c);
	v3 chroma = sub(c, spl(l));
	float x = l * (l * (1.5f - l) + 0.5f);
	x = l + (x - l) * 1.3f;
	return add(spl(x), chroma);
}

static v3 ref_vibrance(v3 c)
{
	float l = dot3(mk(0.212656f, 0.715158f, 0.072186f), c);
	float s = maxc(c) - minc(c);
	return mixv(spl(l), c, 1.0f + 0.15f * (1.0f - s));
}

static v3 ref_colourfulness(v3 c0)
{
	const v3 coeff = { 0.2558f, 0.6511f, 0.0931f };
	float luma = sqrtf(dot3(sat3(mul(c0, mk(fabsf(c0.r), fabsf(c0.g), fabsf(c0.b)))), coeff));
	c0 = sat3(c0);
	v3 dl = sub(c0, spl(luma));
	v3 cd = sub(scl(dl, 1.4f), dl);
	v3 t = add(scl(cd, 1.2f), c0);
	v3 rlc = sub(mk(fminf(fmaxf(t.r, -0.0001f), 1.0001f), fminf(fmaxf(t.g, -0.0001f), 1.0001f), fminf(fmaxf(t.b, -0.0001f), 1.0001f)), c0);
	float poslim = (1.0002f - luma) / (fabsf(maxc(dl)) + 0.0001f);
	float neglim = (luma + 0.0002f) / (fabsf(minc(dl)) + 0.0001f);
	v3 dmax = sub(scl(dl, fminf(fminf(poslim, neglim), 32.0f)), dl);
	float *pc = &cd.r;
	const float *pm = &dmax.r, *pr = &rlc.r;
	for (int i = 0; i < 3; i++) {
		float s = 0.7f * sqrtf(fabsf(pm[i])) + 0.3f * sqrtf(fabsf(pr[i]));
		s = fmaxf(s * s, 1e-7f);
		pc[i] = pc[i] * s / sqrtf(s * s + pc[i] * pc[i]);
	}
	return add(c0, cd);
}

static v3 ref_technicolor2(v3 color)
{
	color = sat3(color);
	v3 temp = sub(spl(1), color);
	v3 target = mk(temp.g, temp.r, temp.g), target2 = mk(temp.b, temp.b, temp.r);
	v3 temp2 = mul(mul(color, target), target2);
	temp = scl(temp2, 0.2f);
	target = mk(temp.g, temp.r, temp.g);
	target2 = mk(temp.b, temp.b, temp.r);
	temp = add(sub(color, target), temp2);
	return sub(temp, target2);  // strength 1, saturation 1
}

static v3 ref_dpx(v3 in)
{
	static const float RGBm[3][3] = { { 2.6714711726599600f, -1.2672360578624100f, -0.4109956021722270f },
					  { -1.0251070293466400f, 1.9840911624108900f, 0.0439502493584124f },
					  { 0.0610009456429445f, -0.2236707508128630f, 1.1590210416706100f } };
	static const float XYZm[3][3] = { { 0.5003033835433160f, 0.3380975732227390f, 0.1645897795458570f },
					  { 0.2579688942747580f, 0.6761952591447060f, 0.0658358459823868f },
					  { 0.0234517888692628f, 0.1126992737203000f, 0.8668396731242010f } };
	const float cc[3] = { 0.36f, 0.36f, 0.34f };
	float b[3] = { in.r * 0.9f + 0.05f, in.g * 0.9f + 0.05f, in.b * 0.9f + 0.05f };
	float bt = 1.0f / (1.0f + expf(4.0f));
	for (int i = 0; i < 3; i++)
		b[i] = (1.0f / (1.0f + expf(-8.0f * (b[i] - cc[i])))) / (-2.0f * bt + 1.0f) + (-bt / (-2.0f * bt + 1.0f));
	float value = fmaxf(b[0], fmaxf(b[1], b[2]));
	float col[3], c0[3], c1[3];
	for (int i = 0; i < 3; i++)
		col[i] = powf(fabsf(b[i] / value), 1.0f / 2.5f) * value;
	for (int i = 0; i < 3; i++)
		c0[i] = XYZm[i][0] * col[0] + XYZm[i][1] * col[1] + XYZm[i][2] * col[2];
	float luma = c0[0] * 0.30f + c0[1] * 0.59f + c0[2] * 0.11f;
	for (int i = 0; i < 3; i++)
		c0[i] = -2.0f * luma + 3.0f * c0[i];
	for (int i = 0; i < 3; i++)
		c1[i] = RGBm[i][0] * c0[0] + RGBm[i][1] * c0[1] + RGBm[i][2] * c0[2];
	return mixv(in, mk(c1[0], c1[1], c1[2]), 0.2f);
}

static v3 ref_technicolor(v3 c)
{
	float k = 1.0f / (0.88f * 4.0f);
	float r = 1.05f * c.r * k + 0.62f * c.g * k;
	float g = 0.30f * c.r * k + 1.0f * c.g * k;
	float b = 1.0f * c.r * k + 1.05f * c.b * k;
	v3 R = add(spl(r), mk(0, 1.3f, 1)), G = add(spl(g), mk(1, 0, 1.05f)), B = add(spl(b), mk(1.6f, 1.6f, 0.05f));
	return mixv(c, mul(mul(R, G), B), 0.4f);
}

static v3 ref_bleach_bypass(v3 c)
{
	float l = dot3(c, LUMA709);
	v3 m = mixv(spl(l), c, 0.38f);
	v3 ct = mul(mul(m, m), sub(spl(3), scl(m, 2)));
	return mixv(c, sat3(mixv(m, ct, 0.68f)), 0.72f);
}

static v3 ref_sepia(v3 c) { return mixv(c, scl(mul(c, mk(0.55f, 0.43f, 0.42f)), 2.55f), 0.58f); }

static v3 ref_monochrome(v3 c) { return spl(dot3(mk(0.21f, 0.72f, 0.07f), c)); }

static v3 ref_noir(v3 c, float u, float v)
{
	float l = dot3(c, LUMA709);
	float m = sat((l - 0.5f) * 1.32f + 0.5f);
	float x = u * 2 - 1, y = v * 2 - 1;
	float vig = sat(1.0f - (x * x + y * y) * 0.18f);
	return sat3(scl(mk(1.02f, 1.0f, 0.96f), m * vig));
}

static float s2l(float c) { return c > 0.04045f ? powf((c + 0.055f) / 1.055f, 2.4f) : c / 12.92f; }
static float l2s(float c) { c = fmaxf(c, 0); return c > 0.0031308f ? 1.055f * powf(c, 1 / 2.4f) - 0.055f : c * 12.92f; }

static v3 ref_nostalgia(v3 c, float v, uint32_t h)
{
	static const float pal[16][3] = { { 0, 0, 0 }, { 255, 255, 255 }, { 136, 0, 0 }, { 170, 255, 238 }, { 204, 68, 204 },
					  { 0, 204, 85 }, { 0, 0, 170 }, { 238, 238, 119 }, { 221, 136, 85 }, { 102, 68, 0 },
					  { 255, 119, 119 }, { 51, 51, 51 }, { 119, 119, 119 }, { 170, 255, 102 }, { 0, 136, 255 },
					  { 187, 187, 187 } };
	v3 lin = mk(s2l(sat(c.r)), s2l(sat(c.g)), s2l(sat(c.b)));
	int best = 0;
	float bd = 1e9f;
	for (int i = 0; i < 16; i++) {
		v3 d = sub(lin, mk(pal[i][0] / 255, pal[i][1] / 255, pal[i][2] / 255));
		float dd = dot3(d, d);
		if (dd < bd) {
			bd = dd;
			best = i;
		}
	}
	float scan = v * (float)h * 0.5f;
	scan = scan - floorf(scan) + 0.5f;
	return mk(l2s(pal[best][0] / 255 * scan), l2s(pal[best][1] / 255 * scan), l2s(pal[best][2] / 255 * scan));
}

static v3 ref_hdr_flat(v3 c)
{
	// Flat image: all ring samples equal c.
	v3 bloom = scl(c, 0.005f * 8.0f);
	v3 hdr = scl(add(c, bloom), 0.87f - 0.793f);
	v3 bl = add(hdr, c);
	return add(mk(powf(fabsf(bl.r), 1.3f), powf(fabsf(bl.g), 1.3f), powf(fabsf(bl.b), 1.3f)), hdr);
}

// ---- GPU tests ----------------------------------------------------------------------------

static uint8_t q8(float x) { return (uint8_t)lrintf(sat(x) * 255.0f); }

// A smooth image covering many colours (no hard edges, so neighbourhood terms stay small).
static void fill_ramp(uint8_t *img, uint32_t w, uint32_t h)
{
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
			uint8_t *p = img + 4 * ((size_t)y * w + x);
			p[0] = q8(u);
			p[1] = q8(v);
			p[2] = q8(0.5f + 0.5f * sinf(6.2831853f * (u + v)));
			p[3] = 255;
		}
}

static void fill_flat(uint8_t *img, uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b)
{
	for (size_t i = 0; i < (size_t)w * h; i++) {
		img[4 * i + 0] = r;
		img[4 * i + 1] = g;
		img[4 * i + 2] = b;
		img[4 * i + 3] = 255;
	}
}

typedef v3 (*ref_fn)(v3 c, float u, float v, uint32_t h);

static v3 r_levels(v3 c, float u, float v, uint32_t h) { (void)u; (void)v; (void)h; return ref_levels_plus(c); }
static v3 r_curves(v3 c, float u, float v, uint32_t h) { (void)u; (void)v; (void)h; return ref_curves(c); }
static v3 r_vibrance(v3 c, float u, float v, uint32_t h) { (void)u; (void)v; (void)h; return ref_vibrance(c); }
static v3 r_colourful(v3 c, float u, float v, uint32_t h) { (void)u; (void)v; (void)h; return ref_colourfulness(c); }
static v3 r_tech2(v3 c, float u, float v, uint32_t h) { (void)u; (void)v; (void)h; return ref_technicolor2(c); }
static v3 r_dpx(v3 c, float u, float v, uint32_t h) { (void)u; (void)v; (void)h; return ref_dpx(c); }
static v3 r_tech(v3 c, float u, float v, uint32_t h) { (void)u; (void)v; (void)h; return ref_technicolor(c); }
static v3 r_bleach(v3 c, float u, float v, uint32_t h) { (void)u; (void)v; (void)h; return ref_bleach_bypass(c); }
static v3 r_sepia(v3 c, float u, float v, uint32_t h) { (void)u; (void)v; (void)h; return ref_sepia(c); }
static v3 r_mono(v3 c, float u, float v, uint32_t h) { (void)u; (void)v; (void)h; return ref_monochrome(c); }
static v3 r_noir(v3 c, float u, float v, uint32_t h) { (void)h; return ref_noir(c, u, v); }
static v3 r_nostalgia(v3 c, float u, float v, uint32_t h) { (void)u; return ref_nostalgia(c, v, h); }

static void test_pointwise(post_env *e, const char *name, ref_fn ref, float tol)
{
	size_t n = (size_t)e->w * e->h;
	uint8_t *src = malloc(n * 4), *img = malloc(n * 4);
	fill_ramp(src, e->w, e->h);
	memcpy(img, src, n * 4);
	if (run_post(e, name, 0, img, NULL)) {
		CHECK(0, "%s: run failed", name);
		free(src);
		free(img);
		return;
	}
	float worst = 0;
	size_t bad = 0;
	for (uint32_t y = 0; y < e->h; y++)
		for (uint32_t x = 0; x < e->w; x++) {
			const uint8_t *s = src + 4 * ((size_t)y * e->w + x), *g = img + 4 * ((size_t)y * e->w + x);
			v3 c = mk(s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f);
			v3 r = sat3(ref(c, (x + 0.5f) / e->w, (y + 0.5f) / e->h, e->h));
			float d = fmaxf(fabsf(r.r * 255 - g[0]), fmaxf(fabsf(r.g * 255 - g[1]), fabsf(r.b * 255 - g[2])));
			worst = fmaxf(worst, d);
			bad += d > tol;
			if (g[3] != s[3])
				bad++;
		}
	// Nostalgia snaps to a palette: a colour almost equidistant from two entries may round
	// either way on the GPU; allow a handful of such pixels.
	size_t allowed = !strcmp(name, "nostalgia") ? n / 500 : 0;
	printf("  %-14s max |gpu - cpu| = %.2f/255, %zu px over %.1f\n", name, worst, bad, tol);
	CHECK(bad <= allowed, "%s: %zu pixels differ from the CPU reference by more than %.1f/255", name, bad, tol);
	free(src);
	free(img);
}

// Flat image in, flat image out (within tol), for effects that only react to detail.
static void test_flat_identity(post_env *e, const char *spec, float tol)
{
	size_t n = (size_t)e->w * e->h;
	uint8_t *img = malloc(n * 4);
	fill_flat(img, e->w, e->h, 90, 140, 200);
	if (run_post(e, spec, 0.3f, img, NULL)) {
		CHECK(0, "%s: run failed", spec);
		free(img);
		return;
	}
	int worst = 0;
	for (size_t i = 0; i < n; i++) {
		worst = abs(img[4 * i] - 90) > worst ? abs(img[4 * i] - 90) : worst;
		worst = abs(img[4 * i + 1] - 140) > worst ? abs(img[4 * i + 1] - 140) : worst;
		worst = abs(img[4 * i + 2] - 200) > worst ? abs(img[4 * i + 2] - 200) : worst;
	}
	printf("  %-22s flat image: max change %d/255\n", spec, worst);
	CHECK(worst <= tol, "%s changes a flat image by %d/255", spec, worst);
	free(img);
}

static void test_hdr_flat(post_env *e)
{
	size_t n = (size_t)e->w * e->h;
	uint8_t *img = malloc(n * 4);
	fill_flat(img, e->w, e->h, 90, 140, 200);
	run_post(e, "hdr_look", 0, img, NULL);
	v3 r = sat3(ref_hdr_flat(mk(90 / 255.0f, 140 / 255.0f, 200 / 255.0f)));
	float d = fmaxf(fabsf(r.r * 255 - img[0]), fmaxf(fabsf(r.g * 255 - img[1]), fabsf(r.b * 255 - img[2])));
	printf("  %-14s flat: gpu (%d,%d,%d) cpu (%.1f,%.1f,%.1f)\n", "hdr_look", img[0], img[1], img[2], r.r * 255, r.g * 255, r.b * 255);
	CHECK(d <= 1.0f, "hdr_look flat differs by %.2f", d);
	free(img);
}

// Hard diagonal edge (1 px stairs): antialiasing must create intermediate values along it
// and leave pixels far from the edge alone.
static void test_aa(post_env *e, const char *spec)
{
	uint32_t w = e->w, h = e->h;
	size_t n = (size_t)w * h;
	uint8_t *img = malloc(n * 4), *src = calloc(n, 4);
	size_t mid_src = 0, mid_out = 0, far_changed = 0;
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			// Shallow slope (1 in 5): long stairs, the case SMAA and FXAA are built for.
			uint8_t v = (float)y > 0.2f * (float)x + 0.3f * (float)h ? 230 : 20;
			uint8_t *p = src + 4 * ((size_t)y * w + x);
			p[0] = p[1] = p[2] = v;
			p[3] = 255;
		}
	memcpy(img, src, n * 4);
	if (run_post(e, spec, 0, img, NULL)) {
		CHECK(0, "%s: run failed", spec);
		goto out;
	}
	for (uint32_t y = 2; y + 2 < h; y++)
		for (uint32_t x = 2; x + 2 < w; x++) {
			const uint8_t *s = src + 4 * ((size_t)y * w + x), *o = img + 4 * ((size_t)y * w + x);
			mid_src += s[1] > 30 && s[1] < 220;
			mid_out += o[1] > 30 && o[1] < 220;
			float edge_y = 0.2f * (float)x + 0.3f * (float)h;
			if (fabsf((float)y - edge_y) > 4 && o[1] != s[1])
				far_changed++;
		}
	printf("  %-6s stairs: %zu intermediate pixels (source %zu), %zu changed away from the edge\n", spec, mid_out, mid_src, far_changed);
	// 1-in-5 stairs: w / 5 steps; FXAA blends a couple of pixels at the good end of each.
	CHECK(mid_out > (size_t)w / 4, "%s does not smooth a stair edge (%zu intermediate pixels)", spec, mid_out);
	CHECK(far_changed == 0, "%s changes %zu pixels away from the edge", spec, far_changed);
out:
	free(img);
	free(src);
}

// Vertical step edge: sharpening must increase local contrast next to it.
static void test_sharpen(post_env *e, const char *spec)
{
	uint32_t w = e->w, h = e->h;
	size_t n = (size_t)w * h;
	uint8_t *img = malloc(n * 4);
	// Soft step (a few pixels wide) at the centre column.
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			float t = sat(((float)x - w / 2.0f) / 4.0f + 0.5f);
			uint8_t *p = img + 4 * ((size_t)y * w + x);
			p[0] = p[1] = p[2] = q8(0.3f + 0.4f * t);
			p[3] = 255;
		}
	uint8_t before_lo = img[4 * ((size_t)(h / 2) * w + w / 2 - 2) + 1], before_hi = img[4 * ((size_t)(h / 2) * w + w / 2 + 2) + 1];
	if (run_post(e, spec, 0, img, NULL)) {
		CHECK(0, "%s: run failed", spec);
		free(img);
		return;
	}
	uint8_t lo = img[4 * ((size_t)(h / 2) * w + w / 2 - 2) + 1], hi = img[4 * ((size_t)(h / 2) * w + w / 2 + 2) + 1];
	printf("  %-12s step: %d..%d -> %d..%d\n", spec, before_lo, before_hi, lo, hi);
	CHECK(hi - lo > before_hi - before_lo, "%s does not sharpen a soft edge", spec);
	free(img);
}

static void test_detail(post_env *e)
{
	size_t n = (size_t)e->w * e->h;
	uint8_t *src = malloc(n*4), *out = malloc(n*4);
	const uint8_t profile[] = {65, 65, 80, 110, 145, 175, 190, 190};
	for (uint32_t y = 0; y < e->h; y++)
		for (uint32_t x = 0; x < e->w; x++) {
			int j = (int)x - (int)e->w/2 + 4;
			uint8_t v = profile[j < 0 ? 0 : j > 7 ? 7 : j];
			size_t i = (size_t)y*e->w+x;
			src[4*i] = src[4*i+1] = src[4*i+2] = v; src[4*i+3] = (uint8_t)(i%256);
		}
	memcpy(out, src, n*4);
	CHECK(!run_post(e, "detail=0", 0, out, NULL) && !memcmp(src, out, n*4), "detail=0 byte-exact RGBA bypass");
	CHECK(!run_post(e, "detail=1", 0, out, NULL), "detail dispatch");
	size_t m = (size_t)(e->h/2)*e->w + e->w/2;
	CHECK(out[4*m]-out[4*(m-1)] > src[4*m]-src[4*(m-1)], "detail steepens soft edge interior");
	for (size_t i=0; i<n; i++) {
		CHECK(out[4*i] >= 65 && out[4*i] <= 190, "detail adds halo at %zu", i);
		CHECK(out[4*i+3] == src[4*i+3], "detail changes alpha at %zu", i);
	}
	// Already hard boundaries must not acquire ringing. Flat black/white must stay finite.
	for (size_t i=0; i<n; i++)
		src[4*i] = src[4*i+1] = src[4*i+2] = i % e->w < e->w/2 ? 0 : 255;
	memcpy(out, src, n*4);
	CHECK(!run_post(e, "detail=1", 0, out, NULL) && !memcmp(src, out, n*4), "hard black/white edge unchanged");
	free(src); free(out);
}

static void test_finish(post_env *e)
{
	uint32_t w = e->w, h = e->h;
	size_t n = (size_t)w * h;
	uint8_t *img = malloc(n * 4), *img2 = malloc(n * 4);
	fill_flat(img, w, h, 160, 160, 160);
	run_post(e, "vignette", 0, img, NULL);
	int centre = img[4 * ((size_t)(h / 2) * w + w / 2)], corner = img[0];
	printf("  vignette: centre %d, corner %d (input 160)\n", centre, corner);
	CHECK(abs(centre - 160) <= 1 && corner < centre - 20, "vignette: centre kept, corners darker");

	// Dark grey: with signal-to-noise 6 the grain is strongest in the shadows.
	fill_flat(img, w, h, 40, 40, 40);
	fill_flat(img2, w, h, 40, 40, 40);
	run_post(e, "film_grain", 0.1f, img, NULL);
	run_post(e, "film_grain", 0.6f, img2, NULL);
	double mean = 0;
	size_t diff = 0;
	for (size_t i = 0; i < n; i++) {
		mean += img[4 * i];
		diff += img[4 * i] != img2[4 * i];
	}
	mean /= (double)n;
	printf("  film_grain: mean %.1f (input 40), %.0f%% of pixels differ between two phases\n", mean, 100.0 * diff / n);
	CHECK(fabs(mean - 40) < 3, "film grain shifts the mean brightness to %.1f", mean);
	CHECK(diff > n / 2, "film grain does not move between frames");
	free(img);
	free(img2);
}

// Presets the plugin offers (GFG Extreme filter presets).
static const struct { const char *name, *spec; } PRESETS[] = {
	{ "sharp", "cas=0.5" },
	{ "vivid", "vibrance,cas=0.35" },
	{ "hdr", "hdr_look,cas=0.3" },
	{ "cinema", "technicolor2,vignette" },
	{ "noir", "noir,film_grain" },
	{ "retro", "nostalgia,vignette" },
	{ "smooth", "cas=0.3,smaa" },
	{ "everything", "deband,hdr_look,clarity,cartoon,levels_plus,curves,vibrance,colourfulness,technicolor2,dpx,technicolor,"
			"bleach_bypass,sepia,noir,monochrome,nostalgia,dls=0.5:0.17,smaa,chromatic_aberration,vignette,film_grain" },
};

#include "kfg_post_batch.h"

int main(int argc, char **argv)
{
	const char *spv = "build";
	const char *batch = NULL;
	int w = 320, h = 200, bench = 0, bw = 1280, bh = 800;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--spv") && i + 1 < argc) spv = argv[++i];
		else if (!strcmp(argv[i], "--batch") && i + 1 < argc) batch = argv[++i];
		else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--bench-size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &bw, &bh);
		else {
			fprintf(stderr, "usage: kfg_posttest [--spv DIR] [--bench N] [--bench-size WxH]\n");
			return 2;
		}
	}
	printf("parser and plan\n");
	test_parse();
	test_plan();

	kfg_vk vk;
	if (kfg_vk_init(&vk, spv, getenv("KFG_VALIDATION") != NULL)) {
		fprintf(stderr, "Vulkan init failed\n");
		return 1;
	}
	printf("device: %s (%s)\n", vk.device_name, vk.driver_info);
	if (batch) {
		int status = post_batch(&vk, batch);
		kfg_vk_destroy(&vk);
		return status || g_fail;
	}
	post_env e;
	if (env_init(&e, &vk, (uint32_t)w, (uint32_t)h)) {
		fprintf(stderr, "post images failed\n");
		return 1;
	}
	printf("colour looks against the CPU reference\n");
	const struct { const char *name; ref_fn fn; float tol; } PW[] = {
		{ "levels_plus", r_levels, 1.0f }, { "curves", r_curves, 1.0f }, { "vibrance", r_vibrance, 1.0f },
		{ "colourfulness", r_colourful, 1.0f }, { "technicolor2", r_tech2, 1.0f }, { "dpx", r_dpx, 1.0f },
		{ "technicolor", r_tech, 1.0f }, { "bleach_bypass", r_bleach, 1.0f }, { "sepia", r_sepia, 1.0f },
		{ "monochrome", r_mono, 1.0f }, { "noir", r_noir, 1.0f }, { "nostalgia", r_nostalgia, 1.0f },
	};
	for (size_t i = 0; i < sizeof(PW) / sizeof(PW[0]); i++)
		test_pointwise(&e, PW[i].name, PW[i].fn, PW[i].tol);
	test_hdr_flat(&e);

	printf("detail effects on a flat image\n");
	const char *flat[] = { "clarity", "cartoon", "deband", "cas=1", "dls=1:0.17", "detail=1", "detail=0", "fxaa", "smaa", "chromatic_aberration" };
	for (size_t i = 0; i < sizeof(flat) / sizeof(flat[0]); i++)
		test_flat_identity(&e, flat[i], !strcmp(flat[i], "deband") ? 1 : 0);

	printf("sharpening and antialiasing\n");
	test_sharpen(&e, "cas=0.5");
	test_sharpen(&e, "dls=0.5:0.17");
	test_detail(&e);
	test_aa(&e, "fxaa");
	test_aa(&e, "smaa");
	printf("finish\n");
	test_finish(&e);

	printf("chains\n");
	for (size_t i = 0; i < sizeof(PRESETS) / sizeof(PRESETS[0]); i++) {
		size_t n = (size_t)w * h;
		uint8_t *img = malloc(n * 4);
		fill_ramp(img, (uint32_t)w, (uint32_t)h);
		CHECK(run_post(&e, PRESETS[i].spec, 0.5f, img, NULL) == 0, "preset %s fails", PRESETS[i].name);
		free(img);
	}
	env_destroy(&e);

	if (bench > 0) {
		printf("timing at %dx%d, %d runs (median, all passes of the chain)\n", bw, bh, bench);
		if (env_init(&e, &vk, (uint32_t)bw, (uint32_t)bh)) {
			fprintf(stderr, "bench images failed\n");
			return 1;
		}
		size_t n = (size_t)bw * bh;
		uint8_t *img = malloc(n * 4);
		double *t = malloc(sizeof(double) * (size_t)bench);
		for (size_t i = 0; i < sizeof(PRESETS) / sizeof(PRESETS[0]); i++) {
			for (int k = 0; k < bench; k++) {
				fill_ramp(img, (uint32_t)bw, (uint32_t)bh);
				run_post(&e, PRESETS[i].spec, (float)k / bench, img, &t[k]);
			}
			for (int a = 1; a < bench; a++)
				for (int b = a; b > 0 && t[b - 1] > t[b]; b--) {
					double x = t[b];
					t[b] = t[b - 1];
					t[b - 1] = x;
				}
			printf("  %-10s %8.1f us  (%s)\n", PRESETS[i].name, t[bench / 2] / 1000.0, PRESETS[i].spec);
		}
		free(img);
		free(t);
		env_destroy(&e);
	}
	kfg_vk_destroy(&vk);
	printf(g_fail ? "FAIL: %d failure(s)\n" : "PASS: %d failure(s)\n", g_fail);
	return g_fail ? 1 : 0;
}
