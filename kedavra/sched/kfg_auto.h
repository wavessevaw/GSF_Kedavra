// SPDX-License-Identifier: GPL-3.0-or-later
//
// KEDAVRA automatic source cap.
//
// Generation needs a source that arrives on a fixed vblank cadence. Instead of a manual
// FPS limit, the engine caps the game itself (Gamescope's frame limiter, with the
// fractional hook for caps that do not divide the refresh) and picks the cap from what the
// game actually delivers. The caps are steps of 5 fps from refresh / 3 (at least 30) up to
// refresh / 2 (90 Hz: 30, 35, 40, 45, so x3, x2.6, x2.25, x2; 120 Hz: 40 to 60; 60 Hz, a TV:
// 30, x2). By default generation never goes above x3: a game that cannot hold the lowest cap
// stays capped there and the scheduler repeats frames. A controller may lower the floor
// (kfg_auto_floor) to 24 fps or refresh / 4, whichever is higher (90 Hz: 24, x3.75, frames
// held 3 or 4 refreshes, never 5; 60 Hz: 24, x2.5; 120 Hz: 30, x4); the floor is then the
// lowest cap, below the usual steps.
//
//   * it starts at the lowest cap, the cadence most games can hold;
//   * after a 1 s window with no late source frame it tries the next cap up;
//   * as soon as more than late_limit of a window's expected source frames came late it
//     drops (without waiting for the window to end) to the highest cap at or below the
//     median of the game's frame rate over the last few windows, at least one step; a
//     failed trial doubles the wait before the next one (up to backoff_max), a passed
//     one resets it.
//
// A late source frame is a latch interval longer than the cap allows: under the cap a game
// that keeps up is latched every floor or ceil(refresh / cap) vblanks. Jitter around the
// cap is not late: an interval that, with its neighbour before or after it, still fits two
// cap periods (2 then 4, or 4 then 2, at 30 fps on 90 Hz) is what a game locked in its own
// settings looks like when its presents sit near a vblank boundary, and the scheduler keeps
// such frames on the grid. (The scheduler's own repeats are not the signal: it absorbs an
// irregular source by filling longer intervals with more intermediates, which keeps flips
// regular but makes motion uneven.)
// Self-contained C99/C++ like kfg_sched.h, so the compositor and the tests share it.
#ifndef KFG_AUTO_H
#define KFG_AUTO_H

#include <stdbool.h>
#include <stdint.h>

#define KFG_AUTO_MAX_LEVELS 16
#define KFG_AUTO_HISTORY 5   // windows in the median

typedef struct kfg_auto {
	uint32_t refresh;        // Hz
	uint32_t levels[KFG_AUTO_MAX_LEVELS];  // caps in fps, ascending
	uint32_t nlevels;
	uint32_t level;          // index of the current cap
	uint32_t cap;            // levels[level]
	uint32_t mult;           // longest source interval in vblanks at this cap (scheduler multiplier)
	int64_t window_start;    // vblank of the current evaluation window; -1 = not started
	uint64_t late_base;
	uint64_t lates;          // latch intervals longer than the cap allows
	uint64_t latches;        // all latches
	uint64_t latch_base;
	double history[KFG_AUTO_HISTORY];  // the game's fps in the last windows
	uint32_t nhistory;
	int64_t last_latch;      // -1 = none yet
	int64_t prev_latch;      // the latch before last_latch; -1 = none
	bool pending;            // the last interval was long; the next one decides if it was late
	uint32_t window_vbl;     // vblanks per evaluation window
	uint32_t probe_vbl;      // base wait before trying a higher cap again
	uint32_t backoff;        // multiplies probe_vbl after failed trials
	uint32_t backoff_max;
	int64_t probe_at;        // first vblank at which a higher cap may be tried
	bool trial;              // the current cap is being tried
	double late_limit;       // tolerated fraction of late source frames per window
	uint64_t n_switch;       // cap changes, for status
} kfg_auto;

static inline uint32_t kfg_auto_div_up(uint32_t a, uint32_t b) { return (a + b - 1u) / b; }

