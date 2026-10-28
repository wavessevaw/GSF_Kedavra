// SPDX-License-Identifier: GPL-3.0-or-later
//
// KEDAVRA frame generation: GPU pass plan.
//
// The plan is the single description of which compute passes run, in what
// order, on which images and with which constants. Hosts (the standalone
// Vulkan harness and the Gamescope integration) only map image roles to their
// own textures, bind them in the listed slots and dispatch. Nothing here
// touches Vulkan, so the plan is shared verbatim between hosts.
//
// Per source frame (when it is latched):  analysis of the pair (A = previous, B = new)
//   reduce   colour B   -> luma B level 1 (1/2 resolution)
//   reduce   luma B L1  -> L2 -> L3 -> L4 (1/16)
//   me       L4 A,B               -> flow L4   (bidirectional)
//   me       L3 A,B + flow L4     -> flow L3
//   me       L2 A,B + flow L3     -> flow L2   (one vector pair per 4x4 pixels)
//   stats    L2 A,B + flow L2     -> stats     (scene cut)
// Per generated output at phase t:
//   select   L1 A,B + flow L2 + stats -> selection (4x4 blocks)
//   repair   x2: selection -> selection (neighbour agreement, ping-pong between two fields)
//   warp     colour A,B + selection + stats -> output (full resolution)
//
// Above about 1.44x the Deck's pixel count the analysis runs on a frame scaled down to that
// count (kfg_analysis_size): the first reduce resamples colour straight to the analysis
// level 1, every analysis pass works in analysis pixels, and only the warp runs at the
// source's own size, scaling the vectors up. A 1080p or 4K game then costs about 1.4x the
// analysis of a Deck frame plus a warp over its own pixels, and a camera turn covers the same
// share of the search range at any resolution.
#ifndef KFG_PLAN_H
#define KFG_PLAN_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KFG_LEVELS 4      // luma levels 1..4
#define KFG_RING 4        // frames / pairs kept so delayed outputs can still use their pair
#define KFG_MAX_SLOTS 4
#define KFG_MAX_PASSES 16

enum kfg_shader {
	KFG_SHADER_REDUCE = 0,
	KFG_SHADER_ME,
	KFG_SHADER_STATS,
	KFG_SHADER_SELECT,
	KFG_SHADER_WARP,
	KFG_SHADER_POST,        // post-generation filters (kfg_post.h)
	KFG_SHADER_SMAA_EDGE,
	KFG_SHADER_SMAA_WEIGHT,
	KFG_SHADER_SMAA_BLEND,
	KFG_SHADER_COUNT
};

static const char *const kfg_shader_names[KFG_SHADER_COUNT] = {
	"kfg_reduce", "kfg_me", "kfg_stats", "kfg_select", "kfg_warp",
	"kfg_post", "kfg_smaa_edge", "kfg_smaa_weight", "kfg_smaa_blend",
};

// Image roles. Ring-indexed roles carry the ring index in kfg_image_ref.ring.
enum kfg_role {
	KFG_ROLE_NONE = 0,
	KFG_ROLE_COLOR,     // source colour of a frame (host-owned, e.g. a compositor commit)
	KFG_ROLE_LUMA,      // luma pyramid of a frame; level 1..4
	KFG_ROLE_FLOW,      // bidirectional flow of a pair; level 2..4
	KFG_ROLE_STATS,     // 7x1 pair statistics: texel 0 cut test, texels 1-4 camera motion (two projective maps), 5-6 log of the forward map
	KFG_ROLE_SELECT,    // selection field scratch (select and repair ping-pong)
	KFG_ROLE_PAIR_SELECT, // a pair's selection field at mid-phase (ring-indexed like FLOW)
	KFG_ROLE_OUTPUT,    // generated output (host-owned)
	// Post-generation filters (kfg_post.h), all host-owned.
	KFG_ROLE_POST_IN,       // the frame this vblank shows, real or generated
	KFG_ROLE_POST,          // filter ping-pong images, ring 0 or 1, RGBA8 at frame size
	KFG_ROLE_SMAA_EDGES,    // RGBA8 at frame size
	KFG_ROLE_SMAA_WEIGHTS,  // RGBA8 at frame size
	KFG_ROLE_SMAA_AREA,     // constant lookup texture, RGBA8 160x560 (kfg_smaa_tex.h)
	KFG_ROLE_SMAA_SEARCH,   // constant lookup texture, R8 64x16, sampled nearest
};

