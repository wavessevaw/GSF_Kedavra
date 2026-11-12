// SPDX-License-Identifier: GPL-3.0-or-later
//
// KEDAVRA interpolation correctness test and benchmark.
//
// Renders analytic scenes at continuous time, so the true intermediate frame
// at any phase is known. Runs the production pass plan and SPIR-V on a real
// Vulkan device (lavapipe in CI; RADV when run on the Steam Deck) and checks:
//   * generated frames are closer to the true intermediate than a blend or a
//     duplicate of the nearest frame (genuine temporal interpolation),
//   * static content and HUD stay exact, scene cuts are never blended,
//   * x3 phases 1/3 and 2/3 differ from each other and from both sources,
//   * the frame ring handles a running sequence.
// Writes a JSON report; GPU pass durations come from Vulkan timestamps and are
// only meaningful on the device they were measured on.
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kfg_vk.h"

// KFG_NO_TEMPORAL=1 disables the temporal seed (A/B comparisons).
static int g_temporal = 1;

typedef struct scene {
	const char *name;
	int kind;
	double vx, vy;          // background motion, pixels per source frame
	bool dynamic;
	bool cut;
} scene;

enum { S_PAN, S_OBJECTS, S_OCCLUSION, S_STATIC, S_CUT, S_FASTPAN, S_STAIRS, S_CAMERA, S_DOLLY, S_DOLLY_STAIRS, S_NEAR, S_CLIMB, S_TURN };

static const scene SCENES[] = {
	{ "pan", S_PAN, 24, 6, true, false },
	{ "fast_pan", S_FASTPAN, 72, 0, true, false },
	{ "objects_hud", S_OBJECTS, 0, 0, true, false },
	{ "occlusion", S_OCCLUSION, -16, 0, true, false },
	{ "static", S_STATIC, 0, 0, false, false },
	{ "scene_cut", S_CUT, 0, 0, false, true },
	{ "stairs", S_STAIRS, 18, 10, true, false },
	{ "dolly", S_DOLLY, 0, 0, true, false },
	{ "dolly_stairs", S_DOLLY_STAIRS, 0, 0, true, false },
	{ "near_object", S_NEAR, 12, 0, true, false },
	{ "climb", S_CLIMB, 0, 0, true, false },
};

// Low base frame rate (--lowbase): every source interval is 1.25 times longer, so all
// motion between two source frames grows by 1.25 (24 instead of 30 FPS under the same camera),
// and the generated phases are those of 24 -> 90 Hz (x3.75: multiples of 1/15).
static double g_timescale = 1.0;
// --scaled: the scenes keep their 1280x800 layout and motion at any --size (a game at 1080p or
// 4K shows the same picture with more pixels), so results compare across resolutions.
// Regions given to the metrics are in that layout too.
static double g_geo = 1.0;
static bool g_lowbase = false;
#define PHASES_PER_PAIR (g_lowbase ? 4 : 3)
#define PHASE_STEP (g_lowbase ? 4.0 / 15.0 : 1.0 / 3.0)

static double hash2(int x, int y, int seed)
{
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return (double)(h & 0xffffff) / 16777215.0;
}

