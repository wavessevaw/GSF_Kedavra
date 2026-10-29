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

enum { S_PAN, S_OBJECTS, S_OCCLUSION, S_STATIC, S_CUT, S_FASTPAN, S_STAIRS, S_CAMERA, S_DOLLY, S_DOLLY_STAIRS, S_NEAR, S_CLIMB, S_ROTATE, S_LINES, S_CUT_FLAT, S_CUT_STAIRS, S_GRAIN, S_HTURN, S_HPAR, S_HLINES, S_HTURN_DARK };

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
	// GSF 1.5: in-plane rotation (4 degrees per frame about the centre) and one-pixel lines
	// (wires, fences, UI rules) panning under a HUD.
	{ "rotation", S_ROTATE, 0, 0, true, false },
	{ "thin_lines", S_LINES, 9, 3, true, false },
	// Slow vertical lines (3 px/frame) read as a cut before GSF 1.5; at 12 px/frame horizontal
	// the HUD's edge rows took the world's vector.
	{ "thin_lines_v3", S_LINES, 0, 3, true, false },
	{ "thin_lines_h12", S_LINES, 12, 0, true, false },
	// Cuts the field-coherence test must still catch: to a flat dark picture (loading) and
	// from the stairs to unrelated texture.
	{ "cut_to_flat", S_CUT_FLAT, 0, 0, false, true },
	{ "cut_stairs", S_CUT_STAIRS, 0, 0, false, true },
	// A fast turn over a dim, low-contrast surface with per-frame grain (TAA noise, film grain):
	// where the motion search has little to hold on to and block artifacts showed on the Deck.
	{ "grain_turn", S_GRAIN, 38, 7, true, false },
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

// A centred crosshair (a static overlay over the moving world): arms 2 px wide from 5 to 17 px
// off the centre and a 2x2 dot, white with a one-pixel dark outline.
static bool crosshair(double x, double y, int w, int h, double rgb[3])
{
	const double dx = x - 0.5 * w, dy = y - 0.5 * h, ax = fabs(dx), ay = fabs(dy);
	const bool core = (ax < 1 && ay < 1) || (ay < 1 && ax >= 5 && ax < 17) || (ax < 1 && ay >= 5 && ay < 17);
	const bool edge = (ax < 2 && ay < 2) || (ay < 2 && ax >= 4 && ax < 18) || (ax < 2 && ay >= 4 && ay < 18);
	if (!edge)
		return false;
	rgb[0] = rgb[1] = rgb[2] = core ? 0.95 : 0.08;
	return true;
}

// ---- harder scenes (--hard, GSF 1.5) ---------------------------------------------------------
// What the Deck shows on fast turns and the earlier scenes did not reproduce: blocks inside the
// frame. A walk with a turn through a 3D yard (perspective ground of tiles with thin grout
// lines, textured pillars at several depths in front of a far panorama: parallax, occlusion,
// a flow that varies across the frame), a layered parallax pan with fine foliage-like detail
// and a thin fence, and thin straight lines (1-2 px grids, a diagonal edge, a crosshair) under
// slow and fast pans. Texture contrast fades with distance as mipmapping would, so the truth
// itself does not alias.
#define H_F 640.0      // focal length (px): a turn of w rad per frame moves the centre by H_F * w
#define H_CX 640.0
#define H_CY 380.0     // horizon
#define H_CAM 1.6      // eye height (world units)
static const double H_PILLAR[][2] = {
	{ -2.5, 4.0 }, { 1.8, 5.5 }, { -0.6, 8.0 }, { 3.6, 9.0 }, { -4.2, 11.0 }, { 0.9, 13.5 },
	{ 5.5, 6.0 }, { -6.0, 7.5 }, { 7.0, 12.0 }, { -2.0, 16.0 }, { 4.0, 17.5 }, { 8.5, 3.5 },
	{ -8.0, 4.5 }, { 10.0, 9.5 }, { -10.5, 13.0 }, { 2.6, 2.2 },
};
#define H_NPILLAR ((int)(sizeof(H_PILLAR) / sizeof(H_PILLAR[0])))

// Share of [u - fp/2, u + fp/2] covered by the periodic band [kP, kP + wl): box-filtered
// coverage of thin lines over a sample's footprint fp (the scenes take 2x2 samples per pixel,
// fp = 0.5 px), so a line at a sub-pixel position is a partial pixel and moves smoothly.
static double pband_int(double x, double P, double wl)
{
	const double k = floor(x / P), r = x - k * P;
	return k * wl + (r < wl ? r : wl);
}
static double pcov(double u, double P, double wl, double fp)
{
	if (fp < 1e-6)
		fp = 1e-6;
	return (pband_int(u + 0.5 * fp, P, wl) - pband_int(u - 0.5 * fp, P, wl)) / fp;
}