// Transfer function of the source colour. The ring and the outputs keep the source's encoded
// values unchanged, so the compositor decodes generated frames exactly like the game's own.
// Only the luma pyramid is derived differently, so motion analysis sees similar contrast
// for SDR and HDR. HDR needs float colour images (KFG_FORMAT_RGBA16F): 8 bits per channel
// would band PQ and clip scRGB above 1.0.
enum kfg_transfer {
	KFG_TRANSFER_SDR = 0,   // sRGB / gamma 2.2 code values, 8 or 10 bit
	KFG_TRANSFER_PQ,        // HDR10: SMPTE ST 2084 code values (BT.2020)
	KFG_TRANSFER_SCRGB,     // linear BT.709, 1.0 = 80 nits, above 1.0 and below 0.0 allowed
};

static const char *const kfg_transfer_names[3] = { "sdr", "pq", "scrgb" };

// Formats requested for internal roles (hosts translate to their own enums).
enum kfg_format {
	KFG_FORMAT_R8 = 0,         // luma
	KFG_FORMAT_RGBA16F,        // flow, selection, stats
	KFG_FORMAT_RGBA8,          // output
};

typedef struct kfg_image_ref {
	enum kfg_role role;
	uint32_t ring;   // frame ring index (COLOR, LUMA) or pair ring index (FLOW, STATS)
	uint32_t level;  // LUMA / FLOW level
} kfg_image_ref;

typedef struct kfg_params {
	// Layout matches kfg_common.glsl (scalar block layout).
	int32_t dst_size[2];
	int32_t src_size[2];
	int32_t full_size[2];
	float t;
	int32_t level;
	int32_t radius;
	int32_t flags;
	float lambda;
	float cut_threshold;
	float scale[2];   // warp: source pixels per analysis pixel (x, y)
} kfg_params;

#define KFG_FLAG_COARSEST 1
#define KFG_FLAG_DEBUG_MARKER 2
#define KFG_FLAG_LUMA_INPUT 4
#define KFG_FLAG_TENT 8        // reduce: 4x4 tent instead of 2x2 box (coarse search levels)
#define KFG_FLAG_TEMPORAL 16   // me (coarsest): slot 2 holds the previous pair's flow, slot 3 its stats
#define KFG_FLAG_GLOBAL 32     // stats: camera motion (affine) of the coarsest flow into stats texels 1-3
#define KFG_FLAG_REPAIR 64     // select: clean up the selection field in slot 2 (one smoothing round)
#define KFG_FLAG_PQ 128        // reduce (from colour): HDR10 PQ code values, luma normalised to 0..1000 nits
#define KFG_FLAG_SCRGB 256     // reduce: linear scRGB, luma taken in PQ like KFG_FLAG_PQ; warp: keep
                               // negative values (colours outside BT.709)
#define KFG_FLAG_PAIR 512      // select: mid-phase field for the whole pair (z = side ratio, no lean);
                               // warp: project the pair's field to the output phase
#define KFG_FLAG_RESAMPLE 1024 // reduce (from colour): any ratio down to the analysis level 1
#define KFG_REPAIR_ROUNDS 2    // select -> repair -> repair

// Testing only (kfg_vktest A/B): select and repair for every output phase, as up to 1.2.
static int kfg_per_phase_select = 0;

// Pixel count the motion analysis works at: 1.44x the Deck's screen (1536x960). At the Deck's
// own count slow pans on a 1080p TV lost up to 4.5 dB against a full-size analysis (motion is
// found to the Deck's precision); at 1.44x they are within 1.6 dB and fast motion, occlusion and
// the camera are as good or better, for about 1.4x the Deck's analysis work at any resolution
// (1080p in full: 2x, 4K: 8x). Hosts may change it before creating their images (0 = always the
// source's own size).
static uint32_t kfg_analysis_pixels = 1536u * 960u;

typedef struct kfg_pass {
	enum kfg_shader shader;
	kfg_image_ref dst;
	kfg_image_ref slots[KFG_MAX_SLOTS];
	uint32_t nslots;
	uint32_t nearest_mask;  // slots sampled with a nearest filter (bit per slot); the rest linear
	kfg_params params;
	uint32_t groups[2];   // workgroup counts
	const char *label;
} kfg_pass;

