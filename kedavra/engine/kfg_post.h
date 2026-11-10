// SPDX-License-Identifier: GPL-3.0-or-later
//
// KEDAVRA post-generation filters: configuration and pass plan.
//
// Detail chains run inside the composite on the frame a vblank shows, after it is picked
// or generated. Motion analysis never sees Detail. Legacy chains retain 1.5.0's source-ring
// prefiltering when generation is active; with generation off they filter the shown frame.
// The HUD and Steam overlays are other layers and stay unfiltered.
//
// Stages, each one dispatch (or three for SMAA), skipped when nothing in it is on:
//   grade    colour looks (kfg_post.comp, level 0), fixed order below
//   sharpen  CAS, DLS or opt-in Detail (level 1)
//   aa       FXAA (level 2) or SMAA (edge, weight, blend)
//   finish   chromatic aberration, vignette, film grain (level 3)
// Images: POST_IN -> POST[0] -> POST[1] -> POST[0] ...; the last one written is shown.
//
// Pixel values are the game's encoded (sRGB) values, read through the UNORM view, as
// ReShade and vkBasalt see them; effects that need linear light convert explicitly.
//
// Configuration string (gamescopectl kfg_post), comma separated, empty = off:
//   effect names (KFG_POST_EFFECTS), "cas=S", "dls=S:D", "detail=S", "fxaa", "smaa"
//   S = sharpness 0..1, D = DLS denoise 0..1. Example: "vibrance,vignette,cas=0.5,smaa".
#ifndef KFG_POST_H
#define KFG_POST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kfg_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

enum kfg_post_stage {
	KFG_POST_GRADE = 0,
	KFG_POST_SHARPEN = 1,
	KFG_POST_FXAA = 2,
	KFG_POST_FINISH = 3,
};

// Grade effects, in the order they run (bit i = row i of KFG_POST_EFFECTS while stage is grade).
#define KFG_GRADE_DEBAND         (1u << 0)
#define KFG_GRADE_HDR_LOOK       (1u << 1)
#define KFG_GRADE_CLARITY        (1u << 2)
#define KFG_GRADE_CARTOON        (1u << 3)
#define KFG_GRADE_LEVELS_PLUS    (1u << 4)
#define KFG_GRADE_CURVES         (1u << 5)
#define KFG_GRADE_VIBRANCE       (1u << 6)
#define KFG_GRADE_COLOURFULNESS  (1u << 7)
#define KFG_GRADE_TECHNICOLOR2   (1u << 8)
#define KFG_GRADE_DPX            (1u << 9)
#define KFG_GRADE_TECHNICOLOR    (1u << 10)
#define KFG_GRADE_BLEACH_BYPASS  (1u << 11)
#define KFG_GRADE_SEPIA          (1u << 12)
#define KFG_GRADE_NOIR           (1u << 13)
#define KFG_GRADE_MONOCHROME     (1u << 14)
#define KFG_GRADE_NOSTALGIA      (1u << 15)

#define KFG_FINISH_CHROMATIC     (1u << 0)
#define KFG_FINISH_VIGNETTE      (1u << 1)
#define KFG_FINISH_FILM_GRAIN    (1u << 2)

#define KFG_SHARPEN_NONE 0
#define KFG_SHARPEN_CAS 1
#define KFG_SHARPEN_DLS 2
#define KFG_SHARPEN_DETAIL 3
#ifdef KFG_POST_RESEARCH
#define KFG_SHARPEN_RCAS 4
#endif

#define KFG_AA_NONE 0
#define KFG_AA_FXAA 1
#define KFG_AA_SMAA 2

typedef struct kfg_post_effect {
	const char *name;
	enum kfg_post_stage stage;
	uint32_t bit;
} kfg_post_effect;

static const kfg_post_effect KFG_POST_EFFECTS[] = {
	{ "deband", KFG_POST_GRADE, KFG_GRADE_DEBAND },
	{ "hdr_look", KFG_POST_GRADE, KFG_GRADE_HDR_LOOK },
	{ "clarity", KFG_POST_GRADE, KFG_GRADE_CLARITY },
	{ "cartoon", KFG_POST_GRADE, KFG_GRADE_CARTOON },
	{ "levels_plus", KFG_POST_GRADE, KFG_GRADE_LEVELS_PLUS },
	{ "curves", KFG_POST_GRADE, KFG_GRADE_CURVES },
	{ "vibrance", KFG_POST_GRADE, KFG_GRADE_VIBRANCE },
	{ "colourfulness", KFG_POST_GRADE, KFG_GRADE_COLOURFULNESS },
	{ "technicolor2", KFG_POST_GRADE, KFG_GRADE_TECHNICOLOR2 },
	{ "dpx", KFG_POST_GRADE, KFG_GRADE_DPX },
	{ "technicolor", KFG_POST_GRADE, KFG_GRADE_TECHNICOLOR },
	{ "bleach_bypass", KFG_POST_GRADE, KFG_GRADE_BLEACH_BYPASS },
	{ "sepia", KFG_POST_GRADE, KFG_GRADE_SEPIA },
	{ "noir", KFG_POST_GRADE, KFG_GRADE_NOIR },
	{ "monochrome", KFG_POST_GRADE, KFG_GRADE_MONOCHROME },
	{ "nostalgia", KFG_POST_GRADE, KFG_GRADE_NOSTALGIA },
	{ "chromatic_aberration", KFG_POST_FINISH, KFG_FINISH_CHROMATIC },
	{ "vignette", KFG_POST_FINISH, KFG_FINISH_VIGNETTE },
	{ "film_grain", KFG_POST_FINISH, KFG_FINISH_FILM_GRAIN },
};
#define KFG_POST_EFFECT_COUNT (sizeof(KFG_POST_EFFECTS) / sizeof(KFG_POST_EFFECTS[0]))