static double vnoise(double x, double y, int seed)
{
	int xi = (int)floor(x), yi = (int)floor(y);
	double fx = x - xi, fy = y - yi;
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	double a = hash2(xi, yi, seed), b = hash2(xi + 1, yi, seed), c = hash2(xi, yi + 1, seed), d = hash2(xi + 1, yi + 1, seed);
	return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

// Textured background: multi-octave noise with a brick grid, in [0,1]^3.
static void background(double x, double y, int seed, double rgb[3])
{
	double n = 0.5 * vnoise(x / 37.0, y / 37.0, seed) + 0.3 * vnoise(x / 11.0, y / 11.0, seed + 1) + 0.2 * vnoise(x / 4.0, y / 4.0, seed + 2);
	double bx = fmod(x + (fmod(floor(y / 40.0), 2.0) ? 30.0 : 0.0), 60.0), by = fmod(y, 40.0);
	if (bx < 0) bx += 60.0;
	if (by < 0) by += 40.0;
	double mortar = (bx < 3.0 || by < 3.0) ? 0.35 : 1.0;
	rgb[0] = (0.25 + 0.6 * n) * mortar;
	rgb[1] = (0.20 + 0.5 * vnoise(x / 23.0 + 7, y / 23.0, seed + 3)) * mortar;
	rgb[2] = (0.15 + 0.7 * (1 - n)) * mortar;
}

static bool hud(double x, double y, int w, int h, double rgb[3])
{
	// Text-like high-contrast glyph block, top left, and a status bar at the bottom.
	if (x >= 40 && x < 360 && y >= 30 && y < 70) {
		int cx = (int)((x - 40) / 8), cy = (int)((y - 30) / 8);
		bool on = hash2(cx, cy, 99) > 0.45 && fmod(x - 40, 8) < 6 && fmod(y - 30, 8) < 6;
		rgb[0] = rgb[1] = rgb[2] = on ? 0.95 : 0.05;
		return true;
	}
	if (y >= h - 50 && y < h - 30 && x >= 60 && x < w - 60) {
		rgb[0] = 0.9;
		rgb[1] = 0.75;
		rgb[2] = 0.1;
		return true;
	}
	return false;
}

// Stairs and a railing: treads repeat every 14 pixels and the camera moves 18 per source
// frame, so a wrong match 4 pixels off fits almost as well as the true motion (the aperture
// problem behind flickering stairs and straight lines in games).
static void stairs(double x, double y, double rgb[3])
{
	const double n = 0.06 * vnoise(x / 9.0, y / 9.0, 5);
	double ty = fmod(y + 0.35 * x, 14.0);
	if (ty < 0) ty += 14.0;
	double v = ty < 2.0 ? 0.92 : (ty < 3.5 ? 0.12 : 0.45 + 0.25 * ty / 14.0);
	// Railing: a thin straight handrail and posts.
	double ry = fmod(y + 0.35 * x, 112.0), rx = fmod(x, 46.0);
	if (ry < 0) ry += 112.0;
	if (rx < 0) rx += 46.0;
	if ((ry > 40 && ry < 41.6) || (rx < 1.5 && ry > 41.6 && ry < 80))
		v = 0.05;
	rgb[0] = v + n;
	rgb[1] = 0.95 * v + n;
	rgb[2] = 0.85 * v + n;
}

// Camera sequence: a pan that speeds up to 140 pixels per source frame and slows down again,
// with fractional speeds and a vertical drift, under a static HUD (an active camera in a game).
// Position is piecewise linear in time, so the true intermediate frame is well defined.
static const double CAM_VX[] = { 12.3, 31.7, 55.2, 78.9, 101.4, 124.6, 140.1, 118.3, 84.7, 47.5, 20.2 };
#define CAM_FRAMES ((int)(sizeof(CAM_VX) / sizeof(CAM_VX[0])))

static void camera_pos(double t, double *cx, double *cy)
{
	double x = 0, y = 0;
	int f = (int)floor(t);
	for (int i = 0; i < f && i < CAM_FRAMES; i++) {
		x += CAM_VX[i];
		y += 0.18 * CAM_VX[i] * sin(i * 1.3);
	}
	if (f >= 0 && f < CAM_FRAMES) {
		x += CAM_VX[f] * (t - f);
		y += 0.18 * CAM_VX[f] * sin(f * 1.3) * (t - f);
	}
	*cx = x * g_timescale;
	*cy = y * g_timescale;
}

// Turning camera in perspective (90 degree horizontal field of view at 1280 x 800): the yaw
// speeds up to 10 degrees per source frame and slows down again, with a pitch drift and a
// slight roll, under a static HUD. The world is a textured sphere around the camera, so the
// motion is a pure rotation: twice as fast at the sides as in the centre, curved off the
// horizon, as a game's camera turn is (a translation-only model misses the sides).
static const double TURN_W[] = { 0.8, 2.0, 3.5, 5.5, 7.5, 10.0, 8.0, 5.5, 3.0, 1.2 };
#define TURN_FRAMES ((int)(sizeof(TURN_W) / sizeof(TURN_W[0])))

static void turn_angles(double t, double *yaw, double *pitch, double *roll)
{
	double a = 0, b = 0;
	int f = (int)floor(t);
	for (int i = 0; i < f && i < TURN_FRAMES; i++) {
		a += TURN_W[i];
		b += 0.25 * TURN_W[i] * sin(i * 0.9);
	}
	if (f >= 0 && f < TURN_FRAMES) {
		a += TURN_W[f] * (t - f);
		b += 0.25 * TURN_W[f] * sin(f * 0.9) * (t - f);
	}
	const double k = g_timescale * 3.14159265358979 / 180.0;
	*yaw = a * k;
	*pitch = b * k;
	*roll = 0.04 * b * k;
}

static void sphere_pixel(double t, double x, double y, double rgb[3])
{
	const double f = 640.0;
	double yaw, pitch, roll;
	turn_angles(t, &yaw, &pitch, &roll);
	double dx = x - 640.0, dy = y - 400.0, dz = f;
	// roll (about z), pitch (about x), then yaw (about y)
	double cr = cos(roll), sr = sin(roll);
	double x1 = cr * dx - sr * dy, y1 = sr * dx + cr * dy, z1 = dz;
	double cp = cos(pitch), sp = sin(pitch);
	double y2 = cp * y1 - sp * z1, z2 = sp * y1 + cp * z1;
	double cy = cos(yaw), sy = sin(yaw);
	double x3 = cy * x1 + sy * z2, z3 = -sy * x1 + cy * z2;
	double lon = atan2(x3, z3), lat = atan2(y2, hypot(x3, z3));
	background(lon * f + 4000.0, lat * f, 17, rgb);
}

// Where the content at pixel (x, y) of the turn at time t0 is at time t1.
static void turn_project(double t0, double t1, double x, double y, double *x1, double *y1)
{
	double yaw, pitch, roll;
	turn_angles(t0, &yaw, &pitch, &roll);
	double dx = x - 640.0, dy = y - 400.0, dz = 640.0;
	double a = cos(roll) * dx - sin(roll) * dy, b = sin(roll) * dx + cos(roll) * dy, c = dz;
	double b2 = cos(pitch) * b - sin(pitch) * c, c2 = sin(pitch) * b + cos(pitch) * c;
	double a3 = cos(yaw) * a + sin(yaw) * c2, c3 = -sin(yaw) * a + cos(yaw) * c2;
	turn_angles(t1, &yaw, &pitch, &roll);
	double a4 = cos(yaw) * a3 - sin(yaw) * c3, c4 = sin(yaw) * a3 + cos(yaw) * c3;
	double b5 = cos(pitch) * b2 + sin(pitch) * c4, c5 = -sin(pitch) * b2 + cos(pitch) * c4;
	double a6 = cos(roll) * a4 + sin(roll) * b5, b6 = -sin(roll) * a4 + cos(roll) * b5;
	*x1 = 640.0 + 640.0 * a6 / c5;
	*y1 = 400.0 + 640.0 * b6 / c5;
}

static void scene_pixel(const scene *s, double t, double x, double y, int w, int h, double rgb[3])
{
	switch (s->kind) {
	case S_TURN:
		if (hud(x, y, w, h, rgb))
			return;
		sphere_pixel(t, x, y, rgb);
		return;
	case S_CAMERA: {
		if (hud(x, y, w, h, rgb))
			return;
		double cx, cy;
		camera_pos(t, &cx, &cy);
		background(x + cx, y + cy, 1, rgb);
		return;
	}
	case S_STAIRS:
		stairs(x - s->vx * t, y - s->vy * t, rgb);
		return;
	case S_DOLLY:
	case S_DOLLY_STAIRS: {
		// Walking forward (up the stairs): the image grows by 5% per frame about a point above
		// the centre, so what is near the camera (bottom, sides) moves fastest and leaves the
		// frame.
		const double cx = 640, cy = 300, k = 1.0 + 0.05 * t;
		if (s->kind == S_DOLLY_STAIRS)
			stairs(cx + (x - cx) / k, cy + (y - cy) / k, rgb);
		else
			background(cx + (x - cx) / k, cy + (y - cy) / k, 3, rgb);
		return;
	}
	case S_CLIMB: {
		// Climbing wide stone steps: a ground plane in perspective under a far wall. Pale
		// treads and dark risers, almost flat, so only their edges carry motion; the camera
		// moves forward an eighth of a step per frame (walking pace at 40 FPS): 12 px at the
		// bottom edge (96 px steps), 1.5 px where the steps start (y = 380, 12 px steps).
		const double hor = 150, K = 8802;
		if (y < 380) {  // the far wall, above the steps
			background(x, y, 9, rgb);
			return;
		}
		const double z = K / (y - hor) + 0.25 * t, u = (x - 640) * z / K;
		double ph = fmod(z, 2.0);
		if (ph < 0)
			ph += 2.0;
		const double n = 0.04 * vnoise(u * 40.0, z * 6.0, 21) + 0.03 * vnoise(u * 160.0, z * 25.0, 22);
		const double v = ph < 1.55 ? 0.82 : (ph < 1.7 ? 0.55 : 0.16);
		rgb[0] = v + n;
		rgb[1] = 0.97 * v + n;
		rgb[2] = 0.92 * v + n;
		return;
	}
	case S_NEAR: {
		// A character close to the camera crossing fast, cut off by the bottom of the frame,
		// over a slower background.
		const double ox = 700 - 70 * t, oy = 640;
		const double dx = (x - ox) / 170.0, dy = (y - oy) / 260.0;
		if (dx * dx + dy * dy < 1.0)
			background(x - ox + 3000, y - oy, 13, rgb);
		else
			background(x - s->vx * t, y - s->vy * t, 1, rgb);
		return;
	}
	case S_STATIC:
		background(x, y, 1, rgb);
		return;
	case S_CUT:
		background(x, y, t < 0.5 ? 1 : 50, rgb);
		return;
	case S_PAN:
	case S_FASTPAN:
		background(x - s->vx * t, y - s->vy * t, 1, rgb);
		return;
	case S_OCCLUSION: {
		background(x - s->vx * t, y - s->vy * t, 1, rgb);
		double ox = 300 + 40 * t, oy = 260;
		if (x >= ox && x < ox + 160 && y >= oy && y < oy + 160)
			background((x - ox) * 1.3 + 2000, (y - oy) * 1.3, 7, rgb);
		return;
	}
	case S_OBJECTS:
	default: {
		if (hud(x, y, w, h, rgb))
			return;
		background(x, y, 1, rgb);
		double ox = 300 + 36 * t, oy = 320 - 12 * t;
		if (x >= ox && x < ox + 120 && y >= oy && y < oy + 120) {
			background((x - ox) * 1.5 + 5000, (y - oy) * 1.5, 11, rgb);
			return;
		}
		double dx = x - (850 - 28 * t), dy = y - (450 + 20 * t);
		if (dx * dx + dy * dy < 55 * 55) {
			double a = atan2(dy, dx);
			double stripe = fmod(a * 6 / 3.14159265 + 12, 2.0) < 1.0 ? 1.0 : 0.3;
			rgb[0] = 0.9 * stripe;
			rgb[1] = 0.3 * stripe;
			rgb[2] = 0.3;
		}
		return;
	}
	}
}


static void render(const scene *s, double t, int w, int h, uint8_t *out)
{
	// The camera sequence keeps its frame count and moves faster instead (camera_pos).
	if (s->kind != S_CAMERA && s->kind != S_TURN)
		t *= g_timescale;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			double acc[3] = { 0, 0, 0 };
			for (int sy = 0; sy < 2; sy++)
				for (int sx = 0; sx < 2; sx++) {
					double c[3];
					scene_pixel(s, t, (x + 0.25 + 0.5 * sx) / g_geo, (y + 0.25 + 0.5 * sy) / g_geo, (int)lrint(w / g_geo), (int)lrint(h / g_geo), c);
					for (int i = 0; i < 3; i++)
						acc[i] += c[i] * 0.25;
				}
			uint8_t *p = out + 4 * ((size_t)y * w + x);
			for (int i = 0; i < 3; i++) {
				double v = acc[i] * 255.0 + 0.5;
				p[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
			}
			p[3] = 255;
		}
}

static double psnr_region(const uint8_t *a, const uint8_t *b, int w, int h, int x0, int y0, int x1, int y1)
{
	double se = 0;
	long n = 0;
	if (g_geo != 1.0) {
		x0 = (int)lrint(x0 * g_geo); y0 = (int)lrint(y0 * g_geo);
		x1 = (int)lrint(x1 * g_geo); y1 = (int)lrint(y1 * g_geo);
	}
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > w) x1 = w;
	if (y1 > h) y1 = h;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			for (int c = 0; c < 3; c++) {
				double d = (double)a[4 * ((size_t)y * w + x) + c] - b[4 * ((size_t)y * w + x) + c];
				se += d * d;
				n++;
			}
	if (!n)
		return 0;
	double mse = se / (double)n;
	return mse <= 1e-12 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
}

// Screen border metric, independent of the layout scaling used by object regions.
static double psnr_border(const uint8_t *a, const uint8_t *b, int w, int h)
{
	double se = 0;
	long n = 0;
	const int band = (w < h ? w : h) / 20;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			if (x >= band && x < w - band && y >= band && y < h - band)
				continue;
			for (int c = 0; c < 3; c++) {
				double d = (double)a[4 * ((size_t)y * w + x) + c] - b[4 * ((size_t)y * w + x) + c];
				se += d * d;
				n++;
			}
		}
	return !n || se <= 1e-12 ? 99.0 : 10.0 * log10(255.0 * 255.0 * n / se);
}