typedef struct kfg_plan {
	kfg_pass pass[KFG_MAX_PASSES];
	uint32_t n;
} kfg_plan;

typedef struct kfg_tuning {
	int32_t radius_coarsest;   // texels at level 4 (x16 pixels)
	int32_t radius_refine;     // texels at levels 3 and 2
	float lambda;
	float cut_threshold;       // fraction of badly explained blocks
	int32_t transfer;          // enum kfg_transfer of the colour frames
} kfg_tuning;

static inline kfg_tuning kfg_default_tuning(void)
{
	kfg_tuning t;
	t.radius_coarsest = 6;  // +-96 pixels per source frame before refinement
	t.radius_refine = 1;
	t.lambda = 0.02f;
	t.cut_threshold = 0.5f;
	t.transfer = KFG_TRANSFER_SDR;
	return t;
}

// Flags a pass that reads colour needs for this transfer.
static inline int32_t kfg_transfer_flags(int32_t transfer)
{
	return transfer == KFG_TRANSFER_PQ ? KFG_FLAG_PQ : transfer == KFG_TRANSFER_SCRGB ? KFG_FLAG_SCRGB : 0;
}

// Format of the colour ring, the generated outputs and the filter images for a transfer.
static inline enum kfg_format kfg_color_format(int32_t transfer)
{
	return transfer == KFG_TRANSFER_SDR ? KFG_FORMAT_RGBA8 : KFG_FORMAT_RGBA16F;
}

static inline uint32_t kfg_div_up(uint32_t a, uint32_t b) { return (a + b - 1u) / b; }

// Size of luma/flow level l (l >= 1) for a full-resolution w x h source.
static inline void kfg_level_size(uint32_t w, uint32_t h, uint32_t l, uint32_t out[2])
{
	uint32_t x = w, y = h;
	for (uint32_t i = 0; i < l; i++) {
		x = (x + 1u) / 2u;
		y = (y + 1u) / 2u;
	}
	out[0] = x ? x : 1u;
	out[1] = y ? y : 1u;
}

// Size of the frame the analysis works on: the source itself up to 1.2x kfg_analysis_pixels
// (the Deck's 1280x800, 1600x900 and 1680x1050 stay exact), else the source scaled down, aspect
// kept, to kfg_analysis_pixels (1920x1080 and 3840x2160 -> 1619x911).
static inline void kfg_analysis_size(uint32_t w, uint32_t h, uint32_t out[2])
{
	const double px = (double)w * (double)h, limit = (double)kfg_analysis_pixels;
	out[0] = w;
	out[1] = h;
	if (!kfg_analysis_pixels || px <= 1.2 * limit)
		return;
	const double k = sqrt(px / limit);
	out[0] = (uint32_t)((double)w / k + 0.5);
	out[1] = (uint32_t)((double)h / k + 0.5);
	if (out[0] < 1u) out[0] = 1u;
	if (out[1] < 1u) out[1] = 1u;
}

// Size of the selection field (one texel per 4x4 analysis pixels) - equals analysis level 2.
// w x h: the analysis size.
static inline void kfg_select_size(uint32_t w, uint32_t h, uint32_t out[2])
{
	out[0] = kfg_div_up(w, 4u);
	out[1] = kfg_div_up(h, 4u);
}

static inline kfg_image_ref kfg_ref(enum kfg_role role, uint32_t ring, uint32_t level)
{
	kfg_image_ref r;
	r.role = role;
	r.ring = ring;
	r.level = level;
	return r;
}

static inline kfg_pass *kfg_plan_add(kfg_plan *p, enum kfg_shader sh, kfg_image_ref dst, const char *label)
{
	kfg_pass *x = &p->pass[p->n++];
	memset(x, 0, sizeof(*x));
	x->shader = sh;
	x->dst = dst;
	x->label = label;
	return x;
}

static inline void kfg_set_domain(kfg_pass *x, const uint32_t dst[2], uint32_t gx, uint32_t gy)
{
	x->params.dst_size[0] = (int32_t)dst[0];
	x->params.dst_size[1] = (int32_t)dst[1];
	x->groups[0] = kfg_div_up(dst[0], gx);
	x->groups[1] = kfg_div_up(dst[1], gy);
}