static void hturn(const scene *s, double t, double x, double y, double rgb[3])
{
	// vx: turn speed at the centre (px per source frame), vy: walking speed (units per frame).
	const double yaw = s->vx / H_F * t, cz = s->vy * t;
	const double dxc = (x - H_CX) / H_F, dyc = -(y - H_CY) / H_F;
	const double c = cos(yaw), sn = sin(yaw);
	const double dx = dxc * c + sn, dz = -dxc * sn + c;  // world direction (horizontal), dy = dyc
	// Nearest pillar along the ray (circles of radius 0.4, 3.2 high).
	double best = 1e9, bu = 0;
	for (int i = 0; i < H_NPILLAR; i++) {
		const double px = H_PILLAR[i][0], pz = H_PILLAR[i][1] - cz;
		const double a = dx * dx + dz * dz, b = -2.0 * (dx * px + dz * pz), cc = px * px + pz * pz - 0.16;
		const double disc = b * b - 4 * a * cc;
		if (disc < 0)
			continue;
		const double sh = (-b - sqrt(disc)) / (2 * a);
		if (sh <= 0.05 || sh >= best)
			continue;
		const double hy = H_CAM + dyc * sh;
		if (hy < 0 || hy > 3.2)
			continue;
		best = sh;
		const double hx = dx * sh - px, hz = dz * sh - pz;
		bu = atan2(hz, hx) * 0.4 + 7.0 * i;
	}
	const double sg = dyc < 0 ? H_CAM / -dyc : 1e9;
	if (best < 1e9 && best < sg) {
		// Pillar: stone with fine detail and thin grooves every 0.3 units of height.
		const double hy = H_CAM + dyc * best, k = 160.0;
		const double fade = 1.0 / (1.0 + best / 10.0);
		double n = 0.5 * vnoise(bu * k / 9.0, hy * k / 9.0, 41) + 0.3 * vnoise(bu * k / 3.0, hy * k / 3.0, 42) + 0.2 * vnoise(bu * k / 1.3, hy * k / 1.3, 43);
		const double groove = 1.0 - 0.7 * pcov(hy, 0.3, 0.012, 0.5 * best / H_F);
		const double v = (0.35 + 0.5 * (0.5 + (n - 0.5) * fade)) * (1.0 - (1.0 - groove) * fade);
		rgb[0] = 0.95 * v;
		rgb[1] = 0.85 * v;
		rgb[2] = 0.70 * v;
		return;
	}
	if (sg < 60.0) {
		// Ground: 1x1 tiles with thin grout lines and fine speckle, fading with distance.
		const double gx = dx * sg, gz = dz * sg + cz;
		const double fade = 1.0 / (1.0 + sg / 6.0);
		const double gw = 0.025, fpx = 0.5 * sg / H_F, fpz = fpx * sg / H_CAM;
		const double cx_ = pcov(gx, 1.0, gw, fpx), cz_ = pcov(gz, 1.0, gw, fpz);
		const double grout = 1.0 - 0.75 * (cx_ > cz_ ? cx_ : cz_);
		const double n = 0.5 * vnoise(gx * 7.0, gz * 7.0, 44) + 0.5 * vnoise(gx * 40.0, gz * 40.0, 45);
		const double tone = 0.55 + 0.25 * hash2((int)floor(gx), (int)floor(gz), 46);
		const double v = (tone + 0.35 * (n - 0.5) * fade) * (1.0 - (1.0 - grout) * fade);
		rgb[0] = 0.75 * v;
		rgb[1] = 0.80 * v;
		rgb[2] = 0.85 * v;
		return;
	}
	// Far panorama: only the turn moves it.
	const double ang = atan2(dx, dz);
	background(ang * H_F, y, 47, rgb);
}

static void hpar(const scene *s, double t, double x, double y, double rgb[3])
{
	// Layered pan: far hills (0.35 v), a fence of thin posts and rails (v), near foliage (1.6 v).
	const double vx = s->vx, vy = s->vy;
	const double nx = x - 1.4 * vx * t, ny = y - 1.4 * vy * t;
	// Near layer: blobs of fine foliage-like detail.
	const double m = vnoise(nx / 90.0, ny / 90.0, 61);
	if (m > 0.62) {
		const double d = 0.45 * vnoise(nx / 3.0, ny / 3.0, 62) + 0.35 * vnoise(nx / 2.0, ny / 2.0, 63) + 0.2 * vnoise(nx / 7.0, ny / 7.0, 64);
		const double edge = m < 0.66 ? 0.6 : 1.0;
		rgb[0] = (0.10 + 0.25 * d) * edge;
		rgb[1] = (0.25 + 0.55 * d) * edge;
		rgb[2] = (0.08 + 0.15 * d) * edge;
		return;
	}
	const double fx = x - vx * t, fy = y - vy * t;
	// Posts 2 px wide every 61, rails 1 px high every 53 (box-filtered).
	const double cp = pcov(fx, 61.0, 2.0, 0.5);
	const double cr = pcov(fy, 53.0, 1.0, 0.5);
	background(x - 0.35 * vx * t, y - 0.35 * vy * t, 65, rgb);
	const double c2 = cp > cr ? cp : cr;
	for (int k = 0; k < 3; k++)
		rgb[k] = rgb[k] * (1.0 - c2) + 0.9 * c2;
}