static double mean_abs(const uint8_t *a, const uint8_t *b, int w, int h, int x0, int y0, int x1, int y1)
{
	double s = 0;
	long n = 0;
	if (g_geo != 1.0) {
		x0 = (int)lrint(x0 * g_geo); y0 = (int)lrint(y0 * g_geo);
		x1 = (int)lrint(x1 * g_geo); y1 = (int)lrint(y1 * g_geo);
	}
	if (x1 > w) x1 = w;
	if (y1 > h) y1 = h;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			for (int c = 0; c < 3; c++) {
				s += fabs((double)a[4 * ((size_t)y * w + x) + c] - b[4 * ((size_t)y * w + x) + c]);
				n++;
			}
	return n ? s / (double)n : 0;
}

// Temporal error: how much the change from the previous output to this one differs from the
// true change, per pixel and channel (flicker and shimmer show up here, a static blur does not).
static double temporal_err(const uint8_t *o, const uint8_t *op, const uint8_t *t, const uint8_t *tp, int w, int h)
{
	double sum = 0;
	for (int i = 0; i < w * h; i++)
		for (int c = 0; c < 3; c++) {
			const int k = i * 4 + c;
			sum += fabs((double)(o[k] - op[k]) - (double)(t[k] - tp[k]));
		}
	return sum / (3.0 * w * h);
}

static int max_abs(const uint8_t *a, const uint8_t *b, size_t n)
{
	int m = 0;
	for (size_t i = 0; i < n; i++) {
		if ((i & 3) == 3)
			continue;
		int d = abs((int)a[i] - (int)b[i]);
		if (d > m)
			m = d;
	}
	return m;
}

static void write_ppm(const char *dir, const char *name, const uint8_t *rgba, int w, int h)
{
	if (!dir)
		return;
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (size_t i = 0; i < (size_t)w * h; i++)
		fwrite(rgba + 4 * i, 1, 3, f);
	fclose(f);
}

// ---- engine resources ---------------------------------------------------------------------
typedef struct engine {
	kfg_vk *vk;
	uint32_t w, h;
	kfg_vk_image color[KFG_RING];
	kfg_vk_image luma[KFG_RING][KFG_LEVELS + 1];
	kfg_vk_image flow[KFG_RING][KFG_LEVELS + 1];
	kfg_vk_image stats[KFG_RING];
	kfg_vk_image select[2];
	kfg_vk_image pair_select[KFG_RING];
	kfg_vk_image output;
} engine;

static const kfg_vk_image *resolve(void *user, kfg_image_ref r)
{
	engine *e = user;
	switch (r.role) {
	case KFG_ROLE_COLOR: return &e->color[r.ring];
	case KFG_ROLE_LUMA: return &e->luma[r.ring][r.level];
	case KFG_ROLE_FLOW: return &e->flow[r.ring][r.level];
	case KFG_ROLE_STATS: return &e->stats[r.ring];
	case KFG_ROLE_SELECT: return &e->select[r.ring & 1];
	case KFG_ROLE_PAIR_SELECT: return &e->pair_select[r.ring];
	case KFG_ROLE_OUTPUT: return &e->output;
	default: return NULL;
	}
}

// color_fmt: the colour ring and the output (R8G8B8A8_UNORM for SDR, R16G16B16A16_SFLOAT for HDR).
static int engine_init(engine *e, kfg_vk *vk, uint32_t w, uint32_t h, VkFormat color_fmt)
{
	memset(e, 0, sizeof(*e));
	e->vk = vk;
	e->w = w;
	e->h = h;
	uint32_t asz[2];
	kfg_analysis_size(w, h, asz);  // luma, flow and fields work at the analysis size
	for (uint32_t r = 0; r < KFG_RING; r++) {
		if (kfg_vk_image_create(vk, &e->color[r], color_fmt, w, h))
			return -1;
		for (uint32_t l = 1; l <= KFG_LEVELS; l++) {
			uint32_t sz[2];
			kfg_level_size(asz[0], asz[1], l, sz);
			if (kfg_vk_image_create(vk, &e->luma[r][l], kfg_vk_format(KFG_FORMAT_R8), sz[0], sz[1]))
				return -1;
			if (l >= 2 && kfg_vk_image_create(vk, &e->flow[r][l], kfg_vk_format(KFG_FORMAT_RGBA16F), sz[0], sz[1]))
				return -1;
		}
		if (kfg_vk_image_create(vk, &e->stats[r], kfg_vk_format(KFG_FORMAT_RGBA16F), 7, 1))
			return -1;
	}
	uint32_t bs[2];
	kfg_select_size(asz[0], asz[1], bs);
	for (int i = 0; i < 2; i++)
		if (kfg_vk_image_create(vk, &e->select[i], kfg_vk_format(KFG_FORMAT_RGBA16F), bs[0], bs[1]))
			return -1;
	for (uint32_t r = 0; r < KFG_RING; r++)
		if (kfg_vk_image_create(vk, &e->pair_select[r], kfg_vk_format(KFG_FORMAT_RGBA16F), bs[0], bs[1]))
			return -1;
	return kfg_vk_image_create(vk, &e->output, color_fmt, w, h);
}

static void engine_destroy(engine *e)
{
	for (uint32_t r = 0; r < KFG_RING; r++) {
		kfg_vk_image_destroy(e->vk, &e->color[r]);
		for (uint32_t l = 0; l <= KFG_LEVELS; l++) {
			kfg_vk_image_destroy(e->vk, &e->luma[r][l]);
			kfg_vk_image_destroy(e->vk, &e->flow[r][l]);
		}
		kfg_vk_image_destroy(e->vk, &e->stats[r]);
	}
	kfg_vk_image_destroy(e->vk, &e->select[0]);
	kfg_vk_image_destroy(e->vk, &e->select[1]);
	for (uint32_t r = 0; r < KFG_RING; r++)
		kfg_vk_image_destroy(e->vk, &e->pair_select[r]);
	kfg_vk_image_destroy(e->vk, &e->output);
}

static int run_plan(engine *e, const kfg_plan *p, double *times, uint32_t *ntimes)
{
	kfg_vk_begin(e->vk);
	if (kfg_vk_record(e->vk, p, resolve, e))
		return -1;
	if (kfg_vk_submit_wait(e->vk))
		return -1;
	if (times && ntimes)
		*ntimes = kfg_vk_pass_times(e->vk, times, KFG_MAX_PASSES);
	return 0;
}

// ---- HDR ----------------------------------------------------------------------------------
// The scenes again as HDR games present them: the SDR picture as display light (gamma 2.2,
// white = 600 nits), encoded as HDR10 PQ code values or as linear scRGB (1.0 = 80 nits, white
// = 7.5), in float colour frames as Gamescope keeps them for HDR. Outputs are compared in a
// perceptual 0..255 scale (PQ, 1000 nits = 255).
#define HDR_WHITE_NITS 600.0