// Motion selection at block resolution and its repair rounds, ending in `out`.
// w x h: the analysis size.
static inline void kfg_plan_selection(kfg_plan *p, uint32_t w, uint32_t h, uint32_t fa, uint32_t fb, uint32_t pr,
				      float t, int32_t flags, kfg_image_ref out)
{
	uint32_t l1[2], bs[2];
	kfg_level_size(w, h, 1, l1);
	kfg_select_size(w, h, bs);

	kfg_pass *x = kfg_plan_add(p, KFG_SHADER_SELECT, kfg_ref(KFG_ROLE_SELECT, 0, 0), "select");
	x->slots[0] = kfg_ref(KFG_ROLE_LUMA, fa, 1);
	x->slots[1] = kfg_ref(KFG_ROLE_LUMA, fb, 1);
	x->slots[2] = kfg_ref(KFG_ROLE_FLOW, pr, 2);
	x->slots[3] = kfg_ref(KFG_ROLE_STATS, pr, 0);
	x->nslots = 4;
	x->params.src_size[0] = (int32_t)l1[0];
	x->params.src_size[1] = (int32_t)l1[1];
	x->params.full_size[0] = (int32_t)w;
	x->params.full_size[1] = (int32_t)h;
	x->params.t = t;
	x->params.flags = flags;
	kfg_set_domain(x, bs, 8, 8);

	// Repair rounds: a block whose vector disagrees with its neighbours takes a neighbour's
	// vector when that explains the block about as well. Wrong vectors were whole blocks
	// (square artifacts); each round removes isolated ones and shrinks small clusters.
	for (uint32_t r = 0; r < KFG_REPAIR_ROUNDS; r++) {
		x = kfg_plan_add(p, KFG_SHADER_SELECT, r + 1 == KFG_REPAIR_ROUNDS ? out : kfg_ref(KFG_ROLE_SELECT, (r + 1) & 1u, 0), "repair");
		x->slots[0] = kfg_ref(KFG_ROLE_LUMA, fa, 1);
		x->slots[1] = kfg_ref(KFG_ROLE_LUMA, fb, 1);
		x->slots[2] = kfg_ref(KFG_ROLE_SELECT, r & 1u, 0);
		x->slots[3] = kfg_ref(KFG_ROLE_STATS, pr, 0);
		x->nslots = 4;
		x->params.src_size[0] = (int32_t)l1[0];
		x->params.src_size[1] = (int32_t)l1[1];
		x->params.full_size[0] = (int32_t)w;
		x->params.full_size[1] = (int32_t)h;
		x->params.t = t;
		x->params.flags = flags | KFG_FLAG_REPAIR;
		kfg_set_domain(x, bs, 8, 8);
	}
}