static inline void kfg_auto_set_level(kfg_auto *a, uint32_t level)
{
	a->level = level;
	a->cap = a->levels[level];
	a->mult = kfg_auto_div_up(a->refresh, a->cap);
}

// Lowest floor a controller may set: a real frame is held at most 4 refreshes (refresh / 4
// rounded up) and the game never runs below 24 fps. 90 Hz: 24 (x3.75), 60 Hz: 24 (x2.5),
// 120 Hz: 30 (x4). Without a floor Auto stays at x3 at most; the plugin offers the 120 Hz x4
// floor only as a hidden, not recommended option.
// GSF 1.5: at most two generated frames between two real frames, so the floor is refresh / 3
// rounded up (90 Hz: 30, x3; 60 Hz: 20; 120 Hz: 40) and Auto stays within x2 to x3.
static inline uint32_t kfg_auto_min_floor(uint32_t refresh_hz)
{
	const uint32_t f = kfg_auto_div_up(refresh_hz, 3u);
	return f < 20u ? 20u : f;
}

// floor_fps: lowest cap; 0 = refresh / 3 (x3). Clamped to [kfg_auto_min_floor, refresh / 3].
static inline void kfg_auto_init_floor(kfg_auto *a, uint32_t refresh_hz, uint32_t floor_fps)
{
	if (refresh_hz < 30)
		refresh_hz = 30;
	a->refresh = refresh_hz;
	// refresh / 3 (rounded up, so never above x3) but at least 30 fps, then steps of 5 up to
	// refresh / 2. 60 Hz (a TV): 30 only, x2; base 20 at x3 would feel slow.
	const uint32_t hi = refresh_hz / 2, lo3 = kfg_auto_div_up(refresh_hz, 3);
	const uint32_t lo = lo3 >= 30u ? lo3 : hi < 30u ? hi : 30u;
	a->nlevels = 0;
	if (floor_fps) {
		const uint32_t mf = kfg_auto_min_floor(refresh_hz);
		if (floor_fps < mf)
			floor_fps = mf;
		if (floor_fps < lo)
			a->levels[a->nlevels++] = floor_fps;
	}
	a->levels[a->nlevels++] = lo;
	for (uint32_t c = (lo / 5 + 1) * 5; c < hi && a->nlevels < KFG_AUTO_MAX_LEVELS - 1; c += 5)
		a->levels[a->nlevels++] = c;
	if (hi > lo)
		a->levels[a->nlevels++] = hi;
	kfg_auto_set_level(a, 0);
	a->window_start = -1;
	a->late_base = 0;
	a->lates = 0;
	a->latches = 0;
	a->latch_base = 0;
	a->nhistory = 0;
	a->last_latch = -1;
	a->prev_latch = -1;
	a->pending = false;
	// Short memory: decisions rest on the last seconds only, and a failed trial is retried
	// after 8 s, at most 32 s after repeated failures, so a scene that gets lighter is used
	// within seconds instead of minutes.
	a->window_vbl = refresh_hz;        // 1 s
	a->probe_vbl = 8u * refresh_hz;    // 8 s
	a->backoff = 1;
	a->backoff_max = 4;
	a->probe_at = 0;                   // the first clean window may step up
	a->trial = false;
	a->late_limit = 0.03;
	a->n_switch = 0;
}

static inline void kfg_auto_init(kfg_auto *a, uint32_t refresh_hz)
{
	kfg_auto_init_floor(a, refresh_hz, 0);
}

// A source frame was latched at vblank vbl.
static inline void kfg_auto_on_latch(kfg_auto *a, int64_t vbl)
{
	a->latches++;
	// Longest regular interval at this cap, and the longest pair of intervals.
	const int64_t m = (int64_t)a->mult;
	const int64_t m2 = (int64_t)kfg_auto_div_up(2u * a->refresh, a->cap);
	if (a->last_latch >= 0) {
		const int64_t iv = vbl - a->last_latch;
		const bool pair_long = a->prev_latch < 0 || vbl - a->prev_latch > m2;
		if (a->pending) {
			// The long interval before this one: late unless this one gave the time back.
			if (pair_long)
				a->lates++;
			a->pending = false;
		}
		// Long, and not compensating an early frame before it: the next interval decides.
		if (iv > m && pair_long)
			a->pending = true;
	}
	a->prev_latch = a->last_latch;
	a->last_latch = vbl;
}