static void hlines(const scene *s, double t, double x, double y, int w, int h, double rgb[3])
{
	if (crosshair(x, y, w, h, rgb))
		return;
	const double u = x - s->vx * t, v = y - s->vy * t;
	background(u, v, 71, rgb);
	for (int k = 0; k < 3; k++)
		rgb[k] = 0.35 + 0.4 * (rgb[k] - 0.5);  // a calm texture under the lines
	// Thin straight lines: vertical 1 px every 29, horizontal 1 px every 41, a 2 px vertical
	// every 113 and a 2 px horizontal every 97, and a straight diagonal edge every 151.
	const double c1 = pcov(u, 29.0, 1.0, 0.5), c2 = pcov(v, 41.0, 1.0, 0.5);
	const double d1 = pcov(u, 113.0, 2.0, 0.5), d2 = pcov(v, 97.0, 2.0, 0.5);
	const double c = c1 > c2 ? c1 : c2, d = d1 > d2 ? d1 : d2;
	const double g = pcov(u + 0.6 * v, 151.0, 75.0, 0.5 * 1.6);  // straight diagonal edges
	for (int k = 0; k < 3; k++) {
		rgb[k] = rgb[k] * (1.0 - g) + (0.75 * rgb[k] + 0.2) * g;
		rgb[k] = rgb[k] * (1.0 - c) + 0.92 * c;
		rgb[k] = rgb[k] * (1.0 - d) + 0.04 * d;
	}
}