// Analysis of the pair (frame ring fa -> frame ring fb), written to pair ring pr.
// have_prev = 0 for the first frame after a reset: only the pyramid of B is built.
// prev_pair = pair ring of the previous pair (the one ending at frame A) when its flow is
// valid, else -1. Its coarse flow seeds the coarsest search, so a camera that keeps turning
// is tracked beyond the search radius and the vectors stay steady from pair to pair.
static inline void kfg_plan_analyze(kfg_plan *p, uint32_t w, uint32_t h, uint32_t fa, uint32_t fb, uint32_t pr,
				    int have_prev, int prev_pair, const kfg_tuning *tune)
{
	p->n = 0;
	const uint32_t fw = w, fh = h;
	uint32_t asz[2];
	kfg_analysis_size(fw, fh, asz);
	w = asz[0];
	h = asz[1];
	const int resample = w != fw || h != fh;
	uint32_t sz[KFG_LEVELS + 1][2];
	sz[0][0] = fw;
	sz[0][1] = fh;
	for (uint32_t l = 1; l <= KFG_LEVELS; l++)
		kfg_level_size(w, h, l, sz[l]);

	for (uint32_t l = 1; l <= KFG_LEVELS; l++) {
		kfg_pass *x = kfg_plan_add(p, KFG_SHADER_REDUCE, kfg_ref(KFG_ROLE_LUMA, fb, l), "reduce");
		x->slots[0] = l == 1 ? kfg_ref(KFG_ROLE_COLOR, fb, 0) : kfg_ref(KFG_ROLE_LUMA, fb, l - 1);
		x->nslots = 1;
		x->params.src_size[0] = (int32_t)sz[l - 1][0];
		x->params.src_size[1] = (int32_t)sz[l - 1][1];
		x->params.flags = l == 1 ? kfg_transfer_flags(tune->transfer) | (resample ? KFG_FLAG_RESAMPLE : 0)
			: KFG_FLAG_LUMA_INPUT | (l >= 3 ? KFG_FLAG_TENT : 0);
		kfg_set_domain(x, sz[l], 8, 8);
	}
	if (!have_prev)
		return;
	for (uint32_t l = KFG_LEVELS; l >= 2; l--) {
		kfg_pass *x = kfg_plan_add(p, KFG_SHADER_ME, kfg_ref(KFG_ROLE_FLOW, pr, l), "me");
		x->slots[0] = kfg_ref(KFG_ROLE_LUMA, fa, l);
		x->slots[1] = kfg_ref(KFG_ROLE_LUMA, fb, l);
		// Coarsest level: slots 2 and 3 are the previous pair's flow at this level and its
		// global motion (temporal seeds). Finer levels: the coarser flow and this pair's
		// global motion.
		const int temporal = l == KFG_LEVELS && prev_pair >= 0;
		x->slots[2] = l < KFG_LEVELS ? kfg_ref(KFG_ROLE_FLOW, pr, l + 1)
			: temporal ? kfg_ref(KFG_ROLE_FLOW, (uint32_t)prev_pair, l) : kfg_ref(KFG_ROLE_NONE, 0, 0);
		x->slots[3] = l < KFG_LEVELS ? kfg_ref(KFG_ROLE_STATS, pr, 0)
			: temporal ? kfg_ref(KFG_ROLE_STATS, (uint32_t)prev_pair, 0) : kfg_ref(KFG_ROLE_NONE, 0, 0);
		x->nslots = 4;
		x->params.src_size[0] = (int32_t)sz[l][0];
		x->params.src_size[1] = (int32_t)sz[l][1];
		x->params.full_size[0] = (int32_t)w;
		x->params.full_size[1] = (int32_t)h;
		x->params.level = (int32_t)l;
		x->params.radius = l == KFG_LEVELS ? tune->radius_coarsest : tune->radius_refine;
		x->params.flags = l == KFG_LEVELS ? KFG_FLAG_COARSEST | (temporal ? KFG_FLAG_TEMPORAL : 0) : 0;
		x->params.lambda = tune->lambda;
		kfg_set_domain(x, sz[l], 8, 8);
		if (l == KFG_LEVELS) {
			// Camera motion of the coarsest field (turns and walking), a candidate for every block.
			x = kfg_plan_add(p, KFG_SHADER_STATS, kfg_ref(KFG_ROLE_STATS, pr, 0), "global");
			x->slots[0] = kfg_ref(KFG_ROLE_FLOW, pr, l);
			x->nslots = 1;
			x->params.src_size[0] = (int32_t)sz[l][0];
			x->params.src_size[1] = (int32_t)sz[l][1];
			x->params.full_size[0] = (int32_t)w;
			x->params.full_size[1] = (int32_t)h;
			x->params.flags = KFG_FLAG_GLOBAL;
			x->params.dst_size[0] = 7;
			x->params.dst_size[1] = 1;
			x->groups[0] = 1;
			x->groups[1] = 1;
		}
	}
	// The camera motion again from the finest field (GSF 1.4.3): the coarsest one is quantised
	// to 8 pixels and wrong at the sides of a fast turn, where the map matters most. Used by
	// the selection, the warp and the next pair's coarsest search.
	kfg_pass *x = kfg_plan_add(p, KFG_SHADER_STATS, kfg_ref(KFG_ROLE_STATS, pr, 0), "global");
	x->slots[0] = kfg_ref(KFG_ROLE_FLOW, pr, 2);
	x->nslots = 1;
	x->params.src_size[0] = (int32_t)sz[2][0];
	x->params.src_size[1] = (int32_t)sz[2][1];
	x->params.full_size[0] = (int32_t)w;
	x->params.full_size[1] = (int32_t)h;
	x->params.radius = 2;  // every second block in both directions
	x->params.flags = KFG_FLAG_GLOBAL;
	x->params.dst_size[0] = 7;
	x->params.dst_size[1] = 1;
	x->groups[0] = 1;
	x->groups[1] = 1;
	x = kfg_plan_add(p, KFG_SHADER_STATS, kfg_ref(KFG_ROLE_STATS, pr, 0), "stats");
	x->slots[0] = kfg_ref(KFG_ROLE_LUMA, fa, 2);
	x->slots[1] = kfg_ref(KFG_ROLE_LUMA, fb, 2);
	x->slots[2] = kfg_ref(KFG_ROLE_FLOW, pr, 2);
	x->nslots = 3;
	x->params.src_size[0] = (int32_t)sz[2][0];
	x->params.src_size[1] = (int32_t)sz[2][1];
	x->params.cut_threshold = tune->cut_threshold;
	x->params.dst_size[0] = 1;
	x->params.dst_size[1] = 1;
	x->groups[0] = 1;
	x->groups[1] = 1;
	if (!kfg_per_phase_select)
		kfg_plan_selection(p, w, h, fa, fb, pr, 0.5f, KFG_FLAG_PAIR, kfg_ref(KFG_ROLE_PAIR_SELECT, pr, 0));
}

