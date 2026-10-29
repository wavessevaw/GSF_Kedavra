// SPDX-License-Identifier: GPL-3.0-or-later
// Closed-loop tests of kfg_auto.h with kfg_sched.h and a model of a game behind
// Gamescope's frame limiter (frame callback released on the cap's vblank grid).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "kfg_auto.h"
#include "kfg_sched.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static double urand(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (double)(rs >> 11) / 9007199254740992.0; }

typedef struct {
	double x2_fraction;    // share of vblanks at the top cap (refresh / 2, x2) in the second half
	double cap_avg;        // mean cap in the second half, fps
	uint64_t switches;
	double late_fraction;  // source-late repeats per source frame, second half
	double x2_all;         // share of vblanks at x2 over the whole run
	double uneven;         // second half: outputs whose motion step is off by > 25%
} result;

// In-game frame limiter (the recommended setup): 0 = none. The game works for a part of
// the frame, then its limiter sleeps until the next deadline (period 1 / lock_fps on the
// game's own clock, which runs slightly off the display clock, so its phase against the
// vblanks drifts through every position) and presents. A deadline it misses resets it.
static double g_lock_fps;
static double g_lock_skew = 2e-4;   // game clock vs display clock
static double g_lock_sleep = 0.4e-3; // limiter wake-up jitter, +-s
static uint32_t g_floor;             // kfg_auto_floor (0 = x3)

// native_fps(t_s) gives the game's own frame rate at time t; jitter is relative.
static result run(double (*native_fps)(double), double seconds, double jitter)
{
	const uint32_t hz = 90;
	const double period = 1.0 / hz;
	kfg_auto a;
	kfg_auto_init_floor(&a, hz, g_floor);
	kfg_sched_config cfg = kfg_sched_default_config(a.mult);
	kfg_sched s;
	kfg_sched_init(&s, &cfg);
	uint32_t mult = a.mult, cap = a.cap;
	double cap_sum = 0;
	const int64_t n = (int64_t)(seconds * hz);
	double busy_until = 0;      // game render end time
	double done_prev = -1;      // when the previous frame was done
	bool rendering = false, callback = true;
	uint64_t id = 0;
	double deadline = 0;        // in-game limiter
	double *gt = calloc((size_t)n + 2, sizeof(double)); // game time of each source frame
	double shown_prev = -1, shown = -1;
	int64_t steps = 0, uneven = 0;
	int64_t x2 = 0, half = 0, x2_all = 0;
	uint64_t late_base = 0, src_half = 0, ulate_base = 0;
	for (int64_t k = 0; k < n; k++) {
		const double t = k * period;
		if (k == n / 2) {
			late_base = s.n_repeat_reason[KFG_REPEAT_SOURCE_LATE];
			ulate_base = a.lates;
			src_half = id;
		}
		// Latch a frame that finished before this vblank.
		if (rendering && busy_until <= t) {
			rendering = false;
			done_prev = busy_until;
			kfg_sched_on_latch(&s, ++id, k);
			gt[id] = busy_until;
			kfg_auto_on_latch(&a, k);
			kfg_sched_on_me(&s, id, k + 2, true, false);
		}
		// Limiter: one frame callback each time the cap's grid passes a vblank.
		if (k == 0 || (k * cap) / hz != ((k - 1) * cap) / hz)
			callback = true;
		if (!rendering && callback) {
			callback = false;
			rendering = true;
			const double f = native_fps(t);
			// The game starts the frame at its callback or, if that came while it was still
			// busy, as soon as the previous one was done (not at the vblank that latched it).
			const double start = t - period < done_prev && done_prev < t ? done_prev : t;
			busy_until = start + (1.0 / f) * (1.0 + jitter * (2.0 * urand() - 1.0));
			if (g_lock_fps > 0) {
				const double p = (1.0 + g_lock_skew) / g_lock_fps;
				const double work = t + 0.6 * p * (1.0 + jitter * (2.0 * urand() - 1.0));
				deadline = deadline + p < work ? work : deadline + p;
				busy_until = deadline + g_lock_sleep * (2.0 * urand() - 1.0);
			}
		}
		const kfg_decision d = kfg_sched_decide(&s, k);
		if (d.show == KFG_SHOW_REAL)
			shown = gt[d.src_a];
		else if (d.show == KFG_SHOW_GEN)
			shown = gt[d.src_a] + (gt[d.src_b] - gt[d.src_a]) * d.phase_num / d.phase_den;
		if (k >= n / 2 && shown_prev >= 0 && d.state == KFG_STATE_ACTIVE) {
			steps++;
			if (fabs(shown - shown_prev - period) > 0.25 * period)
				uneven++;
		}
		shown_prev = shown;
		const uint32_t m = kfg_auto_step(&a, k);
		if (m != mult) {
			mult = m;
			kfg_sched_set_multiplier(&s, m);
		}
		cap = a.cap;
		const bool top = a.level + 1 == a.nlevels;
		x2_all += top;
		if (k >= n / 2) {
			half++;
			x2 += top;
			cap_sum += cap;
		}
	}
	result r;
	r.x2_fraction = (double)x2 / (double)half;
	r.cap_avg = cap_sum / (double)half;
	r.x2_all = (double)x2_all / (double)n;
	r.uneven = steps ? (double)uneven / (double)steps : 1.0;
	free(gt);
	r.switches = a.n_switch;
	const uint64_t src = id - src_half;
	(void)late_base;
	r.late_fraction = src ? (double)(a.lates - ulate_base) / (double)src : 1.0;
	return r;
}