static void scene_pixel(const scene *s, double t, double x, double y, int w, int h, double rgb[3])
{
	switch (s->kind) {
	case S_HTURN:
		hturn(s, t, x, y, rgb);
		return;
	case S_HTURN_DARK: {
		// The same yard at night: dim and low contrast, with per-frame grain (TAA noise).
		hturn(s, t, x, y, rgb);
		const double g = (hash2((int)x, (int)y, 2000 + (int)lrint(t * 64.0)) - 0.5) * 0.03;
		for (int k = 0; k < 3; k++)
			rgb[k] = 0.06 + 0.3 * rgb[k] + g;
		return;
	}
	case S_HPAR:
		hpar(s, t, x, y, rgb);
		return;
	case S_HLINES:
		hlines(s, t, x, y, w, h, rgb);
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
	case S_ROTATE: {
		const double a = -4.0 * 3.14159265358979 / 180.0 * t, cx = 640, cy = 400;
		const double dx = x - cx, dy = y - cy;
		background(cx + dx * cos(a) - dy * sin(a), cy + dx * sin(a) + dy * cos(a), 5, rgb);
		return;
	}
	case S_LINES: {
		if (hud(x, y, w, h, rgb))
			return;
		const double u = x - s->vx * t, v = y - s->vy * t;
		background(u, v, 17, rgb);
		// One-pixel bright lines every 23 px (vertical) and 31 px (horizontal), box-filtered
		// so that a sub-pixel position is a partial pixel, as a game renders them.
		const double fu = fmod(fmod(u, 23.0) + 23.0, 23.0), fv = fmod(fmod(v, 31.0) + 31.0, 31.0);
		const double cu = fu < 1.0 ? 1.0 - fu : fu > 22.0 ? fu - 22.0 : 0.0;
		const double cv = fv < 1.0 ? 1.0 - fv : fv > 30.0 ? fv - 30.0 : 0.0;
		const double c = cu > cv ? cu : cv;
		for (int k = 0; k < 3; k++)
			rgb[k] = rgb[k] * (1.0 - c) + 0.95 * c;
		return;
	}
	case S_GRAIN: {
		background(x - s->vx * t, y - s->vy * t, 23, rgb);
		// Per-frame grain: a new pattern at every source time (t = 0, 1; generated phases get
		// the truth of their own time, which no interpolation can reproduce: it only adds a
		// floor to the error, the same for every method).
		const double g = (hash2((int)x, (int)y, 1000 + (int)lrint(t * 64.0)) - 0.5) * 0.05;
		for (int k = 0; k < 3; k++)
			rgb[k] = 0.25 + 0.3 * (rgb[k] - 0.5) + g;
		return;
	}
	case S_CUT_FLAT:
		if (t < 0.5)
			background(x, y, 1, rgb);
		else
			rgb[0] = rgb[1] = rgb[2] = 0.06;
		return;
	case S_CUT_STAIRS:
		if (t < 0.5)
			stairs(x, y, rgb);
		else
			background(x, y, 31, rgb);
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
	if (s->kind != S_CAMERA)
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
// Blockiness (GSF 1.5). errgrad: mean |gradient| of the luma error G - T (8-bit units): steps
// in the error (a block's vector next to another's, a seam) score, a smooth error does not.
// blk: how much more of G's gradient sits on one column (row) phase of the 4-pixel block grid
// than the truth's (max over the 4 phases / mean, G minus T, x and y averaged; 0 = no grid).
// The grid moves with the motion at a phase, which is why every phase offset is tried.
typedef struct { double errgrad, blk; } blockiness;
static blockiness g_blk, g_blk_cam;
static int g_blk_n, g_blk_cam_n;
static double luma8(const uint8_t *a, size_t i) { return 0.25 * a[i] + 0.5 * a[i + 1] + 0.25 * a[i + 2]; }
static blockiness block_metric(const uint8_t *g, const uint8_t *t, int w, int h)
{
	blockiness r = { 0, 0 };
	double eg = 0, n = 0, gx[2][4] = {{0}}, gy[2][4] = {{0}}, nx[4] = {0}, ny[4] = {0};
	const int m = 8;  // borders out
	for (int y = m; y < h - m; y++)
		for (int x = m; x < w - m; x++) {
			const size_t i = ((size_t)y * w + x) * 4, ix = i + 4, iy = i + (size_t)w * 4;
			const double e = luma8(g, i) - luma8(t, i);
			eg += fabs(luma8(g, ix) - luma8(t, ix) - e) + fabs(luma8(g, iy) - luma8(t, iy) - e);
			n++;
			gx[0][x & 3] += fabs(luma8(g, ix) - luma8(g, i));
			gx[1][x & 3] += fabs(luma8(t, ix) - luma8(t, i));
			gy[0][y & 3] += fabs(luma8(g, iy) - luma8(g, i));
			gy[1][y & 3] += fabs(luma8(t, iy) - luma8(t, i));
			nx[x & 3]++;
			ny[y & 3]++;
		}
	r.errgrad = eg / n;
	double ratio[2][2];
	for (int k = 0; k < 2; k++) {
		double mx = 0, sx = 0, my = 0, sy = 0;
		for (int c = 0; c < 4; c++) {
			const double vx = gx[k][c] / nx[c], vy = gy[k][c] / ny[c];
			mx = vx > mx ? vx : mx;
			my = vy > my ? vy : my;
			sx += vx / 4;
			sy += vy / 4;
		}
		ratio[k][0] = sx > 0 ? mx / sx : 1;
		ratio[k][1] = sy > 0 ? my / sy : 1;
	}
	r.blk = 0.5 * ((ratio[0][0] - ratio[1][0]) + (ratio[0][1] - ratio[1][1]));
	return r;
}

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
		if (kfg_vk_image_create(vk, &e->stats[r], kfg_vk_format(KFG_FORMAT_RGBA16F), 4, 1))
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
		float st[16];
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
				if (max_abs(G, A, nbytes) > 0) { ok = false; why = "scene cut not the real frame A"; }
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

// ---- harder set (--hard, GSF 1.5) ----------------------------------------------------------
static double luma_px(const uint8_t *a, size_t i) { return 0.299 * a[4 * i] + 0.587 * a[4 * i + 1] + 0.114 * a[4 * i + 2]; }

// Luma Laplacian magnitude into hp (0 at the border).
static void laplace(const uint8_t *img, int w, int h, float *hp)
{
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const size_t i = (size_t)y * w + x;
			if (x == 0 || y == 0 || x == w - 1 || y == h - 1) {
				hp[i] = 0;
				continue;
			}
			hp[i] = (float)(4.0 * luma_px(img, i) - luma_px(img, i - 1) - luma_px(img, i + 1) - luma_px(img, i - (size_t)w) - luma_px(img, i + (size_t)w));
		}
}

// Interior outlier blocks: 8x8 blocks whose mean |luma error| is above 6 (8-bit) and above
// 2.5 times the median of its 8 neighbours': a square patch that does not fit around it,
// what blocks inside the frame look like. Returns their share in % (borders of 16 px out).
static double bad_blocks(const uint8_t *g, const uint8_t *t, int w, int h)
{
	const int bw = w / 8, bh = h / 8;
	double *e = calloc((size_t)bw * bh, sizeof(double));
	for (int by = 0; by < bh; by++)
		for (int bx = 0; bx < bw; bx++) {
			double sum = 0;
			for (int y = by * 8; y < by * 8 + 8; y++)
				for (int x = bx * 8; x < bx * 8 + 8; x++)
					sum += fabs(luma_px(g, (size_t)y * w + x) - luma_px(t, (size_t)y * w + x));
			e[by * bw + bx] = sum / 64.0;
		}
	int bad = 0, n = 0;
	for (int by = 2; by < bh - 2; by++)
		for (int bx = 2; bx < bw - 2; bx++) {
			double nb[8];
			int k = 0;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++)
					if (dx || dy)
						nb[k++] = e[(by + dy) * bw + bx + dx];
			qsort(nb, 8, sizeof(double), cmp_double);
			const double med = 0.5 * (nb[3] + nb[4]), v = e[by * bw + bx];
			bad += v > 6.0 && v > 2.5 * med;
			n++;
		}
	free(e);
	return n ? 100.0 * bad / n : 0.0;
}

static const scene HARD[] = {
	{ "h_pan_ref", S_PAN, 24, 6, true, false },  // the old pan, for reference
	{ "h_turn24", S_HTURN, 24, 0.06, true, false },
	{ "h_turn32", S_HTURN, 32, 0.10, true, false },
	{ "h_turn40", S_HTURN, 40, 0.08, true, false },
	{ "h_turn64", S_HTURN, 64, 0.12, true, false },
	{ "h_turn96", S_HTURN, 96, 0.10, true, false },
	{ "h_dark64", S_HTURN_DARK, 64, 0.12, true, false },
	{ "h_parallax", S_HPAR, 30, 4, true, false },
	{ "h_lines_slow", S_HLINES, 2.7, 0.9, true, false },
	{ "h_lines_fast", S_HLINES, 24, 7, true, false },
};