// One generated output at phase t in (0, 1) between frames fa and fb of pair pr: a single
// warp pass over the pair's selection field, which the analysis made once for every phase
// (up to 1.2 the selection and its two repair rounds ran again for each generated frame,
// nine tenths of the generation cost).
// flags: KFG_FLAG_DEBUG_MARKER and kfg_transfer_flags() of the colour frames.
static inline void kfg_plan_generate(kfg_plan *p, uint32_t w, uint32_t h, uint32_t fa, uint32_t fb, uint32_t pr,
				     float t, int32_t flags)
{
	p->n = 0;
	uint32_t full[2] = { w, h }, asz[2];
	kfg_analysis_size(w, h, asz);
	kfg_image_ref sel = kfg_ref(KFG_ROLE_PAIR_SELECT, pr, 0);
	if (kfg_per_phase_select) {
		sel = kfg_ref(KFG_ROLE_SELECT, 0, 0);  // even rounds end in field 0
		kfg_plan_selection(p, asz[0], asz[1], fa, fb, pr, t, 0, sel);
	} else
		flags |= KFG_FLAG_PAIR;

	kfg_pass *x = kfg_plan_add(p, KFG_SHADER_WARP, kfg_ref(KFG_ROLE_OUTPUT, 0, 0), "warp");
	x->slots[0] = kfg_ref(KFG_ROLE_COLOR, fa, 0);
	x->slots[1] = kfg_ref(KFG_ROLE_COLOR, fb, 0);
	x->slots[2] = sel;
	x->slots[3] = kfg_ref(KFG_ROLE_STATS, pr, 0);
	x->nslots = 4;
	x->params.src_size[0] = (int32_t)asz[0];  // analysis size: the field is a quarter of it
	x->params.src_size[1] = (int32_t)asz[1];
	x->params.full_size[0] = (int32_t)w;
	x->params.full_size[1] = (int32_t)h;
	x->params.scale[0] = (float)w / (float)asz[0];
	x->params.scale[1] = (float)h / (float)asz[1];
	x->params.t = t;
	x->params.flags = flags;
	kfg_set_domain(x, full, 8, 8);
}

// Format of an internal role.
static inline enum kfg_format kfg_role_format(enum kfg_role r)
{
	switch (r) {
	case KFG_ROLE_LUMA: return KFG_FORMAT_R8;
	case KFG_ROLE_OUTPUT:
	case KFG_ROLE_POST:
	case KFG_ROLE_SMAA_EDGES:
	case KFG_ROLE_SMAA_WEIGHTS:
	case KFG_ROLE_SMAA_AREA:
		return KFG_FORMAT_RGBA8;
	case KFG_ROLE_SMAA_SEARCH: return KFG_FORMAT_R8;
	default: return KFG_FORMAT_RGBA16F;
	}
}

#ifdef __cplusplus
}
#endif
#endif