typedef struct kfg_post_cfg {
	uint32_t grade;     // KFG_GRADE_* bits
	uint32_t finish;    // KFG_FINISH_* bits
	int sharpen;        // KFG_SHARPEN_*
	float sharpness;    // 0..1
	float denoise;      // 0..1 (DLS)
	int aa;             // KFG_AA_*
} kfg_post_cfg;

static inline int kfg_post_active(const kfg_post_cfg *c)
{
	return c->grade || c->finish || c->sharpen != KFG_SHARPEN_NONE || c->aa != KFG_AA_NONE;
}

// Detail is evaluated on each displayed frame, never on the motion-analysis input.
// Legacy presets retain their 1.5.0 placement before generation.
static inline int kfg_post_after_generation(const kfg_post_cfg *c)
{
	return c->sharpen == KFG_SHARPEN_DETAIL;
}

static inline kfg_post_cfg kfg_post_for_transfer(kfg_post_cfg c, int transfer)
{
	if (transfer == KFG_TRANSFER_SCRGB)
		memset(&c, 0, sizeof(c)); // Existing bypass: preserve negative/over-range linear light.
	else if (transfer == KFG_TRANSFER_PQ) {
		c.grade = 0;
		// Detail's SDR noise thresholds have not been validated for PQ. Keep HDR untouched
		// by this new effect; explicitly requested legacy effects still behave as before.
		if (c.sharpen == KFG_SHARPEN_DETAIL)
			c.sharpen = KFG_SHARPEN_NONE;
	}
	return c;
}

static inline float kfg_post_unit(const char *s, int *ok)
{
	char *end = NULL;
	const double v = strtod(s, &end);
	if (end == s || !(v >= 0.0 && v <= 1.0))
		*ok = 0;
	return (float)v;
}

// Parses a kfg_post string. Returns 0 and fills *out, or -1 with a reason in err (and *out off).
static inline int kfg_post_parse(const char *spec, kfg_post_cfg *out, char *err, size_t nerr)
{
	memset(out, 0, sizeof(*out));
	if (err && nerr)
		err[0] = 0;
	if (!spec)
		return 0;
	char buf[512];
	if (strlen(spec) >= sizeof(buf)) {
		if (err) snprintf(err, nerr, "too long");
		return -1;
	}
	strcpy(buf, spec);
	kfg_post_cfg c;
	memset(&c, 0, sizeof(c));
	for (char *tok = buf, *end; *tok; tok = end) {
		while (*tok == ',' || *tok == ' ')
			tok++;
		if (!*tok)
			break;
		for (end = tok; *end && *end != ',' && *end != ' '; end++)
			;
		if (*end)
			*end++ = 0;
		int ok = 1, known = 0;
		for (size_t i = 0; i < KFG_POST_EFFECT_COUNT; i++) {
			if (!strcmp(tok, KFG_POST_EFFECTS[i].name)) {
				if (KFG_POST_EFFECTS[i].stage == KFG_POST_GRADE)
					c.grade |= KFG_POST_EFFECTS[i].bit;
				else
					c.finish |= KFG_POST_EFFECTS[i].bit;
				known = 1;
			}
		}
		if (known)
			continue;
		if (!strncmp(tok, "detail=", 7)) {
			char *tail = NULL;
			double value = strtod(tok + 7, &tail);
			ok = tail != tok + 7 && !*tail && value >= 0.0 && value <= 1.0;
			c.sharpness = (float)value;
			c.sharpen = value == 0.0 ? KFG_SHARPEN_NONE : KFG_SHARPEN_DETAIL;
#ifdef KFG_POST_RESEARCH
		} else if (!strncmp(tok, "rcas=", 5)) {
			c.sharpen = KFG_SHARPEN_RCAS;
			c.sharpness = kfg_post_unit(tok + 5, &ok);
#endif
		} else if (!strncmp(tok, "cas=", 4)) {
			c.sharpen = KFG_SHARPEN_CAS;
			c.sharpness = kfg_post_unit(tok + 4, &ok);
		} else if (!strncmp(tok, "dls=", 4)) {
			c.sharpen = KFG_SHARPEN_DLS;
			char *colon = strchr(tok + 4, ':');
			if (colon)
				*colon = 0;
			c.sharpness = kfg_post_unit(tok + 4, &ok);
			c.denoise = colon ? kfg_post_unit(colon + 1, &ok) : 0.17f;
		} else if (!strcmp(tok, "fxaa")) {
			c.aa = KFG_AA_FXAA;
		} else if (!strcmp(tok, "smaa")) {
			c.aa = KFG_AA_SMAA;
		} else {
			ok = 0;
		}
		if (!ok) {
			if (err) snprintf(err, nerr, "bad token '%s'", tok);
			return -1;
		}
	}
	*out = c;
	return 0;
}