// The source stopped (pause, menu, focus change): its gap is not a late frame.
static inline void kfg_auto_reset_source(kfg_auto *a)
{
	a->last_latch = -1;
	a->prev_latch = -1;
	a->pending = false;
}

static inline double kfg_auto_median(const kfg_auto *a)
{
	double v[KFG_AUTO_HISTORY];
	const uint32_t n = a->nhistory < KFG_AUTO_HISTORY ? a->nhistory : KFG_AUTO_HISTORY;
	for (uint32_t i = 0; i < n; i++) {
		double x = a->history[i];
		uint32_t j = i;
		for (; j > 0 && v[j - 1] > x; j--)
			v[j] = v[j - 1];
		v[j] = x;
	}
	return n ? v[n / 2] : 0.0;
}

// Called once per vblank. Returns the scheduler multiplier for the following vblanks; the
// source cap is a->cap.
static inline uint32_t kfg_auto_step(kfg_auto *a, int64_t vbl)
{
	const uint64_t lates = a->lates;
	if (a->window_start < 0 || vbl < a->window_start) {
		a->window_start = vbl;
		a->late_base = lates;
		a->latch_base = a->latches;
		return a->mult;
	}
	const int64_t elapsed = vbl - a->window_start;
	const uint64_t late = lates - a->late_base;
	const double expected = (double)a->window_vbl * (double)a->cap / (double)a->refresh;
	const bool bad = (double)late > a->late_limit * expected;
	// A window already over the limit cannot recover: lates only grow. Drop at once instead
	// of at the end of the window, so a failed trial (a game locked at 30 in its own
	// settings, say) costs a few frames instead of a second.
	if (elapsed < (int64_t)a->window_vbl && !(a->level > 0 && bad))
		return a->mult;
	const uint32_t before = a->level;

	// The game's own rate over this window (a whole window, or the part before an early drop).
	// A game that stays below the cap without single long intervals (one locked at 30 in
	// its own settings, under a cap of 35: every interval is 3 vblanks, which 35 allows)
	// fails on its rate instead.
	bool slow = false;
	if (elapsed > 0) {
		const double fps = (double)(a->latches - a->latch_base) * (double)a->refresh / (double)elapsed;
		a->history[a->nhistory % KFG_AUTO_HISTORY] = fps;
		a->nhistory++;
		slow = elapsed >= (int64_t)a->window_vbl && fps < 0.95 * (double)a->cap;
	}
	if ((bad || slow) && a->level > 0) {
		// Highest cap the game's median rate holds, at least one step down. 3% headroom:
		// a game measured at exactly the cap would be late on every slow frame.
		const double med = kfg_auto_median(a);
		uint32_t l = a->level - 1;
		while (l > 0 && 0.97 * (double)a->levels[l] > med)
			l--;
		kfg_auto_set_level(a, l);
		if (a->trial && a->backoff < a->backoff_max)
			a->backoff *= 2;
		a->trial = false;
		a->probe_at = vbl + (int64_t)a->probe_vbl * a->backoff;
	} else if (!bad && !slow && a->trial) {
		a->trial = false;  // the cap held for a whole window
		a->backoff = 1;
		// Keep climbing while it holds: the next step needs only one more clean window.
		a->probe_at = vbl;
	} else if (late == 0 && vbl >= a->probe_at && a->level + 1 < a->nlevels) {
		kfg_auto_set_level(a, a->level + 1);
		a->trial = true;
	}
	if (a->level != before) {
		a->n_switch++;
		kfg_auto_reset_source(a);  // the first interval after a switch follows the old cap
	}
	a->window_start = vbl;
	a->late_base = lates;
	a->latch_base = a->latches;
	return a->mult;
}

#endif