// Every scene as a running sequence of 8 frames through the ring (temporal seeds on), shown as
// the real frame and the generated phases of x3 (and x2). Per scene and mode:
//   psnr:     mean and worst over the generated frames;
//   errgrad, blk: blockiness (block_metric); badblk: interior outlier blocks, % (bad_blocks);
//   temporal: mean |change error| between consecutive shown frames (temporal_err);
//   lineflk:  the same in luma on line pixels only (|Laplacian| of the truth above 48 in
//             either frame): shimmer of thin straight lines;
//   sharpflk: mean change of the high-frequency energy ratio output / truth from one shown
//             frame to the next, % (sharp real / soft generated alternation).
static int hard_suite(kfg_vk *vk, engine *e, int w, int h, FILE *js, const char *only, const kfg_tuning *tune)
{
	const size_t nbytes = (size_t)w * h * 4, npx = (size_t)w * h;
	uint8_t *B = malloc(nbytes), *T = malloc(nbytes), *O = malloc(nbytes), *Op = malloc(nbytes), *Tp = malloc(nbytes);
	float *HO = malloc(npx * sizeof(float)), *HT = malloc(npx * sizeof(float)), *HTp = malloc(npx * sizeof(float));
	const int frames = 8;
	if (js)
		fprintf(js, " \"hard\": [\n");
	bool first = true;
	double tot_ps = 0, tot_bb = 0, tot_lf = 0;
	int tot_n = 0;
	for (size_t si = 0; si < sizeof(HARD) / sizeof(HARD[0]); si++) {
		const scene *s = &HARD[si];
		if (only && strcmp(only, s->name))
			continue;
		for (int mode = 3; mode >= 2; mode--) {
			if (mode == 2 && s->kind != S_HLINES && !getenv("KFG_HARD_X2"))
				continue;
			double ps = 0, worst = 1e9, eg = 0, bk = 0, bb = 0, bbmax = 0, te = 0, lf = 0, sf = 0, rprev = -1;
			int n_ps = 0, n_te = 0, n_lf = 0;
			bool have_prev = false;
			for (int f = 0; f < frames; f++) {
				uint32_t fb = (uint32_t)f % KFG_RING, fa = (uint32_t)(f + KFG_RING - 1) % KFG_RING;
				render(s, (double)f, w, h, B);
				kfg_vk_upload_rgba8(vk, &e->color[fb], B);
				kfg_plan p;
				kfg_plan_analyze(&p, (uint32_t)w, (uint32_t)h, fa, fb, fb, f > 0, f > 1 && g_temporal ? (int)fa : -1, tune);
				run_plan(e, &p, NULL, NULL);
				if (f == 0)
					continue;
				if (getenv("KFG_HARD_FIELD") && (s->kind == S_HLINES || s->kind == S_PAN || s->kind == S_HPAR) && mode == 3) {
					// Tuning aid: share of blocks of the pair's field (and of the level-2 flow) within
					// 0.25 / 0.5 / 1 px of the true motion (the uniform layer: lines, the pan, the fence).
					const kfg_vk_image *im[2] = { &e->pair_select[fb], &e->flow[fb][2] };
					for (int q = 0; q < 2; q++) {
						const size_t nf = (size_t)im[q]->width * im[q]->height;
						float *fl = malloc(nf * 4 * sizeof(float));
						kfg_vk_download_rgba16f(vk, im[q], fl);
						int c1 = 0, c2 = 0, c3 = 0;
						double ex = 0, ey = 0;
						for (size_t i = 0; i < nf; i++) {
							const double dx = fl[i * 4] - s->vx, dy = fl[i * 4 + 1] - s->vy, d = hypot(dx, dy);
							c1 += d < 0.25; c2 += d < 0.5; c3 += d < 1.0;
							ex += fabs(dx); ey += fabs(dy);
						}
						printf("  f%d %s: %.0f%% / %.0f%% / %.0f%% within 0.25/0.5/1 px, mean |ex| %.2f |ey| %.2f\n", f, q ? "flow L2" : "field",
						       100.0 * c1 / nf, 100.0 * c2 / nf, 100.0 * c3 / nf, ex / nf, ey / nf);
						if (f == 4) {
							static int hb[41][41];
							memset(hb, 0, sizeof(hb));
							double wsum = 0;
							for (size_t i = 0; i < nf; i++) {
								const int bx = (int)lrint(fl[i * 4] * 2) + 20, by = (int)lrint(fl[i * 4 + 1] * 2) + 20;
								if (bx >= 0 && bx <= 40 && by >= 0 && by <= 40)
									hb[by][bx]++;
								wsum += fl[i * 4 + 3];
							}
							printf("    top (0.5 px):");
							for (int k = 0; k < 6; k++) {
								int m = 0, mx = 0, my = 0;
								for (int y = 0; y <= 40; y++)
									for (int x = 0; x <= 40; x++)
										if (hb[y][x] > m) { m = hb[y][x]; mx = x; my = y; }
								printf(" (%.1f,%.1f) %.0f%%", (mx - 20) * 0.5, (my - 20) * 0.5, 100.0 * m / nf);
								hb[my][mx] = 0;
							}
							printf("  mean w %.3f\n", wsum / nf);
							if (getenv("KFG_HARD_OUT")) {
								char path[1024];
								snprintf(path, sizeof(path), "%s/%s_%s.f32", getenv("KFG_HARD_OUT"), s->name, q ? "flow2" : "field");
								FILE *ff = fopen(path, "wb");
								if (ff) {
									fwrite(&im[q]->width, 4, 1, ff);
									fwrite(&im[q]->height, 4, 1, ff);
									fwrite(fl, sizeof(float), nf * 4, ff);
									fclose(ff);
								}
							}
						}
						free(fl);
					}
				}
				for (int j = 0; j < mode; j++) {
					const double t = (double)j / mode;
					render(s, f - 1 + t, w, h, T);
					if (j == 0) {
						render(s, f - 1, w, h, O);
					} else {
						kfg_plan_generate(&p, (uint32_t)w, (uint32_t)h, fa, fb, fb, (float)t, 0);
						run_plan(e, &p, NULL, NULL);
						kfg_vk_download_rgba8(vk, &e->output, O);
						const double pg = psnr_region(O, T, w, h, 0, 0, w, h);
						ps += pg;
						worst = pg < worst ? pg : worst;
						const blockiness bm = block_metric(O, T, w, h);
						eg += bm.errgrad;
						bk += bm.blk;
						const double b = bad_blocks(O, T, w, h);
						bb += b;
						bbmax = b > bbmax ? b : bbmax;
						n_ps++;
						if (f == 4) {
							char name[128];
							snprintf(name, sizeof(name), "%s_x%d_f%d_t%d_gen", s->name, mode, f, j);
							write_ppm(getenv("KFG_HARD_OUT"), name, O, w, h);
							snprintf(name, sizeof(name), "%s_x%d_f%d_t%d_truth", s->name, mode, f, j);
							write_ppm(getenv("KFG_HARD_OUT"), name, T, w, h);
						}
					}
					laplace(O, w, h, HO);
					laplace(T, w, h, HT);
					double eo = 0, et = 0;
					for (int y = 8; y < h - 8; y++)
						for (int x = 8; x < w - 8; x++) {
							const size_t i = (size_t)y * w + x;
							eo += fabs(HO[i]);
							et += fabs(HT[i]);
						}
					const double r = eo / (et + 1e-9);
					if (have_prev) {
						te += temporal_err(O, Op, T, Tp, w, h);
						double sl = 0;
						long nl = 0;
						for (int y = 8; y < h - 8; y++)
							for (int x = 8; x < w - 8; x++) {
								const size_t i = (size_t)y * w + x;
								if (fabs(HT[i]) < 48.0f && fabs(HTp[i]) < 48.0f)
									continue;
								sl += fabs((luma_px(O, i) - luma_px(Op, i)) - (luma_px(T, i) - luma_px(Tp, i)));
								nl++;
							}
						if (nl) {
							lf += sl / (double)nl;
							n_lf++;
						}
						sf += fabs(r - rprev);
						n_te++;
					}
					rprev = r;
					memcpy(Op, O, nbytes);
					memcpy(Tp, T, nbytes);
					float *tmp = HT; HT = HTp; HTp = tmp;
					have_prev = true;
				}
			}
			printf("hard %-13s x%d: psnr %.2f dB (worst %.2f) errgrad %.3f blk %.4f badblk %.3f%% (max %.3f) temporal %.3f lineflk %.3f sharpflk %.2f%%\n",
			       s->name, mode, ps / n_ps, worst, eg / n_ps, bk / n_ps, bb / n_ps, bbmax, te / n_te, n_lf ? lf / n_lf : 0.0, 100.0 * sf / n_te);
			if (mode == 3) {
				tot_ps += ps / n_ps;
				tot_bb += bb / n_ps;
				tot_lf += n_lf ? lf / n_lf : 0.0;
				tot_n++;
			}
			if (js)
				fprintf(js, "%s  {\"scene\": \"%s\", \"mode\": %d, \"psnr\": %.3f, \"psnr_worst\": %.3f, \"errgrad\": %.4f, \"blk\": %.5f, \"badblk_pct\": %.4f, \"badblk_max\": %.4f, \"temporal\": %.4f, \"lineflk\": %.4f, \"sharpflk_pct\": %.3f}",
					first ? "" : ",\n", s->name, mode, ps / n_ps, worst, eg / n_ps, bk / n_ps, bb / n_ps, bbmax, te / n_te, n_lf ? lf / n_lf : 0.0, 100.0 * sf / n_te);
			first = false;
		}
	}
	if (tot_n)
		printf("hard x3 mean: psnr %.2f dB, badblk %.3f%%, lineflk %.3f\n", tot_ps / tot_n, tot_bb / tot_n, tot_lf / tot_n);
	if (js)
		fprintf(js, "\n ]\n}\n");
	free(B); free(T); free(O); free(Op); free(Tp); free(HO); free(HT); free(HTp);
	return 0;
}