static double pq_encode(double y)  // y in units of 10000 nits
{
	const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	const double p = pow(y < 0 ? 0 : y > 1 ? 1 : y, m1);
	return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

static uint16_t float_to_half(float f)
{
	uint32_t x;
	memcpy(&x, &f, 4);
	const uint32_t s = (x >> 16) & 0x8000u;
	int e = (int)((x >> 23) & 0xff) - 127 + 15;
	uint32_t m = x & 0x7fffffu;
	if (e <= 0)
		return (uint16_t)s;  // flush tiny values
	if (e >= 31)
		return (uint16_t)(s | 0x7c00u);
	m += 0x1000u;  // round to nearest
	if (m & 0x800000u) {
		m = 0;
		if (++e >= 31)
			return (uint16_t)(s | 0x7c00u);
	}
	return (uint16_t)(s | ((uint32_t)e << 10) | (m >> 13));
}

static float half_round(float f)  // the value a half-float image stores for f
{
	const uint16_t h = float_to_half(f);
	const uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 0x1fu, m = h & 0x3ffu;
	if (e == 0)
		return 0.0f;
	const uint32_t x = s | ((e + 112u) << 23) | (m << 13);
	float out;
	memcpy(&out, &x, 4);
	return out;
}

// SDR 8-bit picture -> HDR float RGBA (encoded as the transfer stores it, rounded to half floats
// so references equal what the colour frames hold).
static void to_hdr(const uint8_t *sdr, float *out, size_t npx, int transfer)
{
	for (size_t i = 0; i < npx; i++) {
		for (int c = 0; c < 3; c++) {
			const double nits = pow(sdr[4 * i + c] / 255.0, 2.2) * HDR_WHITE_NITS;
			out[4 * i + c] = half_round((float)(transfer == KFG_TRANSFER_PQ ? pq_encode(nits / 10000.0) : nits / 80.0));
		}
		out[4 * i + 3] = 1.0f;
	}
}

// HDR float -> perceptual 8-bit (PQ code, 1000 nits = 255) for PSNR-style comparisons.
static void hdr_view(const float *in, uint8_t *out, size_t npx, int transfer)
{
	for (size_t i = 0; i < npx; i++) {
		for (int c = 0; c < 3; c++) {
			const double pq = transfer == KFG_TRANSFER_PQ ? in[4 * i + c] : pq_encode(in[4 * i + c] * 80.0 / 10000.0);
			const double v = pq / 0.7518 * 255.0 + 0.5;
			out[4 * i + c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
		}
		out[4 * i + 3] = 255;
	}
}

static int upload_hdr(kfg_vk *vk, kfg_vk_image *img, const float *rgba, uint16_t *tmp)
{
	const size_t n = (size_t)img->width * img->height * 4;
	for (size_t i = 0; i < n; i++)
		tmp[i] = float_to_half(rgba[i]);
	return kfg_vk_upload_raw(vk, img, tmp, n * 2);
}

// Same gates as the SDR scenes; also: static HDR values (above 1.0 in scRGB) come back exactly.
static int hdr_checks(kfg_vk *vk, int w, int h, int transfer, FILE *js)
{
	engine e;
	if (engine_init(&e, vk, (uint32_t)w, (uint32_t)h, VK_FORMAT_R16G16B16A16_SFLOAT)) {
		printf("hdr %s: engine init failed\n", kfg_transfer_names[transfer]);
		return 1;
	}
	const size_t npx = (size_t)w * h, nbytes = npx * 4;
	uint8_t *A8 = malloc(nbytes), *B8 = malloc(nbytes), *T8 = malloc(nbytes);
	uint8_t *A = malloc(nbytes), *B = malloc(nbytes), *T = malloc(nbytes), *G = malloc(nbytes), *L = malloc(nbytes), *D = malloc(nbytes);
	float *Af = malloc(nbytes * sizeof(float)), *Bf = malloc(nbytes * sizeof(float)), *Gf = malloc(nbytes * sizeof(float));
	uint16_t *tmp = malloc(nbytes * 2);
	kfg_tuning tune = kfg_default_tuning();
	tune.transfer = transfer;
	const int32_t flags = kfg_transfer_flags(transfer);
	static const int kinds[] = { 0, 2, 3, 4, 5 };  // pan, objects_hud, occlusion, static, scene_cut
	int failures = 0;
	double worst_gain = 1e9, static_float_err = 0;
	if (js)
		fprintf(js, " \"hdr_%s\": {\"scenes\": [", kfg_transfer_names[transfer]);
	for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
		const scene *s = &SCENES[kinds[k]];
		render(s, 0.0, w, h, A8);
		render(s, 1.0, w, h, B8);
		to_hdr(A8, Af, npx, transfer);
		to_hdr(B8, Bf, npx, transfer);
		hdr_view(Af, A, npx, transfer);
		hdr_view(Bf, B, npx, transfer);
		upload_hdr(vk, &e.color[0], Af, tmp);
		upload_hdr(vk, &e.color[1], Bf, tmp);
		kfg_plan p;
		kfg_plan_analyze(&p, (uint32_t)w, (uint32_t)h, 1, 0, 0, 0, -1, &tune);
		run_plan(&e, &p, NULL, NULL);
		kfg_plan_analyze(&p, (uint32_t)w, (uint32_t)h, 0, 1, 1, 1, -1, &tune);
		run_plan(&e, &p, NULL, NULL);
		float st[28];
		kfg_vk_download_rgba16f(vk, &e.stats[1], st);
		bool ok = true;
		const char *why = "";
		if (s->cut != (st[0] > 0.5)) {
			ok = false;
			why = s->cut ? "scene cut not detected" : "false scene cut";
		}
		double gain = 99;
		for (int pi = 0; pi < 2; pi++) {
			const double t = (pi + 1) / 3.0;
			kfg_plan_generate(&p, (uint32_t)w, (uint32_t)h, 0, 1, 1, (float)t, flags);
			run_plan(&e, &p, NULL, NULL);
			kfg_vk_download_rgba16f(vk, &e.output, Gf);
			hdr_view(Gf, G, npx, transfer);
			render(s, t, w, h, T8);
			float *Tf = Gf;  // reuse after G is taken
			to_hdr(T8, Tf, npx, transfer);
			hdr_view(Tf, T, npx, transfer);
			for (size_t i = 0; i < nbytes; i++) {
				L[i] = (uint8_t)lrint((1 - t) * A[i] + t * B[i]);
				D[i] = t < 0.5 ? A[i] : B[i];
			}
			const double pg = psnr_region(G, T, w, h, 0, 0, w, h), pl = psnr_region(L, T, w, h, 0, 0, w, h),
				     pd = psnr_region(D, T, w, h, 0, 0, w, h);
			if (s->dynamic) {
				const double g = fmin(pg - pl, pg - pd);
				if (g < gain)
					gain = g;
				if (g < 3.0) { ok = false; why = "not better than blend/duplicate by 3 dB"; }
			} else if (s->cut) {
				if (max_abs(G, D, nbytes) > 0) { ok = false; why = "scene cut blended"; }
			} else {
				if (max_abs(G, A, nbytes) > 1) { ok = false; why = "static scene changed"; }
				// The float values themselves, highlights above 1.0 included (scRGB).
				kfg_vk_download_rgba16f(vk, &e.output, Gf);
				for (size_t i = 0; i < nbytes; i++) {
					if ((i & 3) == 3)
						continue;
					const double d = fabs(Gf[i] - Af[i]) / fmax(1.0, fabs(Af[i]));
					if (d > static_float_err)
						static_float_err = d;
				}
				if (static_float_err > 2e-3) { ok = false; why = "static HDR values changed"; }
			}
		}
		if (s->dynamic && gain < worst_gain)
			worst_gain = gain;
		if (!ok)
			failures++;
		printf("hdr %-5s %-12s cut=%.0f %s%s%s\n", kfg_transfer_names[transfer], s->name, st[0], ok ? "PASS" : "FAIL: ", why,
		       s->dynamic ? "" : "");
		if (s->dynamic)
			printf("          worst gain over blend/duplicate %.2f dB\n", gain);
		if (js)
			fprintf(js, "%s{\"scene\": \"%s\", \"cut_flag\": %.0f, \"gain_db\": %.3f, \"pass\": %s}", k ? ", " : "", s->name, st[0],
				s->dynamic ? gain : 0.0, ok ? "true" : "false");
	}
	if (js)
		fprintf(js, "], \"worst_gain_db\": %.3f, \"static_rel_err\": %.6f, \"failures\": %d},\n", worst_gain, static_float_err, failures);
	free(A8); free(B8); free(T8); free(A); free(B); free(T); free(G); free(L); free(D);
	free(Af); free(Bf); free(Gf); free(tmp);
	engine_destroy(&e);
	return failures;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

// Isolate visibility from motion estimation: constant colours and a known field.
// All four screen edges must select the source that actually contains the sample.
static int border_checks(kfg_vk *vk)
{
	const int w = 64, h = 48;
	engine e;
	if (engine_init(&e, vk, w, h, VK_FORMAT_R8G8B8A8_UNORM))
		return 1;
	uint8_t a[64 * 48 * 4], b[sizeof(a)], out[sizeof(a)];
	for (int i = 0; i < w * h; i++) {
		a[4*i] = 255; a[4*i+1] = a[4*i+2] = 0; a[4*i+3] = 255;
		b[4*i] = b[4*i+1] = 0; b[4*i+2] = b[4*i+3] = 255;
	}
	kfg_vk_upload_rgba8(vk, &e.color[0], a);
	kfg_vk_upload_rgba8(vk, &e.color[1], b);
	uint16_t stats[28] = {0};
	kfg_vk_upload_raw(vk, &e.stats[1], stats, sizeof(stats));
	_Float16 field[16 * 12 * 4];
	int failures = 0;
	for (int axis = 0; axis < 2; axis++) {
		for (int i = 0; i < 16 * 12; i++) {
			field[4*i] = axis == 0 ? 4.0 : 0.0;
			field[4*i+1] = axis == 1 ? 4.0 : 0.0;
			field[4*i+2] = 0.5;
			field[4*i+3] = 0.0;
		}
		kfg_vk_upload_raw(vk, &e.pair_select[1], field, sizeof(field));
		// At 1.5 pixels from the last centre, a phase sweep crosses the border.
		int previous = -1;
		for (int step = 1; step < 64; step++) {
			float t = step / 64.0f;
			kfg_plan p;
			kfg_plan_generate(&p, w, h, 0, 1, 1, t, 0);
			run_plan(&e, &p, NULL, NULL);
			kfg_vk_download_rgba8(vk, &e.output, out);
			int lo = axis == 0 ? (h/2)*w : w/2;
			int hi = axis == 0 ? (h/2)*w+w-1 : (h-1)*w+w/2;
			if (out[4*lo+2] != 255 || out[4*lo] != 0 || out[4*hi] != 255 || out[4*hi+2] != 0)
				failures++;
			int near = axis == 0 ? hi-1 : hi-w;
			int value = out[4*near+2];
			if (previous >= 0 && abs(value-previous) > 16)
				failures++;
			previous = value;
		}
	}
	engine_destroy(&e);
	printf("border visibility and phase continuity: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
	return failures;
}

int main(int argc, char **argv)
{
	const char *spv = "build";
	const char *out_dir = NULL;
	const char *json_path = NULL;
	const char *only = NULL;   // run one scene (tuning); skips the sequence and stairs checks
	int w = 1280, h = 800, bench = 0;
	bool validation = getenv("KFG_VALIDATION") != NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--spv") && i + 1 < argc) spv = argv[++i];
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
		else if (!strcmp(argv[i], "--json") && i + 1 < argc) json_path = argv[++i];
		else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
		else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--scene") && i + 1 < argc) only = argv[++i];
		else if (!strcmp(argv[i], "--no-temporal")) g_temporal = 0;
		else if (!strcmp(argv[i], "--lowbase")) { g_lowbase = true; g_timescale = 1.25; }
		else if (!strcmp(argv[i], "--per-phase")) kfg_per_phase_select = 1;  // 1.2 selection, for A/B
		else if (!strcmp(argv[i], "--scaled")) g_geo = -1.0;  // set from --size below
		else if (!strcmp(argv[i], "--analysis-pixels") && i + 1 < argc) kfg_analysis_pixels = (uint32_t)atoi(argv[++i]);  // 0 = full size
		else {
			fprintf(stderr, "usage: kfg_vktest [--spv DIR] [--size WxH] [--out DIR] [--json FILE] [--bench N] [--scene NAME] [--no-temporal] [--lowbase] [--per-phase] [--scaled] [--analysis-pixels N]\n");
			return 2;
		}
	}
	if (g_geo < 0)
		g_geo = h / 800.0;
	kfg_vk vk;
	if (kfg_vk_init(&vk, spv, validation)) {
		fprintf(stderr, "Vulkan init failed\n");
		return 1;
	}
	engine e;
	if (engine_init(&e, &vk, (uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_UNORM)) {
		fprintf(stderr, "engine init failed\n");
		return 1;
	}
	kfg_tuning tune = kfg_default_tuning();
	size_t nbytes = (size_t)w * h * 4;
	uint8_t *A = malloc(nbytes), *B = malloc(nbytes), *T = malloc(nbytes), *G = malloc(nbytes), *L = malloc(nbytes),
		*D = malloc(nbytes), *G2 = malloc(nbytes);
	FILE *js = json_path ? fopen(json_path, "w") : NULL;
	if (js)
		fprintf(js, "{\n \"device\": \"%s\",\n \"driver\": \"%s\",\n \"size\": [%d, %d],\n \"scenes\": [\n", vk.device_name, vk.driver_info, w, h);
	int failures = 0;
	printf("device: %s (%s)\n", vk.device_name, vk.driver_info);

	const double phases_x3[] = { 1.0 / 3.0, 2.0 / 3.0, 0.5 };
	const double phases_lb[] = { 4.0 / 15.0, 11.0 / 15.0, 7.0 / 15.0 };
	const double *phases = g_lowbase ? phases_lb : phases_x3;
	for (size_t si = 0; si < sizeof(SCENES) / sizeof(SCENES[0]); si++) {
		scene sc = SCENES[si];
		if (getenv("KFG_PANV") && sc.kind == S_PAN)
			sscanf(getenv("KFG_PANV"), "%lf,%lf", &sc.vx, &sc.vy);  // tuning aid: pan speed override
		const scene *s = &sc;
		if (only && strcmp(only, s->name))
			continue;
		render(s, 0.0, w, h, A);
		render(s, 1.0, w, h, B);
		kfg_vk_upload_rgba8(&vk, &e.color[0], A);
		kfg_vk_upload_rgba8(&vk, &e.color[1], B);
		kfg_plan p;
		kfg_plan_analyze(&p, (uint32_t)w, (uint32_t)h, 1, 0, 0, 0, -1, &tune); // pyramid of A (ring 0)
		run_plan(&e, &p, NULL, NULL);
		kfg_plan_analyze(&p, (uint32_t)w, (uint32_t)h, 0, 1, 1, 1, -1, &tune); // pair A(0)->B(1), pair ring 1
		run_plan(&e, &p, NULL, NULL);
		float st[28];  // stats is 7x1
		kfg_vk_download_rgba16f(&vk, &e.stats[1], st);
		if (getenv("KFG_FLOWDUMP")) {
			// Tuning aid: the most common forward vectors per level (pixels, rounded).
			for (int l = KFG_LEVELS; l >= 2; l--) {
				const kfg_vk_image *fi = &e.flow[1][l];
				float *fl = malloc((size_t)fi->width * fi->height * 4 * sizeof(float));
				kfg_vk_download_rgba16f(&vk, fi, fl);
				int best[3] = {0}, bx[3] = {0}, by[3] = {0};
				static int hist[129][129];
				memset(hist, 0, sizeof(hist));
				for (uint32_t i = 0; i < fi->width * fi->height; i++) {
					int x = (int)lrintf(fl[i * 4]) + 64, y = (int)lrintf(fl[i * 4 + 1]) + 64;
					if (x >= 0 && x <= 128 && y >= 0 && y <= 128)
						hist[y][x]++;
				}
				for (int y = 0; y <= 128; y++)
					for (int x = 0; x <= 128; x++)
						for (int k = 0; k < 3; k++)
							if (hist[y][x] > best[k]) {
								for (int j = 2; j > k; j--) { best[j] = best[j - 1]; bx[j] = bx[j - 1]; by[j] = by[j - 1]; }
								best[k] = hist[y][x]; bx[k] = x - 64; by[k] = y - 64;
								break;
							}
				printf("  level %d: (%d,%d) %.0f%%  (%d,%d) %.0f%%  (%d,%d) %.0f%%\n", l,
				       bx[0], by[0], 100.0 * best[0] / (fi->width * fi->height), bx[1], by[1], 100.0 * best[1] / (fi->width * fi->height),
				       bx[2], by[2], 100.0 * best[2] / (fi->width * fi->height));
				free(fl);
			}
		}
		if (getenv("KFG_VERBOSE") && (s->kind == S_DOLLY || s->kind == S_DOLLY_STAIRS || s->kind == S_CLIMB)) {
			// Share of blocks whose forward vector is within 2 / 8 pixels of the true zoom field.
			for (int l = KFG_LEVELS; l >= 2; l--) {
				const kfg_vk_image *fi = &e.flow[1][l];
				float *fl = malloc((size_t)fi->width * fi->height * 4 * sizeof(float));
				kfg_vk_download_rgba16f(&vk, fi, fl);
				int ok2 = 0, ok8 = 0, n = 0;
				const int nb = (int)(fi->width * fi->height);
				const double bs = (double)(1 << l);
				double band_v[20] = {0}, band_t[20] = {0};
				int band_n[20] = {0};
				for (int i = 0; i < nb; i++) {
					double px = (i % fi->width + 0.5) * bs, py = (i / fi->width + 0.5) * bs;
					double tx = 0.05 * (px - 640), ty = 0.05 * (py - 300);
					if (s->kind == S_CLIMB) {
						if (py < 380) continue;
						const double z0 = 8802 / (py - 150), z1 = z0 - 0.25;
						tx = (px - 640) * z0 / z1 - (px - 640);
						ty = 150 + 8802 / z1 - py;
					}
					// On the steps only the vertical motion is observable (horizontal bands).
					double d = s->kind == S_CLIMB ? fabs(fl[i * 4 + 1] - ty) : hypot(fl[i * 4] - tx, fl[i * 4 + 1] - ty);
					ok2 += d < 2.0;
					ok8 += d < 8.0;
					n++;
					const int b = (int)(py / 40.0);
					if (b < 20) { band_v[b] += fl[i * 4 + 1]; band_t[b] += ty; band_n[b]++; }
				}
				printf("  %s level %d: %.0f%% within 2 px, %.0f%% within 8 px\n", s->name, l, 100.0 * ok2 / n, 100.0 * ok8 / n);
				if (l == 2) {
					printf("    rows: mean vy / true vy:");
					for (int b = 0; b < 20; b++)
						if (band_n[b])
							printf(" %d:%.1f/%.1f", b * 40, band_v[b] / band_n[b], band_t[b] / band_n[b]);
					printf("\n");
				}
				free(fl);
			}
		}
		if (js)
			fprintf(js, "  {\"scene\": \"%s\", \"dynamic\": %s, \"cut_flag\": %.0f, \"bad_fraction\": %.4f, \"phases\": [\n",
				s->name, s->dynamic ? "true" : "false", st[0], st[1]);
		// The camera map's translation (pixels) and perspective terms (texels 1-2: h0-1 h1 h2 h3, h4-1 h5 h6 h7).
		printf("%-12s cut=%.0f bad=%.3f mc=%.4f zero=%.4f global=(%.1f,%.1f) zoom=(%.3f,%.3f) persp=(%.4f,%.4f)\n", s->name, st[0], st[1], st[2], st[3],
		       st[6] * 0.5 * w, st[9] * 0.5 * w, st[4], st[8], st[10], st[11]);
		if (s->cut && st[0] < 0.5) {
			printf("  FAIL: scene cut not detected\n");
			failures++;
		}
		if (!s->cut && st[0] > 0.5) {
			printf("  FAIL: false scene cut\n");
			failures++;
		}
		for (size_t pi = 0; pi < 3; pi++) {
			double t = phases[pi];
			kfg_plan_generate(&p, (uint32_t)w, (uint32_t)h, 0, 1, 1, (float)t, 0);
			run_plan(&e, &p, NULL, NULL);
			kfg_vk_download_rgba8(&vk, &e.output, G);
			if (pi == 0)
				memcpy(G2, G, nbytes);
			render(s, t, w, h, T);
			for (size_t i = 0; i < nbytes; i++) {
				L[i] = (uint8_t)lrint((1 - t) * A[i] + t * B[i]);
				D[i] = t < 0.5 ? A[i] : B[i];
			}
			double pg = psnr_region(G, T, w, h, 0, 0, w, h);
			double pl = psnr_region(L, T, w, h, 0, 0, w, h);
			double pd = psnr_region(D, T, w, h, 0, 0, w, h);
			double ma = mean_abs(G, A, w, h, 0, 0, w, h), mb = mean_abs(G, B, w, h, 0, 0, w, h);
			// Region of moving objects (objects scene) for a local measure.
			double og = pg, ol = pl, od = pd;
			if (s->kind == S_OBJECTS) {
				og = psnr_region(G, T, w, h, 280, 280, 480, 460);
				ol = psnr_region(L, T, w, h, 280, 280, 480, 460);
				od = psnr_region(D, T, w, h, 280, 280, 480, 460);
			} else if (s->kind == S_OCCLUSION) {
				og = psnr_region(G, T, w, h, 280, 240, 520, 440);
				ol = psnr_region(L, T, w, h, 280, 240, 520, 440);
				od = psnr_region(D, T, w, h, 280, 240, 520, 440);
			}
			else if (s->kind == S_DOLLY) {
				og = psnr_region(G, T, w, h, 0, 600, w, h);
				ol = psnr_region(L, T, w, h, 0, 600, w, h);
				od = psnr_region(D, T, w, h, 0, 600, w, h);
			} else if (s->kind == S_NEAR) {
				og = psnr_region(G, T, w, h, 380, 380, 900, h);
				ol = psnr_region(L, T, w, h, 380, 380, 900, h);
				od = psnr_region(D, T, w, h, 380, 380, 900, h);
			}
			double hud_err = s->kind == S_OBJECTS ? mean_abs(G, T, w, h, 40, 30, 360, 70) : 0.0;
			int maxd = max_abs(G, s->cut ? D : A, nbytes);
			bool ok = true;
			const char *why = "";
			if (s->dynamic) {
				if (!(pg >= pl + 3.0 && pg >= pd + 3.0)) { ok = false; why = "not better than blend/duplicate by 3 dB"; }
				if (!(og >= ol + 2.0 && og >= od + 2.0)) { ok = false; why = "moving region not better than blend/duplicate by 2 dB"; }
				if (!(ma > 0.5 && mb > 0.5)) { ok = false; why = "output equals a source frame"; }
				if (hud_err > 2.0) { ok = false; why = "HUD distorted"; }
			} else if (s->cut) {
				if (maxd > 0) { ok = false; why = "scene cut blended"; }
			} else {
				if (maxd > 1) { ok = false; why = "static scene changed"; }
			}
			// The climb (big steps under perspective, as in the 1.1.1 report) is an open
			// problem: measured and reported, not yet a gate.
			const bool known = s->kind == S_CLIMB;
			if (!ok && !known)
				failures++;
			printf("  t=%.3f gen=%.2fdB blend=%.2fdB dup=%.2fdB region gen/blend/dup=%.2f/%.2f/%.2f |G-A|=%.2f |G-B|=%.2f hud=%.2f %s%s\n",
			       t, pg, pl, pd, og, ol, od, ma, mb, hud_err, ok ? "PASS" : known ? "OPEN (not gated): " : "FAIL: ", why);
			if (js)
				fprintf(js, "   {\"t\": %.6f, \"psnr_gen\": %.3f, \"psnr_border\": %.3f, \"psnr_blend\": %.3f, \"psnr_dup\": %.3f, \"region_gen\": %.3f, \"region_blend\": %.3f, \"region_dup\": %.3f, \"mean_abs_vs_A\": %.3f, \"mean_abs_vs_B\": %.3f, \"hud_err\": %.3f, \"pass\": %s}%s\n",
					t, pg, psnr_border(G, T, w, h), pl, pd, og, ol, od, ma, mb, hud_err, ok ? "true" : "false", pi < 2 ? "," : "");
			char name[128];
			snprintf(name, sizeof(name), "%s_t%03d_gen", s->name, (int)lrint(t * 1000));
			write_ppm(out_dir, name, G, w, h);
			snprintf(name, sizeof(name), "%s_t%03d_truth", s->name, (int)lrint(t * 1000));
			write_ppm(out_dir, name, T, w, h);
			if (pi == 1 && s->dynamic) {
				// x3: the two intermediates must differ from each other.
				double d12 = mean_abs(G, G2, w, h, 0, 0, w, h);
				if (d12 < 0.5) {
					printf("  FAIL: x3 intermediates identical\n");
					failures++;
				}
			}
		}
		if (js)
			fprintf(js, "  ]}%s\n", si + 1 < sizeof(SCENES) / sizeof(SCENES[0]) ? "," : "");
	}
	if (js)
		fprintf(js, " ],\n");

	// Running sequence through the ring: 8 frames of the objects scene, x3 outputs per pair.
	if (!only) {
		const scene *s = &SCENES[2];
		int seq_fail = 0;
		for (int f = 0; f < 8; f++) {
			uint32_t fb = (uint32_t)f % KFG_RING, fa = (uint32_t)(f + KFG_RING - 1) % KFG_RING;
			render(s, (double)f, w, h, B);
			kfg_vk_upload_rgba8(&vk, &e.color[fb], B);
			kfg_plan p;
			kfg_plan_analyze(&p, (uint32_t)w, (uint32_t)h, fa, fb, fb, f > 0, f > 1 && g_temporal ? (int)fa : -1, &tune);
			run_plan(&e, &p, NULL, NULL);
			if (f == 0)
				continue;
			for (int j = 1; j < PHASES_PER_PAIR; j++) {
				double t = j * PHASE_STEP;
				kfg_plan_generate(&p, (uint32_t)w, (uint32_t)h, fa, fb, fb, (float)t, 0);
				run_plan(&e, &p, NULL, NULL);
				kfg_vk_download_rgba8(&vk, &e.output, G);
				render(s, f - 1 + t, w, h, T);
				render(s, f - 1 + (t < 0.5 ? 0.0 : 1.0), w, h, D);
				double pg = psnr_region(G, T, w, h, 0, 0, w, h), pd = psnr_region(D, T, w, h, 0, 0, w, h);
				if (pg < pd + 3.0)
					seq_fail++;
			}
		}
		printf("sequence: %s (%d bad outputs of 14)\n", seq_fail ? "FAIL" : "PASS", seq_fail);
		failures += seq_fail;
		if (js)
			fprintf(js, " \"sequence_bad_outputs\": %d,\n", seq_fail);
	}

	// Active camera: the accelerating pan under a HUD, x3 outputs per pair, every output
	// compared with the truth and with the one before it. Informative: printed and in the JSON.
	static const scene seqs[] = { { "camera", S_CAMERA, 0, 0, true, false }, { "turn", S_TURN, 0, 0, true, false } };
	for (int sq = 0; sq < 2; sq++) {
		const scene *s = &seqs[sq];
		if (only && strcmp(only, s->name))
			continue;
		const int nframes = s->kind == S_TURN ? TURN_FRAMES : CAM_FRAMES;
		double ps_side = 0;
		uint8_t *O = malloc(nbytes), *Op = malloc(nbytes), *Tp = malloc(nbytes), *Dp = malloc(nbytes);
		double te_gen = 0, te_dup = 0, ps_gen = 0, ps_dup = 0, worst = 1e9, hud_max = 0;
		int n_te = 0, n_ps = 0, cuts = 0;
		bool have_prev = false;
		for (int f = 0; f <= nframes; f++) {
			uint32_t fb = (uint32_t)f % KFG_RING, fa = (uint32_t)(f + KFG_RING - 1) % KFG_RING;
			render(s, (double)f, w, h, B);
			kfg_vk_upload_rgba8(&vk, &e.color[fb], B);
			kfg_plan p;
			kfg_plan_analyze(&p, (uint32_t)w, (uint32_t)h, fa, fb, fb, f > 0, f > 1 && g_temporal ? (int)fa : -1, &tune);
			run_plan(&e, &p, NULL, NULL);
			if (f == 0)
				continue;
			float st[28];  // stats is 7x1
			kfg_vk_download_rgba16f(&vk, &e.stats[fb], st);
			if (st[0] > 0.5)
				cuts++;
			if (getenv("KFG_VERBOSE"))
				printf("  camera f%02d stats cut=%.0f bad=%.3f mc/zero=%.3f global=(%.1f,%.1f)\n", f, st[0], st[1], st[2] / (st[3] + 1e-9), st[6] * 0.5 * w, st[9] * 0.5 * w);
			if (getenv("KFG_VERBOSE") && s->kind == S_TURN) {
				printf("  turn f%02d map fwd", f);
				for (int k = 4; k < 12; k++)
					printf(" %.4f", st[k]);
				printf("\n");
				for (int l = KFG_LEVELS; l >= 2; l--) {
					const kfg_vk_image *fi = &e.flow[fb][l];
					float *fl = malloc((size_t)fi->width * fi->height * 4 * sizeof(float));
					kfg_vk_download_rgba16f(&vk, fi, fl);
					int ok2 = 0, ok8 = 0, n = (int)(fi->width * fi->height), side2 = 0, sn = 0;
					const double bs = (double)(1 << l);
					for (int i = 0; i < n; i++) {
						double px = (i % fi->width + 0.5) * bs, py = (i / fi->width + 0.5) * bs, qx, qy;
						if (hud(px, py, w, h, (double[3]){0, 0, 0}))
							continue;
						turn_project(f - 1, f, px, py, &qx, &qy);
						double d = hypot(fl[i * 4] - (qx - px), fl[i * 4 + 1] - (qy - py));
						ok2 += d < 2.0;
						ok8 += d < 8.0;
						if (px < 220 || px > w - 220) { side2 += d < 2.0; sn++; }
					}
					printf("  turn f%02d level %d: %.0f%% within 2 px, %.0f%% within 8 px, sides %.0f%% within 2 px\n", f, l, 100.0 * ok2 / n, 100.0 * ok8 / n, 100.0 * side2 / (sn ? sn : 1));
					free(fl);
				}
			}
			if (getenv("KFG_VERBOSE") && s->kind == S_CAMERA) {
				// Share of blocks whose forward vector is within 2 / 8 pixels of the camera motion.
				double tx = -CAM_VX[f - 1], ty = -0.18 * CAM_VX[f - 1] * sin((f - 1) * 1.3);
				for (int l = KFG_LEVELS; l >= 2; l--) {
					const kfg_vk_image *fi = &e.flow[fb][l];
					float *fl = malloc((size_t)fi->width * fi->height * 4 * sizeof(float));
					kfg_vk_download_rgba16f(&vk, fi, fl);
					int ok2 = 0, ok8 = 0, n = (int)(fi->width * fi->height);
					for (int i = 0; i < n; i++) {
						double d = hypot(fl[i * 4] - tx, fl[i * 4 + 1] - ty);
						ok2 += d < 2.0;
						ok8 += d < 8.0;
					}
					printf("  camera f%02d level %d: %.0f%% within 2 px, %.0f%% within 8 px\n", f, l, 100.0 * ok2 / n, 100.0 * ok8 / n);
					if (l == KFG_LEVELS || l == 2) {
						static int hb[51][51];
						memset(hb, 0, sizeof(hb));
						for (int i = 0; i < n; i++) {
							int bx = (int)lrint(fl[i * 4] / 8.0) + 25, by = (int)lrint(fl[i * 4 + 1] / 8.0) + 25;
							if (bx >= 0 && bx <= 50 && by >= 0 && by <= 50)
								hb[by][bx]++;
						}
						printf("    truth (%.0f,%.0f); top:", tx, ty);
						for (int k = 0; k < 4; k++) {
							int m = 0, mx = 0, my = 0;
							for (int y = 0; y <= 50; y++)
								for (int x = 0; x <= 50; x++)
									if (hb[y][x] > m) { m = hb[y][x]; mx = x; my = y; }
							printf(" (%d,%d) %.0f%%", (mx - 25) * 8, (my - 25) * 8, 100.0 * m / n);
							hb[my][mx] = 0;
						}
						printf("\n");
					}
					free(fl);
				}
			}
			for (int j = 0; j < PHASES_PER_PAIR; j++) {
				double t = j * PHASE_STEP;
				render(s, f - 1 + t, w, h, T);
				render(s, f - 1 + (t < 0.5 ? 0.0 : 1.0), w, h, D);
				if (j == 0) {
					render(s, f - 1, w, h, O);
				} else {
					kfg_plan_generate(&p, (uint32_t)w, (uint32_t)h, fa, fb, fb, (float)t, 0);
					run_plan(&e, &p, NULL, NULL);
					kfg_vk_download_rgba8(&vk, &e.output, O);
					double pg = psnr_region(O, T, w, h, 0, 0, w, h);
					ps_gen += pg;
					// The sides of the picture, where a turn moves fastest and bends most.
					ps_side += 0.5 * (psnr_region(O, T, w, h, 0, 80, 220, h - 60) + psnr_region(O, T, w, h, w - 220, 80, w, h - 60));
					ps_dup += psnr_region(D, T, w, h, 0, 0, w, h);
					if (pg < worst)
						worst = pg;
					if (getenv("KFG_VERBOSE"))
						printf("  %s f%02d t=%d/3 gen %.2f dB\n", s->name, f, j, pg);
					double he = mean_abs(O, T, w, h, 40, 30, 360, 70);
					if (he > hud_max)
						hud_max = he;
					n_ps++;
					char name[64];
					snprintf(name, sizeof(name), "%s_f%02d_t%d_gen", s->name, f, j);
					write_ppm(out_dir, name, O, w, h);
					if (getenv("KFG_VERBOSE")) {
						snprintf(name, sizeof(name), "%s_f%02d_t%d_truth", s->name, f, j);
						write_ppm(out_dir, name, T, w, h);
					}
				}
				if (have_prev) {
					te_gen += temporal_err(O, Op, T, Tp, w, h);
					te_dup += temporal_err(D, Dp, T, Tp, w, h);
					n_te++;
				}
				memcpy(Op, O, nbytes);
				memcpy(Tp, T, nbytes);
				memcpy(Dp, D, nbytes);
				have_prev = true;
			}
		}
		printf("%s: psnr gen %.2f dB (worst %.2f, sides %.2f), dup %.2f dB; temporal gen %.3f, dup %.3f; cuts %d of %d; hud max %.2f\n",
		       s->name, ps_gen / n_ps, worst, ps_side / n_ps, ps_dup / n_ps, te_gen / n_te, te_dup / n_te, cuts, nframes, hud_max);
		if (js)
			fprintf(js, " \"%s\": {\"psnr_gen\": %.3f, \"psnr_worst\": %.3f, \"psnr_sides\": %.3f, \"psnr_dup\": %.3f, \"temporal_gen\": %.4f, \"temporal_dup\": %.4f, \"cuts\": %d, \"hud_max\": %.3f},\n",
				s->name, ps_gen / n_ps, worst, ps_side / n_ps, ps_dup / n_ps, te_gen / n_te, te_dup / n_te, cuts, hud_max);
		free(O); free(Op); free(Tp); free(Dp);
	}

	// Temporal stability: 7 frames at x3, every output compared with the one before it, on the
	// stairs and on slow turns (where shimmer of fine texture shows most). Informative: printed
	// and written to the JSON for tuning.
	static const scene TEMPORAL[] = {
		{ "stairs", S_STAIRS, 18, 10, true, false },
		{ "slow_pan", S_PAN, 2.6, 0.8, true, false },
		{ "slow_stairs", S_STAIRS, 3.1, 1.4, true, false },
		{ "climb", S_CLIMB, 0, 0, true, false },
	};
	for (size_t ti = 0; ti < sizeof(TEMPORAL) / sizeof(TEMPORAL[0]); ti++) {
		const scene *s = &TEMPORAL[ti];
		if (only && strcmp(only, s->name))
			continue;
		uint8_t *O = malloc(nbytes), *Op = malloc(nbytes), *Tp = malloc(nbytes), *Dp = malloc(nbytes);
		double te_gen = 0, te_dup = 0, ps_gen = 0, ps_dup = 0;
		int n_te = 0, n_ps = 0;
		bool have_prev = false;
		for (int f = 0; f < 7; f++) {
			uint32_t fb = (uint32_t)f % KFG_RING, fa = (uint32_t)(f + KFG_RING - 1) % KFG_RING;
			render(s, (double)f, w, h, B);
			kfg_vk_upload_rgba8(&vk, &e.color[fb], B);
			kfg_plan p;
			kfg_plan_analyze(&p, (uint32_t)w, (uint32_t)h, fa, fb, fb, f > 0, f > 1 && g_temporal ? (int)fa : -1, &tune);
			run_plan(&e, &p, NULL, NULL);
			if (f == 0)
				continue;
			if (getenv("KFG_VERBOSE")) {
				float st[28];
				kfg_vk_download_rgba16f(&vk, &e.stats[fb], st);
				printf("  %s f%02d map fwd", s->name, f);
				for (int k = 4; k < 12; k++)
					printf(" %.4f", st[k]);
				printf("\n");
			}
			for (int j = 0; j < PHASES_PER_PAIR; j++) {
				double t = j * PHASE_STEP;
				render(s, f - 1 + t, w, h, T);
				render(s, f - 1 + (t < 0.5 ? 0.0 : 1.0), w, h, D);
				if (j == 0) {
					render(s, f - 1, w, h, O);
				} else {
					kfg_plan_generate(&p, (uint32_t)w, (uint32_t)h, fa, fb, fb, (float)t, 0);
					run_plan(&e, &p, NULL, NULL);
					kfg_vk_download_rgba8(&vk, &e.output, O);
					if (f == 3 && j == 1) {
						char name[128];
						snprintf(name, sizeof(name), "%s_seq_gen", s->name);
						write_ppm(out_dir, name, O, w, h);
						snprintf(name, sizeof(name), "%s_seq_truth", s->name);
						write_ppm(out_dir, name, T, w, h);
					}
					ps_gen += psnr_region(O, T, w, h, 0, 0, w, h);
					ps_dup += psnr_region(D, T, w, h, 0, 0, w, h);
					n_ps++;
				}
				if (have_prev) {
					te_gen += temporal_err(O, Op, T, Tp, w, h);
					te_dup += temporal_err(D, Dp, T, Tp, w, h);
					n_te++;
				}
				memcpy(Op, O, nbytes);
				memcpy(Tp, T, nbytes);
				memcpy(Dp, D, nbytes);
				have_prev = true;
			}
		}
		printf("%s temporal: gen %.3f, duplicate %.3f (mean |change error| per channel); psnr gen %.2f dB, dup %.2f dB\n",
		       s->name, te_gen / n_te, te_dup / n_te, ps_gen / n_ps, ps_dup / n_ps);
		if (js)
			fprintf(js, " \"%s_temporal\": {\"gen\": %.4f, \"dup\": %.4f, \"psnr_gen\": %.3f, \"psnr_dup\": %.3f},\n",
				s->name, te_gen / n_te, te_dup / n_te, ps_gen / n_ps, ps_dup / n_ps);
		free(O); free(Op); free(Tp); free(Dp);
	}

	if (!only || !strncmp(only, "hdr", 3)) {
		failures += hdr_checks(&vk, w, h, KFG_TRANSFER_PQ, js);
		failures += hdr_checks(&vk, w, h, KFG_TRANSFER_SCRGB, js);
	}

	if (!only || !strcmp(only, "border"))
		failures += border_checks(&vk);

	// Benchmark: analysis of one pair + two generated outputs (x3), GPU timestamps per pass.
	if (bench > 0 && vk.timestamps) {
		double samples[KFG_MAX_PASSES * 2][512];
		char labels[KFG_MAX_PASSES * 2][32];
		uint32_t npass = 0;
		int reps = bench > 512 ? 512 : bench;
		const scene *s = &SCENES[2];
		const char *bench_scene = getenv("KFG_BENCH_SCENE");
		if (bench_scene) {
			bool found = false;
			for (size_t i = 0; i < sizeof(SCENES) / sizeof(SCENES[0]); i++) {
				if (!strcmp(bench_scene, SCENES[i].name)) { s = &SCENES[i]; found = true; break; }
			}
			if (!found) { fprintf(stderr, "unknown benchmark scene: %s\n", bench_scene); return 2; }
		}
		printf("benchmark scene: %s\n", s->name);
		render(s, 0.0, w, h, A);
		render(s, 1.0, w, h, B);
		kfg_vk_upload_rgba8(&vk, &e.color[0], A);
		kfg_vk_upload_rgba8(&vk, &e.color[1], B);
		// The benchmark may follow a different scene. Build A's pyramid for this
		// pair and initialise the temporal seed before measuring any passes.
		kfg_plan warm;
		kfg_plan_analyze(&warm, (uint32_t)w, (uint32_t)h, 1, 0, 0, 0, -1, &tune);
		if (run_plan(&e, &warm, NULL, NULL)) { fprintf(stderr, "benchmark pyramid init failed\n"); return 1; }
		if (g_temporal) {
			kfg_plan_analyze(&warm, (uint32_t)w, (uint32_t)h, 0, 1, 0, 1, -1, &tune);
			if (run_plan(&e, &warm, NULL, NULL)) { fprintf(stderr, "benchmark temporal seed init failed\n"); return 1; }
		}
		for (int r = 0; r < reps + 3; r++) {
			kfg_plan pa, g1, g2;
			kfg_plan_analyze(&pa, (uint32_t)w, (uint32_t)h, 0, 1, 1, 1, g_temporal ? 0 : -1, &tune);
			kfg_plan_generate(&g1, (uint32_t)w, (uint32_t)h, 0, 1, 1, 1.0f / 3.0f, 0);
			kfg_plan_generate(&g2, (uint32_t)w, (uint32_t)h, 0, 1, 1, 2.0f / 3.0f, 0);
			kfg_vk_begin(&vk);
			kfg_vk_record(&vk, &pa, resolve, &e);
			kfg_vk_record(&vk, &g1, resolve, &e);
			kfg_vk_record(&vk, &g2, resolve, &e);
			kfg_vk_submit_wait(&vk);
			double t[KFG_MAX_PASSES * 2];
			uint32_t n = kfg_vk_pass_times(&vk, t, KFG_MAX_PASSES * 2);
			if (r < 3)
				continue; // warm-up
			npass = n;
			for (uint32_t i = 0; i < n; i++) {
				samples[i][r - 3] = t[i];
				const kfg_plan *src = i < pa.n ? &pa : i < pa.n + g1.n ? &g1 : &g2;
				uint32_t k = i < pa.n ? i : i < pa.n + g1.n ? i - pa.n : i - pa.n - g1.n;
				snprintf(labels[i], sizeof(labels[i]), "%s%s", src == &pa ? "" : src == &g1 ? "g1:" : "g2:", src->pass[k].label);
			}
		}
		double analysis = 0, gen = 0;
		printf("benchmark (%d reps, %s): median GPU ms per pass\n", reps, vk.device_name);
		if (js)
			fprintf(js, " \"bench\": {\"reps\": %d, \"passes\": [", reps);
		for (uint32_t i = 0; i < npass; i++) {
			qsort(samples[i], (size_t)reps, sizeof(double), cmp_double);
			double med = samples[i][reps / 2] / 1e6, p99 = samples[i][(reps * 99) / 100] / 1e6;
			printf("  %-12s %.3f (p99 %.3f)\n", labels[i], med, p99);
			// "global" is an analysis pass; only g1:/g2: belong to generated outputs.
			if (strncmp(labels[i], "g1:", 3) && strncmp(labels[i], "g2:", 3))
				analysis += med;
			else
				gen += med;
			if (js)
				fprintf(js, "%s{\"pass\": \"%s\", \"median_ms\": %.4f, \"p99_ms\": %.4f}", i ? ", " : "", labels[i], med, p99);
		}
		printf("  analysis %.3f ms, two outputs %.3f ms\n", analysis, gen);
		// GPU time per second of play at 90 Hz: x3 (30 pairs, 60 generated frames) and
		// x3.75 (24 pairs, 66 generated frames).
		printf("  per second at 90 Hz: x3 %.1f ms, x3.75 %.1f ms\n", 30 * analysis + 60 * gen / 2, 24 * analysis + 66 * gen / 2);
		if (js)
			fprintf(js, "], \"analysis_ms\": %.4f, \"two_outputs_ms\": %.4f},\n", analysis, gen);
	}
	if (js) {
		fprintf(js, " \"failures\": %d\n}\n", failures);
		fclose(js);
	}
	printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
	engine_destroy(&e);
	kfg_vk_destroy(&vk);
	free(A); free(B); free(T); free(G); free(L); free(D); free(G2);
	return failures ? 1 : 0;
}