static inline kfg_pass *kfg_post_pass(kfg_plan *p, enum kfg_shader sh, kfg_image_ref dst, const char *label,
				      uint32_t w, uint32_t h)
{
	const uint32_t full[2] = { w, h };
	kfg_pass *x = kfg_plan_add(p, sh, dst, label);
	x->params.src_size[0] = x->params.full_size[0] = (int32_t)w;
	x->params.src_size[1] = x->params.full_size[1] = (int32_t)h;
	kfg_set_domain(x, full, 8, 8);
	return x;
}

// Filter passes for a w x h frame. grain_phase in [0, 1) moves the film grain each frame.
// Returns the POST ring index holding the result, or -1 when there is nothing to do.
static inline int kfg_plan_post(kfg_plan *p, uint32_t w, uint32_t h, const kfg_post_cfg *c, float grain_phase)
{
	p->n = 0;
	if (!kfg_post_active(c))
		return -1;
	kfg_image_ref cur = kfg_ref(KFG_ROLE_POST_IN, 0, 0);
	uint32_t next = 0;
	kfg_pass *x;
#define KFG_POST_STEP(stage_, flags_, label_)                                                    \
	do {                                                                                     \
		x = kfg_post_pass(p, KFG_SHADER_POST, kfg_ref(KFG_ROLE_POST, next, 0), label_, w, h); \
		x->slots[0] = cur;                                                               \
		x->nslots = 1;                                                                   \
		x->params.level = (stage_);                                                      \
		x->params.flags = (int32_t)(flags_);                                             \
		cur = kfg_ref(KFG_ROLE_POST, next, 0);                                           \
		next ^= 1u;                                                                      \
	} while (0)

	if (c->grade)
		KFG_POST_STEP(KFG_POST_GRADE, c->grade, "post grade");
	if (c->sharpen != KFG_SHARPEN_NONE) {
		KFG_POST_STEP(KFG_POST_SHARPEN, c->sharpen, "post sharpen");
		x->params.lambda = c->sharpness;
		x->params.cut_threshold = c->denoise;
	}
	if (c->aa == KFG_AA_FXAA)
		KFG_POST_STEP(KFG_POST_FXAA, 0, "post fxaa");
	if (c->aa == KFG_AA_SMAA) {
		x = kfg_post_pass(p, KFG_SHADER_SMAA_EDGE, kfg_ref(KFG_ROLE_SMAA_EDGES, 0, 0), "smaa edge", w, h);
		x->slots[0] = cur;
		x->nslots = 1;
		x = kfg_post_pass(p, KFG_SHADER_SMAA_WEIGHT, kfg_ref(KFG_ROLE_SMAA_WEIGHTS, 0, 0), "smaa weight", w, h);
		x->slots[0] = kfg_ref(KFG_ROLE_SMAA_EDGES, 0, 0);
		x->slots[1] = kfg_ref(KFG_ROLE_SMAA_AREA, 0, 0);
		x->slots[2] = kfg_ref(KFG_ROLE_SMAA_SEARCH, 0, 0);
		x->nslots = 3;
		x->nearest_mask = 1u << 2;
		x = kfg_post_pass(p, KFG_SHADER_SMAA_BLEND, kfg_ref(KFG_ROLE_POST, next, 0), "smaa blend", w, h);
		x->slots[0] = cur;
		x->slots[1] = kfg_ref(KFG_ROLE_SMAA_WEIGHTS, 0, 0);
		x->nslots = 2;
		cur = kfg_ref(KFG_ROLE_POST, next, 0);
		next ^= 1u;
	}
	if (c->finish) {
		KFG_POST_STEP(KFG_POST_FINISH, c->finish, "post finish");
		x->params.t = grain_phase;
	}
#undef KFG_POST_STEP
	return (int)cur.ring;
}

#ifdef __cplusplus
}
#endif
#endif