static double fps60(double t) { (void)t; return 60.0; }
static double fps50(double t) { (void)t; return 50.0; }
static double fps44(double t) { (void)t; return 44.0; }
static double fps38(double t) { (void)t; return 38.0; }
static double fps33(double t) { (void)t; return 33.0; }
static double fps22(double t) { (void)t; return 22.0; }
static double fps27(double t) { (void)t; return 27.0; }
static double heavy_scene(double t) { return t < 120 ? 60.0 : (t < 240 ? 36.0 : 60.0); }

int main(void)
{
	result r;
	kfg_auto lv;
	kfg_auto_init(&lv, 90);
	CHECK(lv.nlevels == 4 && lv.levels[0] == 30 && lv.levels[1] == 35 && lv.levels[2] == 40 && lv.levels[3] == 45, "caps at 90 Hz: 30 35 40 45");
	kfg_auto_init(&lv, 60);
	CHECK(lv.nlevels == 1 && lv.levels[0] == 30 && lv.mult == 2, "caps at 60 Hz (a TV): 30 only, x2");
	kfg_auto_init(&lv, 120);
	CHECK(lv.nlevels == 5 && lv.levels[0] == 40 && lv.levels[4] == 60, "caps at 120 Hz: 40 45 50 55 60");
	// GSF 1.5: never more than two generated frames per real one, so never below refresh / 3.
	kfg_auto_init_floor(&lv, 120, 24);
	CHECK(lv.levels[0] == 40 && lv.mult == 3, "floor at 120 Hz: 40 (x3), not 30 (x4)");
	kfg_auto_init_floor(&lv, 90, 24);
	CHECK(lv.nlevels == 4 && lv.levels[0] == 30 && lv.mult == 3, "floor 24 at 90 Hz: 30 35 40 45, x3 at most");
	kfg_auto_init_floor(&lv, 90, 10);
	CHECK(lv.levels[0] == 30, "the floor never goes below refresh / 3 (%u)", lv.levels[0]);
	kfg_auto_init_floor(&lv, 90, 40);
	CHECK(lv.nlevels == 4 && lv.levels[0] == 30, "a floor above refresh / 3 changes nothing");
	kfg_auto_init_floor(&lv, 60, 16);
	CHECK(lv.nlevels == 2 && lv.levels[0] == 20 && lv.levels[1] == 30 && lv.mult == 3, "at 60 Hz the floor is 20 (x3), never lower");

	r = run(fps60, 300, 0.10);
	printf("native 60:  cap 45 %.1f%%, mean cap %.1f, switches %llu, late %.3f%%\n", 100 * r.x2_fraction, r.cap_avg, (unsigned long long)r.switches, 100 * r.late_fraction);
	CHECK(r.x2_fraction > 0.99, "a 60 fps game should settle at 45 (x2)");
	CHECK(r.switches <= 4, "no oscillation at 60 fps");

	r = run(fps50, 300, 0.10);
	printf("native 50:  cap 45 %.1f%%, mean cap %.1f, switches %llu, late %.3f%%\n", 100 * r.x2_fraction, r.cap_avg, (unsigned long long)r.switches, 100 * r.late_fraction);
	CHECK(r.x2_fraction > 0.95, "a 50 fps game should hold 45 (x2)");

	// A game between two caps settles below its rate and keeps it clean; trials of the next
	// cap back off. 44 fps is too close to 40 to hold it without long intervals (its frames
	// take 2.05 vblanks, so jitter pushes some past a vblank): 35 or 40 are both fine.
	const struct { double (*f)(double); double lo, hi; const char *name; } mid[] = {
		{ fps44, 34.0, 41.0, "native 44" }, { fps38, 34.0, 36.0, "native 38" }, { fps33, 29.5, 31.0, "native 33" } };
	for (int i = 0; i < 3; i++) {
		r = run(mid[i].f, 600, 0.05);
		printf("%s:  mean cap %.1f, switches %llu, late %.3f%%\n", mid[i].name, r.cap_avg, (unsigned long long)r.switches, 100 * r.late_fraction);
		CHECK(r.cap_avg > mid[i].lo && r.cap_avg < mid[i].hi, "%s should hold the cap below it (mean %.1f)", mid[i].name, r.cap_avg);
		CHECK(r.switches <= 2 * 600 / 32 + 8, "trials must back off (%llu switches)", (unsigned long long)r.switches);
		CHECK(r.late_fraction < 0.02, "%s is late %.3f%% at its cap", mid[i].name, 100 * r.late_fraction);
	}

	// Below refresh / 3 the cap stays at 30: never more than x3.
	r = run(fps22, 300, 0.05);
	printf("native 22:  mean cap %.1f, switches %llu\n", r.cap_avg, (unsigned long long)r.switches);
	CHECK(r.cap_avg < 30.5, "a 22 fps game stays at the lowest cap (x3)");

	// Floor 24 asked for (the old x3.75 Battery floor): since GSF 1.5 Auto stays at x3, a 27 fps
	// game keeps cap 30 and the scheduler holds its long frames.
	g_floor = 24;
	r = run(fps27, 600, 0.05);
	printf("floor 24, native 27:  mean cap %.1f, switches %llu, late %.3f%%, uneven motion %.2f%%\n", r.cap_avg, (unsigned long long)r.switches, 100 * r.late_fraction, 100 * r.uneven);
	CHECK(r.cap_avg > 29.5 && r.cap_avg < 30.5, "a 27 fps game stays at cap 30 (x3) whatever the floor (mean %.1f)", r.cap_avg);
	r = run(fps60, 300, 0.10);
	printf("floor 24, native 60:  cap 45 %.1f%%\n", 100 * r.x2_fraction);
	CHECK(r.x2_fraction > 0.97, "the floor does not hold back a fast game (45 for %.1f%%)", 100 * r.x2_fraction);
	g_floor = 0;

	// In-game FPS lock, its phase drifting through the vblanks.
	g_lock_fps = 30;
	r = run(fps60, 600, 0.10);
	printf("lock 30:    above 30 %.2f%% of the run, switches %llu, late %.3f%%, uneven motion %.2f%%\n", 100 * r.x2_all, (unsigned long long)r.switches, 100 * r.late_fraction, 100 * r.uneven);
	CHECK(r.cap_avg < 30.5, "a game locked at 30 should stay at 30 (mean %.1f)", r.cap_avg);
	CHECK(r.late_fraction < 0.005, "a game locked at 30 is not late at x3 (%.3f%%)", 100 * r.late_fraction);
	CHECK(r.uneven < 0.01, "a game locked at 30 must move evenly at x3 (%.2f%% uneven)", 100 * r.uneven);

	g_lock_fps = 45;
	r = run(fps60, 600, 0.10);
	printf("lock 45:    cap 45 %.1f%%, switches %llu, late %.3f%%, uneven motion %.2f%%\n", 100 * r.x2_fraction, (unsigned long long)r.switches, 100 * r.late_fraction, 100 * r.uneven);
	CHECK(r.x2_fraction > 0.99, "a game locked at 45 should hold 45");
	CHECK(r.late_fraction < 0.005, "a game locked at 45 is not late at x2 (%.3f%%)", 100 * r.late_fraction);
	CHECK(r.uneven < 0.005, "a game locked at 45 must move evenly at x2 (%.2f%% uneven)", 100 * r.uneven);
	g_lock_fps = 0;

	r = run(heavy_scene, 360, 0.10);
	printf("60->36->60: cap 45 %.1f%% (second half), switches %llu\n", 100 * r.x2_fraction, (unsigned long long)r.switches);
	CHECK(r.x2_fraction > 0.5, "45 should come back after the heavy scene");

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("kfg_auto: all tests passed\n");
	return 0;
}