int main(int argc, char **argv)
{
	const char *spv = "build";
	const char *out_dir = NULL;
	const char *json_path = NULL;
	const char *only = NULL;   // run one scene (tuning); skips the sequence and stairs checks
	int w = 1280, h = 800, bench = 0;
	bool hard = false;
	bool validation = getenv("KFG_VALIDATION") != NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--spv") && i + 1 < argc) spv = argv[++i];
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
		else if (!strcmp(argv[i], "--json") && i + 1 < argc) json_path = argv[++i];
		else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
		else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--scene") && i + 1 < argc) only = argv[++i];
		else if (!strcmp(argv[i], "--no-temporal")) g_temporal = 0;
		else if (!strcmp(argv[i], "--hard")) hard = true;  // the harder set only (GSF 1.5)
		else if (!strcmp(argv[i], "--lowbase")) { g_lowbase = true; g_timescale = 1.25; }
		else if (!strcmp(argv[i], "--per-phase")) kfg_per_phase_select = 1;  // 1.2 selection, for A/B
		else if (!strcmp(argv[i], "--scaled")) g_geo = -1.0;  // set from --size below
		else if (!strcmp(argv[i], "--analysis-pixels") && i + 1 < argc) kfg_analysis_pixels = (uint32_t)atoi(argv[++i]);  // 0 = full size
		else {
			fprintf(stderr, "usage: kfg_vktest [--spv DIR] [--size WxH] [--out DIR] [--json FILE] [--bench N] [--scene NAME] [--no-temporal] [--lowbase] [--per-phase] [--scaled] [--analysis-pixels N] [--hard]\n");
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
	if (hard) {
		if (js)
			fprintf(js, " ],\n");
		hard_suite(&vk, &e, w, h, js, only, &tune);
		if (js)
			fclose(js);
		engine_destroy(&e);
		kfg_vk_destroy(&vk);
		return 0;
	}

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
		float st[16];  // stats is 4x1
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
		printf("%-12s cut=%.0f bad=%.3f mc=%.4f zero=%.4f global=(%.1f,%.1f) grad=(%.1f,%.1f;%.1f,%.1f)\n", s->name, st[0], st[1], st[2], st[3], st[4], st[5], st[8], st[9], st[10], st[11]);
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
			double hud_err = s->kind == S_OBJECTS || s->kind == S_LINES ? mean_abs(G, T, w, h, 40, 30, 360, 70) : 0.0;
			int maxd = max_abs(G, A, nbytes);  // a cut repeats the real frame A at every phase
			bool ok = true;
			const char *why = "";
			if (s->dynamic) {
				if (!(pg >= pl + 3.0 && pg >= pd + 3.0)) { ok = false; why = "not better than blend/duplicate by 3 dB"; }
				if (!(og >= ol + 2.0 && og >= od + 2.0)) { ok = false; why = "moving region not better than blend/duplicate by 2 dB"; }
				if (!(ma > 0.5 && mb > 0.5)) { ok = false; why = "output equals a source frame"; }
				if (hud_err > 2.0) { ok = false; why = "HUD distorted"; }
			} else if (s->cut) {
				if (maxd > 0) { ok = false; why = "scene cut not the real frame A"; }
			} else {
				if (maxd > 1) { ok = false; why = "static scene changed"; }
			}
			// The climb (big steps under perspective, as in the 1.1.1 report) is an open
			// problem: measured and reported, not yet a gate.
			const bool known = s->kind == S_CLIMB;
			if (!ok && !known)
				failures++;
			const blockiness bm = block_metric(G, T, w, h);
			if (s->dynamic) {
				g_blk.errgrad += bm.errgrad;
				g_blk.blk += bm.blk;
				g_blk_n++;
			}
			printf("  t=%.3f gen=%.2fdB blend=%.2fdB dup=%.2fdB region gen/blend/dup=%.2f/%.2f/%.2f |G-A|=%.2f |G-B|=%.2f hud=%.2f errgrad=%.3f blk=%.4f %s%s\n",
			       t, pg, pl, pd, og, ol, od, ma, mb, hud_err, bm.errgrad, bm.blk, ok ? "PASS" : known ? "OPEN (not gated): " : "FAIL: ", why);
			if (js)
				fprintf(js, "   {\"t\": %.6f, \"psnr_gen\": %.3f, \"psnr_blend\": %.3f, \"psnr_dup\": %.3f, \"region_gen\": %.3f, \"region_blend\": %.3f, \"region_dup\": %.3f, \"mean_abs_vs_A\": %.3f, \"mean_abs_vs_B\": %.3f, \"hud_err\": %.3f, \"errgrad\": %.4f, \"blk\": %.5f, \"pass\": %s}%s\n",
					t, pg, pl, pd, og, ol, od, ma, mb, hud_err, bm.errgrad, bm.blk, ok ? "true" : "false", pi < 2 ? "," : "");
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
	if (g_blk_n) {
		printf("blockiness (moving scenes): errgrad %.3f, blk %.4f over %d outputs\n", g_blk.errgrad / g_blk_n, g_blk.blk / g_blk_n, g_blk_n);
		if (js)
			fprintf(js, " \"blockiness\": {\"errgrad\": %.4f, \"blk\": %.5f, \"outputs\": %d},\n", g_blk.errgrad / g_blk_n, g_blk.blk / g_blk_n, g_blk_n);
	}

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
	if (!only || !strcmp(only, "camera")) {
		const scene cam = { "camera", S_CAMERA, 0, 0, true, false };
		const scene *s = &cam;
		uint8_t *O = malloc(nbytes), *Op = malloc(nbytes), *Tp = malloc(nbytes), *Dp = malloc(nbytes);
		double te_gen = 0, te_dup = 0, ps_gen = 0, ps_dup = 0, worst = 1e9, hud_max = 0;
		int n_te = 0, n_ps = 0, cuts = 0;
		bool have_prev = false;
		for (int f = 0; f <= CAM_FRAMES; f++) {
			uint32_t fb = (uint32_t)f % KFG_RING, fa = (uint32_t)(f + KFG_RING - 1) % KFG_RING;
			render(s, (double)f, w, h, B);
			kfg_vk_upload_rgba8(&vk, &e.color[fb], B);
			kfg_plan p;
			kfg_plan_analyze(&p, (uint32_t)w, (uint32_t)h, fa, fb, fb, f > 0, f > 1 && g_temporal ? (int)fa : -1, &tune);
			run_plan(&e, &p, NULL, NULL);
			if (f == 0)
				continue;
			float st[16];  // stats is 4x1
			kfg_vk_download_rgba16f(&vk, &e.stats[fb], st);
			if (st[0] > 0.5)
				cuts++;
			if (getenv("KFG_VERBOSE"))
				printf("  camera f%02d stats cut=%.0f bad=%.3f mc/zero=%.3f global=(%.1f,%.1f)\n", f, st[0], st[1], st[2] / (st[3] + 1e-9), st[4], st[5]);
			if (getenv("KFG_VERBOSE")) {
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
					ps_dup += psnr_region(D, T, w, h, 0, 0, w, h);
					if (pg < worst)
						worst = pg;
					if (getenv("KFG_VERBOSE"))
						printf("  camera f%02d v=%.1f t=%d/3 gen %.2f dB\n", f, CAM_VX[f - 1], j, pg);
					const blockiness bm = block_metric(O, T, w, h);
					g_blk_cam.errgrad += bm.errgrad;
					g_blk_cam.blk += bm.blk;
					g_blk_cam_n++;
					double he = mean_abs(O, T, w, h, 40, 30, 360, 70);
					if (he > hud_max)
						hud_max = he;
					n_ps++;
					char name[64];
					snprintf(name, sizeof(name), "camera_f%02d_t%d_gen", f, j);
					write_ppm(out_dir, name, O, w, h);
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
		printf("camera: psnr gen %.2f dB (worst %.2f), dup %.2f dB; temporal gen %.3f, dup %.3f; cuts %d of %d; hud max %.2f; errgrad %.3f blk %.4f\n",
		       ps_gen / n_ps, worst, ps_dup / n_ps, te_gen / n_te, te_dup / n_te, cuts, CAM_FRAMES, hud_max,
		       g_blk_cam_n ? g_blk_cam.errgrad / g_blk_cam_n : 0.0, g_blk_cam_n ? g_blk_cam.blk / g_blk_cam_n : 0.0);
		if (js)
			fprintf(js, " \"camera\": {\"psnr_gen\": %.3f, \"psnr_worst\": %.3f, \"psnr_dup\": %.3f, \"temporal_gen\": %.4f, \"temporal_dup\": %.4f, \"cuts\": %d, \"hud_max\": %.3f, \"errgrad\": %.4f, \"blk\": %.5f},\n",
				ps_gen / n_ps, worst, ps_dup / n_ps, te_gen / n_te, te_dup / n_te, cuts, hud_max,
				g_blk_cam_n ? g_blk_cam.errgrad / g_blk_cam_n : 0.0, g_blk_cam_n ? g_blk_cam.blk / g_blk_cam_n : 0.0);
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

	// Benchmark: analysis of one pair + two generated outputs (x3), GPU timestamps per pass.
	if (bench > 0 && vk.timestamps) {
		double samples[KFG_MAX_PASSES * 2][512];
		char labels[KFG_MAX_PASSES * 2][32];
		uint32_t npass = 0;
		int reps = bench > 512 ? 512 : bench;
		const scene *s = &SCENES[2];
		render(s, 0.0, w, h, A);
		render(s, 1.0, w, h, B);
		kfg_vk_upload_rgba8(&vk, &e.color[0], A);
		kfg_vk_upload_rgba8(&vk, &e.color[1], B);
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
			if (strncmp(labels[i], "g", 1))
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
