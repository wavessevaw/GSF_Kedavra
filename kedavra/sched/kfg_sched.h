// SPDX-License-Identifier: GPL-3.0-or-later
//
// KEDAVRA frame-generation presentation scheduler.
//
// The scheduler runs inside the compositor once per display refresh. All
// times are integer vblank indices of one fixed-refresh output, because the
// compositor latches application commits only on its own vblank wakeups.
//
//   latch L_i      : vblank index at which source frame i was latched.
//   time T_i       : vblank index that frame i stands for on the content timeline. Normally
//                    T_i = L_i. A source that holds the cap on average (a game locked in its
//                    own settings) but whose presents sit close to a vblank boundary latches
//                    one vblank early or late now and then (3, 4, 2 at x3). Its frames are
//                    still evenly spaced in game time, so they keep their place on the cap
//                    grid: T_i = T_(i-1) + multiplier. Timing intermediates by the jittered
//                    latches would move the picture at uneven speed although every flip is
//                    on time; the grid costs at most one vblank of delay instead.
//   content c      : vblank index (in source time) that the next output shows.
//   delay d        : T_latest - c, how far the display runs behind the newest source.
//
// For each output vblank k the scheduler advances c by one when that content
// can be shown in time, and otherwise repeats the previous output (a missed
// cadence slot) without advancing c. The delay therefore settles at the
// smallest value at which every intermediate is ready, and grows by one
// vblank when the source arrives late. After a long stable streak in which
// the next content position would also have been ready, one intermediate is
// skipped to give that latency back. c never decreases, so displayed content
// is always temporally ordered.
//
// Silk catch-up (GSF 1.4): content positions have KFG_SCHED_SUB steps per vblank. Latency the
// display owes (after a stall, or the give-back above) is not taken back in one jump of whole
// vblanks: the content advances faster for a few vblanks instead, so the motion speeds up
// briefly rather than skipping. Every output still costs one warp or one copy (no extra GPU
// work).
//
// Silk 2 (GSF 1.4.5): the content runs from real frame to real frame (kfg_sched_plan). Each
// output covers an even share of what is left to the next real frame; a slow-down or a
// catch-up changes how many outputs an interval gets, so every real frame lands exactly on a
// vblank and is shown. The delay added by a late source is bounded (KFG_REANCHOR), and what a
// game hitch costs is paid back by the catch-up (at most 1.75x) instead of a resynchronisation,
// which only follows a long stall (a source gap over KFG_STALL_GAP x max_delay: loading,
// suspend). A source steadily under the cap is waited for at its own rate, and the give-back
// keeps the delay its recent intervals needed (KFG_NEED_N), so its delay and speed stay
// constant instead of creeping up and catching up in a cycle.
//
// The header is self-contained C99/C++ so the same code runs in the
// compositor, in the simulator and in the unit tests.
#ifndef KFG_SCHED_H
#define KFG_SCHED_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KFG_SCHED_RING 16u
#define KFG_MAX_MULTIPLIER 4u
#define KFG_SCHED_SUB 8      // content sub-steps per vblank (speed-change granularity)
#ifndef KFG_FAST_DEBT
#define KFG_FAST_DEBT 3      // owed vblanks from which a catch-up may run at 1.75x instead of 1.5x
#endif
#ifndef KFG_JUMP_DEBT
#define KFG_JUMP_DEBT 1000   // owed vblanks from which the display jumps back to its steady delay
#endif
#ifndef KFG_SLOW_MIN
#define KFG_SLOW_MIN 4       // slowest slow-down: 1/4 speed
#endif
#ifndef KFG_NEED_N
#define KFG_NEED_N 256       // intervals over which the give-back keeps the delay the source needs
#endif
#ifndef KFG_NEED_MIN
#define KFG_NEED_MIN 2       // intervals that must have needed a delay before it is kept
#endif
#ifndef KFG_UNDERCAP
#define KFG_UNDERCAP 1       // wait for a source under the cap at its own rate
#endif
#ifndef KFG_STALL_GAP
#define KFG_STALL_GAP 2      // a source gap over this many max_delay is a long stall (resync)
#endif
#ifndef KFG_RESYNC_HARD
#define KFG_RESYNC_HARD 2    // resynchronise at this many max_delay behind whatever the cause
#endif
#ifndef KFG_REANCHOR
#define KFG_REANCHOR 1       // vblanks of delay a slow-down episode may add to the steady delay
#endif

enum kfg_show {
	KFG_SHOW_NONE = 0,  // nothing latched yet
	KFG_SHOW_REAL,      // an unmodified source frame
	KFG_SHOW_GEN,       // generated intermediate between src_a and src_b
	KFG_SHOW_REPEAT,    // previous output repeated: a missed cadence slot
};

enum kfg_repeat_reason {
	KFG_REPEAT_NONE = 0,
	KFG_REPEAT_SOURCE_LATE,   // the source frame that bounds this content is not latched
	KFG_REPEAT_ME_LATE,       // motion analysis for the pair is not complete
	KFG_REPEAT_CADENCE,       // source interval longer than the multiplier can fill
	KFG_REPEAT_WARMUP,        // building the delay after enabling generation
};

enum kfg_state {
	KFG_STATE_OFF = 0,        // passthrough: newest frame, no added latency
	KFG_STATE_ACTIVE,         // generating
	KFG_STATE_DEGRADED,       // generation suspended because analysis cannot keep up
};

typedef struct kfg_sched_config {
	uint32_t multiplier;        // 1 = off, 2 = x2, 3 = x3
	uint32_t catchup_streak;    // stable vblanks required before giving latency back
	uint32_t max_delay;         // vblanks; larger delay resynchronises to the newest source
	uint32_t me_lag_budget;     // vblanks from latch to usable analysis that are tolerated
	uint32_t degrade_misses;    // sources over budget within the last degrade_window sources
	uint32_t degrade_window;    // that suspend generation (<= 64)
	uint32_t recover_vblanks;   // suspension length before generation is retried
	uint32_t max_fill_span;     // late source: fill intervals up to this many vblanks with
	                            // one intermediate per vblank; longer ones quantise to 1/multiplier
	uint32_t cadence_frames;    // the cap's cadence: cadence_frames source frames every
	uint32_t cadence_vblanks;   // cadence_vblanks vblanks (40 fps at 90 Hz: 4 in 9; x3: 1 in 3)
	uint32_t giveback_calm;     // vblanks without a slow-down or a repeat before latency is given back
} kfg_sched_config;

typedef struct kfg_source_slot {
	uint64_t id;
	int64_t latch;       // vblank index of the latch
	int64_t t;           // position on the content timeline (see the header comment)
	int64_t me_ready;    // first vblank whose decision may use the pair (prev, this); INT64_MAX = pending
	bool me_ok;          // analysis succeeded (false: failure, show reals only for this pair)
	bool cut;            // scene cut between prev and this
	bool valid;
} kfg_source_slot;

typedef struct kfg_decision {
	enum kfg_show show;
	enum kfg_repeat_reason reason;
	enum kfg_state state;
	uint64_t src_a;      // REAL: the frame. GEN: earlier endpoint.
	uint64_t src_b;      // GEN: later endpoint.
	uint32_t phase_num;  // GEN: phase = num / den in (0, 1)
	uint32_t phase_den;
	int64_t content;     // content vblank index shown (unchanged for REPEAT)
	int64_t delay;       // L_latest - content after this decision
	bool skipped;        // this decision jumped content forward by whole vblanks (resync)
	bool catchup;        // this decision advanced content faster than one vblank (silk catch-up)
	int64_t pos;         // content position in 1/KFG_SCHED_SUB vblank (content * SUB + sub-step)
} kfg_decision;

typedef struct kfg_sched {
	kfg_sched_config cfg;
	kfg_source_slot ring[KFG_SCHED_RING];
	uint32_t head;          // next write position
	uint32_t count;
	int64_t content;        // last shown content index; INT64_MIN before any output
	uint32_t frac;          // sub-vblank part of the last shown content position (0..SUB-1)
	int64_t catchup;        // content the display still owes, in 1/SUB vblank
	uint32_t last_gen_phase_den;
	int64_t last_vblank;
	uint64_t last_gen_pair; // src_b of the last generated pair (cadence de-duplication)
	uint32_t last_gen_phase_num;
	uint32_t ahead_streak;
	int64_t wall_ref;       // established k - content of the steady cadence; -1 unknown
	int64_t realign_k;      // vblank of the last realignment
	enum kfg_state state;
	bool want_on;
	bool warming;           // generation enabled, first intermediate not shown yet
	int64_t degraded_until;
	uint8_t me_late_hist[64];  // per-source ring: analysis over budget
	uint32_t me_late_pos;
	uint32_t me_late_count;
	bool degrade_pending;
	int32_t interval_avg16;    // running mean of latch intervals, 1/16 vblank; 0 = none
	int32_t interval_slow;     // the same over about 64 frames, 1/1024 vblank: steadily under the cap?
	int8_t need_hist[KFG_NEED_N > 0 ? KFG_NEED_N : 1];  // per interval: least steady delay at full speed
	uint32_t need_pos;
	int64_t trouble_k;         // last vblank with a slow-down or a repeat
	int64_t giveback_k;        // last vblank latency was given back
	int64_t fail_k;            // last vblank a give-back near wall_floor ran short
	uint32_t giveback_wait;    // calm vblanks needed before a give-back near wall_floor
	int64_t wall_floor;        // lowest steady delay reached since the cadence was set up
	int64_t episode_ref;       // steady delay when the current slow-down/repeat episode began
	bool episode_overdue;      // ... and a source was overdue during it
	uint64_t plan_id;          // target real frame of a catch-up across a short interval (0: the next one)
	// Counters for telemetry.
	uint64_t n_real, n_gen, n_repeat, n_skip, n_resync, n_degrade, n_catchup;
	uint64_t n_repeat_reason[5];
	uint64_t n_slow, n_fast;      // outputs that advanced content less / more than one vblank
	uint64_t n_jump;              // outputs that advanced content two vblanks or more (resyncs included)
	uint64_t n_real_missed;       // source frames the content passed without showing them
} kfg_sched;

static inline kfg_sched_config kfg_sched_default_config(uint32_t multiplier)
{
	kfg_sched_config c;
	c.multiplier = multiplier;
	c.catchup_streak = 90;   // 1 s at 90 Hz
	c.max_delay = 12;
	c.me_lag_budget = 2;  // analysis is split over two vblanks after the latch
	c.degrade_misses = 8;
	c.degrade_window = 32;
	c.recover_vblanks = 180;
	c.max_fill_span = 2u * multiplier + 4u;
	c.cadence_frames = 1;
	c.cadence_vblanks = multiplier;
	c.giveback_calm = 90;     // 1 s at 90 Hz
	return c;
}

static inline void kfg_sched_init(kfg_sched *s, const kfg_sched_config *cfg)
{
	memset(s, 0, sizeof(*s));
	s->cfg = *cfg;
	if (s->cfg.multiplier > KFG_MAX_MULTIPLIER)
		s->cfg.multiplier = KFG_MAX_MULTIPLIER;
	if (s->cfg.degrade_window > 64)
		s->cfg.degrade_window = 64;
	s->content = INT64_MIN;
	s->last_vblank = INT64_MIN;
	s->wall_ref = -1;
	s->giveback_wait = 0;
	s->wall_floor = 0;
	s->giveback_k = INT64_MIN / 2;
	s->realign_k = INT64_MIN / 2;
	s->want_on = s->cfg.multiplier > 1;
	s->state = KFG_STATE_OFF;
}

static inline const kfg_source_slot *kfg_sched_slot(const kfg_sched *s, uint32_t back)
{
	// back = 0 is the newest latched source.
	if (back >= s->count)
		return NULL;
	return &s->ring[(s->head + KFG_SCHED_RING - 1u - back) % KFG_SCHED_RING];
}

// Longest gap between consecutive sources after content position c (vblanks): a single
// interval longer than max_delay is a stall that a resynchronisation should skip.
static inline int64_t kfg_sched_stall_gap(const kfg_sched *s, int64_t c)
{
	int64_t gap = 0;
	for (uint32_t i = 0; i + 1 < s->count; i++) {
		const kfg_source_slot *b = kfg_sched_slot(s, i), *a = kfg_sched_slot(s, i + 1);
		if (b->t - a->t > gap)
			gap = b->t - a->t;
		if (a->t <= c)
			break;
	}
	if (s->count == 1)
		gap = INT64_MAX / 4;  // nothing to play towards it
	return gap;
}

static inline kfg_source_slot *kfg_sched_find(kfg_sched *s, uint64_t id)
{
	for (uint32_t i = 0; i < s->count; i++) {
		kfg_source_slot *slot = &s->ring[(s->head + KFG_SCHED_RING - 1u - i) % KFG_SCHED_RING];
		if (slot->id == id)
			return slot;
	}
	return NULL;
}

// Forget history: called on enable/disable, refresh change, resize, suspend.
static inline void kfg_sched_reset_history(kfg_sched *s)
{
	s->count = 0;
	s->head = 0;
	s->ahead_streak = 0;
	s->last_gen_pair = 0;
	s->last_gen_phase_num = 0;
	s->last_gen_phase_den = 0;
	s->wall_ref = -1;
	s->giveback_wait = 0;
	s->wall_floor = 0;
	s->giveback_k = INT64_MIN / 2;
	s->warming = true;
	s->interval_avg16 = 0;
	s->interval_slow = 0;
	memset(s->need_hist, 0, sizeof(s->need_hist));
	s->need_pos = 0;
	s->catchup = 0;
	s->plan_id = 0;
}

static inline uint32_t kfg_sched_gcd(uint32_t a, uint32_t b);

// The cap's cadence: a base cap that does not divide the refresh (40 fps at 90 Hz) releases the
// game on a repeating pattern of vblank gaps (2, 2, 2, 3). Knowing the pattern, the scheduler
// expects each source where it really comes instead of at the mean, so a 2-vblank gap is not
// taken for an early frame and a 3-vblank gap for a late one. 0 or a refresh at or below the
// cap: the whole multiplier.
static inline void kfg_sched_set_cadence(kfg_sched *s, uint32_t refresh_hz, uint32_t cap_fps)
{
	uint32_t frames = 1, vblanks = s->cfg.multiplier;
	if (cap_fps > 0 && refresh_hz > cap_fps) {
		const uint32_t g = kfg_sched_gcd(refresh_hz, cap_fps);
		frames = cap_fps / g;
		vblanks = refresh_hz / g;
		if (frames >= KFG_SCHED_RING - 1u) {
			// The pattern is longer than the history: the mean gap it is.
			frames = 1;
			vblanks = (refresh_hz + cap_fps / 2) / cap_fps;
		}
	}
	if (frames == s->cfg.cadence_frames && vblanks == s->cfg.cadence_vblanks)
		return;
	s->cfg.cadence_frames = frames;
	s->cfg.cadence_vblanks = vblanks;
	s->interval_avg16 = 0;
	s->interval_slow = 0;
	memset(s->need_hist, 0, sizeof(s->need_hist));
	s->need_pos = 0;
}

// The content gap expected before the next source: the cadence's own gap one pattern back once
// the history holds a whole pattern in step with it, else the mean gap. 0: not known yet.
static inline int64_t kfg_sched_next_gap(const kfg_sched *s)
{
	const uint32_t P = s->cfg.cadence_frames ? s->cfg.cadence_frames : 1u;
	const int64_t V = s->cfg.cadence_vblanks ? (int64_t)s->cfg.cadence_vblanks : (int64_t)s->cfg.multiplier;
	if (P == 1)
		return V;
	if (s->count > P) {
		const kfg_source_slot *first = kfg_sched_slot(s, P);
		const kfg_source_slot *newest = kfg_sched_slot(s, 0);
		if (newest->t - first->t == V) {
			const kfg_source_slot *a = kfg_sched_slot(s, P);
			const kfg_source_slot *b = kfg_sched_slot(s, P - 1u);
			return b->t - a->t;
		}
	}
	return (V + (int64_t)P / 2) / (int64_t)P;
}

// Gap to the next latch the scheduler waits for: the cap's cadence, or the source's own measured
// rate when it runs steadily under the cap (a GPU-bound game at 24.8 fps under cap 30 latches
// every 3 or 4 vblanks: waiting for it on the cap's grid would call every 4-vblank interval
// late, slow down for it and catch up after).
static inline bool kfg_sched_undercap(const kfg_sched *s)
{
	const int64_t g = kfg_sched_next_gap(s);
	return KFG_UNDERCAP && s->count >= 4 && 1024 * g + 192 < (int64_t)s->interval_slow &&
	       16 * g + 3 < (int64_t)s->interval_avg16;
}

static inline int64_t kfg_sched_expect_gap(const kfg_sched *s)
{
	const int64_t g = kfg_sched_next_gap(s);
	const int64_t own = ((int64_t)s->interval_slow + 1023) / 1024;  // rounded up
	return kfg_sched_undercap(s) && own > g ? own : g;
}

// Source frame `id` was latched at vblank `vbl`. Latches must be strictly increasing.
// Returns false (and ignores the frame) when the latch is not newer than the last one.
static inline bool kfg_sched_on_latch(kfg_sched *s, uint64_t id, int64_t vbl)
{
	const kfg_source_slot *newest = kfg_sched_slot(s, 0);
	if (newest && vbl <= newest->latch)
		return false;
	kfg_source_slot *slot = &s->ring[s->head];
	slot->id = id;
	slot->latch = vbl;
	slot->t = vbl;
	const uint32_t n = s->cfg.multiplier;
	if (newest && n > 1) {
		const int64_t iv = vbl - newest->latch;
		if (iv <= 4 * (int64_t)n) {
			// Mean over about 8 frames; long gaps (pause, loading) are not cadence.
			if (s->interval_avg16 == 0)
				s->interval_avg16 = (int32_t)(16 * iv);
			else
				s->interval_avg16 += (int32_t)((16 * iv - s->interval_avg16) / 8);
			// The steady rate leaves out long frames (hitches, stalls): they are not cadence.
			if (iv <= kfg_sched_next_gap(s) + 2) {
				if (s->interval_slow == 0)
					s->interval_slow = (int32_t)(1024 * iv);
				else
					s->interval_slow += (int32_t)((1024 * iv - s->interval_slow) / 64);
			}
		}
		// The grid of the cap's cadence (whole multiplier: every n vblanks; 40 at 90 Hz: the
		// 2, 2, 2, 3 pattern).
		const int64_t P = s->cfg.cadence_frames ? (int64_t)s->cfg.cadence_frames : 1;
		const int64_t V = s->cfg.cadence_vblanks ? (int64_t)s->cfg.cadence_vblanks : (int64_t)n;
		const int64_t grid = newest->t + kfg_sched_next_gap(s);
		const int64_t dev = (int64_t)s->interval_avg16 * P - 16 * V;
		// On the grid only while the mean interval is the cap (within 3/16 vblank) and this
		// latch is at most one vblank off it; a slower or faster source keeps its latches.
		if (dev >= -3 * P && dev <= 3 * P && vbl - grid >= -1 && vbl - grid <= 1)
			slot->t = grid;
	}
	if (newest && slot->t <= newest->t)
		slot->t = newest->t + 1;
	slot->me_ready = INT64_MAX;
	slot->me_ok = false;
	slot->cut = false;
	slot->valid = true;
	s->head = (s->head + 1u) % KFG_SCHED_RING;
	if (s->count < KFG_SCHED_RING)
		s->count++;
	return true;
}

// Motion analysis for the pair ending at `id` completes in time for decisions at
// vblank >= ready_vbl. ok=false marks analysis failure; cut=true marks a scene cut.
static inline void kfg_sched_on_me(kfg_sched *s, uint64_t id, int64_t ready_vbl, bool ok, bool cut)
{
	kfg_source_slot *slot = kfg_sched_find(s, id);
	if (!slot)
		return;
	slot->me_ready = ready_vbl;
	slot->me_ok = ok;
	slot->cut = cut;
	// The steady delay W at which the content leaves the earlier source of this pair at full
	// speed: the first output after it (W + 1 vblanks after its content time) needs this pair.
	for (uint32_t i = 0; KFG_NEED_N > 0 && i + 1 < s->count; i++) {
		if (kfg_sched_slot(s, i) != slot)
			continue;
		const kfg_source_slot *prev = kfg_sched_slot(s, i + 1);
		// The source's own cadence only: up to one vblank over its mean interval (a 3/4/5
		// mix around 3.6 vblanks counts, a 6-vblank long frame is a hitch, paid back instead).
		const int64_t own = ((int64_t)s->interval_slow + 1023) / 1024;
		if (kfg_sched_undercap(s) && slot->t - prev->t <= own + 1) {
			int64_t need = ready_vbl - prev->t - 1;
			need = need < 0 ? 0 : need > 100 ? 100 : need;
			s->need_hist[s->need_pos++ % (KFG_NEED_N > 0 ? KFG_NEED_N : 1)] = (int8_t)need;
		}
		break;
	}
	if (s->state != KFG_STATE_ACTIVE)
		return;
	const uint32_t w = s->cfg.degrade_window ? s->cfg.degrade_window : 1u;
	const bool late = !ok || ready_vbl - slot->latch > (int64_t)s->cfg.me_lag_budget;
	const uint32_t idx = s->me_late_pos++ % w;
	if (s->me_late_hist[idx])
		s->me_late_count--;
	s->me_late_hist[idx] = late ? 1u : 0u;
	if (late)
		s->me_late_count++;
	if (s->me_late_count >= s->cfg.degrade_misses)
		s->degrade_pending = true;
}

// The steady delay recent intervals needed to play at full speed: the largest that at least
// KFG_NEED_MIN of them needed (a single long interval is a late frame, not the cadence).
static inline int64_t kfg_sched_need(const kfg_sched *s)
{
	int64_t top[KFG_NEED_MIN > 0 ? KFG_NEED_MIN : 1] = {0};
	for (uint32_t i = 0; i < (KFG_NEED_N > 0 ? KFG_NEED_N : 0); i++) {
		int64_t v = s->need_hist[i];
		for (int j = 0; j < KFG_NEED_MIN; j++)
			if (v > top[j]) {
				const int64_t t = top[j];
				top[j] = v;
				v = t;
			}
	}
	return top[KFG_NEED_MIN > 0 ? KFG_NEED_MIN - 1 : 0];
}

// Change the multiplier at runtime. 1 disables generation.
static inline void kfg_sched_set_multiplier(kfg_sched *s, uint32_t multiplier)
{
	if (multiplier > KFG_MAX_MULTIPLIER)
		multiplier = KFG_MAX_MULTIPLIER;
	s->cfg.multiplier = multiplier;
	s->cfg.max_fill_span = 2u * multiplier + 4u;
	s->cfg.me_lag_budget = 2;
	s->cfg.cadence_frames = 1;
	s->cfg.cadence_vblanks = multiplier;
	s->want_on = multiplier > 1;
	s->interval_avg16 = 0;
	s->interval_slow = 0;
	memset(s->need_hist, 0, sizeof(s->need_hist));
	s->need_pos = 0;  // the cap changed: the old cadence says nothing
	// The steady delay depends on the multiplier; rebuild it.
	s->wall_ref = -1;
	s->giveback_wait = 0;
	s->wall_floor = 0;
	s->giveback_k = INT64_MIN / 2;
	s->warming = true;
	s->catchup = 0;
	s->plan_id = 0;
}

// Locate the source interval containing content position c.
// Returns the index (back) of the earlier endpoint, or -1 when c is newer than the newest latch
// or older than retained history.
static inline int kfg_sched_locate(const kfg_sched *s, int64_t c, const kfg_source_slot **a, const kfg_source_slot **b)
{
	*a = *b = NULL;
	for (uint32_t back = 0; back < s->count; back++) {
		const kfg_source_slot *slot = kfg_sched_slot(s, back);
		if (slot->t <= c) {
			*a = slot;
			*b = back ? kfg_sched_slot(s, back - 1u) : NULL;
			return (int)back;
		}
	}
	return -1;
}

typedef struct kfg_candidate {
	bool ok;
	enum kfg_show show;
	enum kfg_repeat_reason reason;
	uint64_t src_a, src_b;
	uint32_t num, den;
} kfg_candidate;

static inline uint32_t kfg_sched_gcd(uint32_t a, uint32_t b)
{
	while (b) {
		const uint32_t t = a % b;
		a = b;
		b = t;
	}
	return a ? a : 1u;
}

// Floor division of a content position (1/SUB vblank) into its vblank.
static inline int64_t kfg_sched_vbl(int64_t p)
{
	return p >= 0 ? p / KFG_SCHED_SUB : -((-p + KFG_SCHED_SUB - 1) / KFG_SCHED_SUB);
}

// Can content position p (in 1/SUB vblank) be shown at the decision for vblank k?
static inline kfg_candidate kfg_sched_eval_pos(const kfg_sched *s, int64_t p, int64_t k)
{
	kfg_candidate r;
	memset(&r, 0, sizeof(r));
	const int64_t c = kfg_sched_vbl(p);
	const int64_t f = p - c * KFG_SCHED_SUB;
	const kfg_source_slot *a, *b;
	if (kfg_sched_locate(s, c, &a, &b) < 0) {
		r.reason = KFG_REPEAT_SOURCE_LATE;
		return r;
	}
	if (a->t == c && f == 0) {
		r.ok = true;
		r.show = KFG_SHOW_REAL;
		r.src_a = a->id;
		return r;
	}
	if (!b) {
		// c lies after the newest latch: the bounding frame has not arrived.
		r.reason = KFG_REPEAT_SOURCE_LATE;
		return r;
	}
	const int64_t span = b->t - a->t;
	const int64_t off = (c - a->t) * KFG_SCHED_SUB + f;  // both in 1/SUB vblank
	const int64_t span_q = span * KFG_SCHED_SUB;
	if (b->me_ready > k) {
		r.reason = KFG_REPEAT_ME_LATE;
		return r;
	}
	if (b->cut || !b->me_ok) {
		// No valid motion: hold the nearer real frame instead of blending across a cut.
		r.ok = true;
		r.show = KFG_SHOW_REAL;
		r.src_a = (2 * off < span_q) ? a->id : b->id;
		return r;
	}
	// Every interval is filled at the content position, long ones too (GSF 1.4.5: the plan
	// decides how many outputs an interval gets, so a long interval after a stall is crossed
	// evenly instead of in steps of 1/multiplier with repeats between them).
	const uint32_t num = (uint32_t)off, den = (uint32_t)span_q;
	const uint32_t g = kfg_sched_gcd(num, den);
	r.ok = true;
	r.show = KFG_SHOW_GEN;
	r.src_a = a->id;
	r.src_b = b->id;
	r.num = num / g;
	r.den = den / g;
	return r;
}

// Can whole content position c be shown at the decision for vblank k?
static inline kfg_candidate kfg_sched_eval(const kfg_sched *s, int64_t c, int64_t k)
{
	return kfg_sched_eval_pos(s, c * KFG_SCHED_SUB, k);
}

// The analysis lag the last pairs actually had (the budget while none is known yet).
static inline int64_t kfg_sched_lag(const kfg_sched *s)
{
	int64_t lag = (int64_t)s->cfg.me_lag_budget;
	for (uint32_t back = 0; back < s->count && back < 4; back++) {
		const kfg_source_slot *slot = kfg_sched_slot(s, back);
		if (slot->me_ready != INT64_MAX) {
			lag = slot->me_ready - slot->latch;
			break;
		}
	}
	return lag < 0 ? 0 : lag;
}

// First vblank whose output may show content after the source `back` (the vblank its pair with
// the next source is analysed): known once that source is latched; for the newest, the next
// source where the cap's cadence puts it (from the cadence grid, so a late latch snapped back
// onto it does not delay the next one; the next vblank when it is already overdue).
static inline int64_t kfg_sched_after_ready(const kfg_sched *s, int64_t k, int back)
{
	const int64_t lag = kfg_sched_lag(s);
	if (back > 0) {
		const kfg_source_slot *c = kfg_sched_slot(s, (uint32_t)back - 1u);
		return c->me_ready != INT64_MAX ? c->me_ready : c->latch + lag;
	}
	const kfg_source_slot *newest = kfg_sched_slot(s, 0);
	int64_t next_latch = (newest->t < newest->latch ? newest->t : newest->latch) + kfg_sched_expect_gap(s);
	if (next_latch <= k)
		next_latch = k + 1;
	return next_latch + lag;
}

enum { KFG_PLAN_NORMAL = 0, KFG_PLAN_FAST, KFG_PLAN_SLOW };

// Silk 2 (GSF 1.4.5): the content position of this output. The content runs from real frame to
// real frame: each output covers an even share of what is left to the next real frame, over the
// number of outputs the plan gives it. Normally that is one output per vblank of content; to
// pay back delay (after a give-back or a stall) one output less per interval, at most 1.5x;
// before a source that will not be analysed in time, more, at least 1/4x. A real frame
// therefore always lands exactly on a vblank, and a speed change spans whole intervals instead
// of leaving the content between vblanks. Intervals of two vblanks or less cannot be shortened
// at 1.5x: the catch-up then spans the next interval as well (the real frame between them is
// passed). `back` is the ring index of the interval's earlier endpoint (> 0).
static inline int64_t kfg_sched_plan(const kfg_sched *s, int64_t k, int64_t base, int *target, bool span, int *mode, bool *held)
{
	const int64_t Q = KFG_SCHED_SUB;
	*mode = KFG_PLAN_NORMAL;
	*held = false;
	int tb = *target;  // the target real frame
	const kfg_source_slot *b = kfg_sched_slot(s, (uint32_t)tb);
	int64_t D = b->t * Q - base;
	int64_t normal = k - 1 + (D + Q - 1) / Q;  // vblank at which b is shown at normal speed
	int64_t arrive = normal;
	int64_t want = INT64_MAX;
	if (s->wall_ref >= 0) {
		want = b->t + s->wall_ref;  // where the steady delay shows b
		const kfg_source_slot *a = kfg_sched_slot(s, (uint32_t)tb + 1u);
		if (span && want < normal && k - 1 + (2 * D + 3 * Q - 1) / (3 * Q) >= normal && tb > 0 &&
		    (!a || b->t - a->t <= 2)) {
			// An interval too short to shorten (two vblanks or less): catch up over this and
			// the next interval. What is left of a longer one waits for the next interval
			// instead, which does not pass a real frame.
			const kfg_source_slot *b2 = kfg_sched_slot(s, (uint32_t)tb - 1u);
			const int64_t D2 = b2->t * Q - base;
			const int64_t normal2 = k - 1 + (D2 + Q - 1) / Q;
			const int64_t lo2 = k - 1 + (2 * D2 + 3 * Q - 1) / (3 * Q);
			int64_t arrive2 = b2->t + s->wall_ref;
			if (arrive2 < lo2)
				arrive2 = lo2;
			// The pair after b must be analysed when the content passes b.
			const int64_t m2 = arrive2 - k + 1;
			const int64_t pass = k + (D * m2) / D2;  // vblank of the first output past b
			if (arrive2 < normal2 && b2->me_ready <= pass && kfg_sched_after_ready(s, k, tb - 1) - 1 <= arrive2) {
				tb--;
				b = b2;
				D = D2;
				normal = normal2;
				arrive = normal2;
				want = arrive2;
			}
		}
		if (want < arrive) {
			// 1.5x; up to 1.75x while the display owes KFG_FAST_DEBT vblanks or more (after a
			// game stall), so a long debt costs fewer sped-up outputs.
			const int64_t num = normal - want >= KFG_FAST_DEBT ? 7 : 6;
			const int64_t lo = k - 1 + (4 * D + num * Q - 1) / (num * Q);
			arrive = want > lo ? want : lo;
		}
	}
	if (want != INT64_MAX && normal - want >= KFG_JUMP_DEBT && want >= k) {
		// A long game stall: one jump to where the steady delay shows b, then normal steps.
		*mode = KFG_PLAN_FAST;
		*target = tb;
		const int64_t p = b->t * Q - (want - k) * Q;
		return p > base + Q ? p : base + Q;
	}
	// Content past b needs the next pair analysed: arrive no earlier than the vblank before.
	const int64_t ready = kfg_sched_after_ready(s, k, tb) - 1;
	if (arrive < ready) {
		int64_t hi = k - 1 + (KFG_SLOW_MIN * D) / Q;  // the slowest speed, 1/KFG_SLOW_MIN
		if (hi < normal)
			hi = normal;
		arrive = ready < hi ? ready : hi;
		if (want < normal)
			*held = true;  // the catch-up waits for the source
	}
	*target = tb;
	if (arrive < k)
		arrive = k;
	if (arrive < normal)
		*mode = KFG_PLAN_FAST;
	else if (arrive > normal)
		*mode = KFG_PLAN_SLOW;
	const int64_t m = arrive - k + 1;
	int64_t step = (2 * D + m) / (2 * m);  // D / m, rounded; the last output lands exactly on b
	if (step < 1)
		step = 1;
	if (step > D)
		step = D;
	return base + step;
}

// Telemetry: whole-vblank jumps and real frames the content passed without showing them.
static inline void kfg_sched_count_pass(kfg_sched *s, const kfg_decision *d, int64_t from, int64_t to)
{
	const int64_t Q = KFG_SCHED_SUB;
	if (to <= from)
		return;
	if (to - from >= 2 * Q)
		s->n_jump++;
	for (uint32_t back = 0; back < s->count; back++) {
		const kfg_source_slot *slot = kfg_sched_slot(s, back);
		const int64_t at = slot->t * Q;
		if (at <= from)
			break;
		if (at < to || (at == to && !(d->show == KFG_SHOW_REAL && d->src_a == slot->id)))
			s->n_real_missed++;
	}
}

// Decide the output for vblank k (strictly increasing between calls).
static inline kfg_decision kfg_sched_decide(kfg_sched *s, int64_t k)
{
	kfg_decision d;
	memset(&d, 0, sizeof(d));
	const kfg_source_slot *newest = kfg_sched_slot(s, 0);
	s->last_vblank = k;

	if (!newest) {
		d.show = KFG_SHOW_NONE;
		d.state = s->state;
		d.content = s->content;
		return d;
	}

	// State transitions.
	if (!s->want_on) {
		s->state = KFG_STATE_OFF;
	} else if (s->state == KFG_STATE_ACTIVE && s->degrade_pending) {
		// Analysis cannot keep up: stop generating rather than keep producing late frames.
		s->state = KFG_STATE_DEGRADED;
		s->degraded_until = k + (int64_t)s->cfg.recover_vblanks;
		s->degrade_pending = false;
		s->n_degrade++;
	} else if (s->state == KFG_STATE_DEGRADED) {
		if (k >= s->degraded_until) {
			s->state = KFG_STATE_ACTIVE;
			s->warming = true;
			s->wall_ref = -1;
			s->giveback_wait = 0;
			s->wall_floor = 0;
			s->giveback_k = INT64_MIN / 2;
			memset(s->me_late_hist, 0, sizeof(s->me_late_hist));
			s->me_late_count = 0;
			s->me_late_pos = 0;
		}
	} else if (s->state == KFG_STATE_OFF) {
		s->state = KFG_STATE_ACTIVE;
		s->ahead_streak = 0;
		s->warming = true;
		s->wall_ref = -1;
		s->giveback_wait = 0;
		s->wall_floor = 0;
		s->giveback_k = INT64_MIN / 2;
	}

	if (s->state != KFG_STATE_ACTIVE) {
		// Passthrough: newest frame. Content only moves forward.
		d.state = s->state;
		if (s->content != INT64_MIN && newest->t <= s->content) {
			d.show = KFG_SHOW_REPEAT;
			d.reason = KFG_REPEAT_SOURCE_LATE;
			s->n_repeat++;
		} else {
			d.show = KFG_SHOW_REAL;
			d.src_a = newest->id;
			s->content = newest->t;
			s->frac = 0;
			s->n_real++;
		}
		s->catchup = 0;
		d.content = s->content;
		d.pos = s->content * KFG_SCHED_SUB + (int64_t)s->frac;
		d.delay = newest->t - s->content;
		return d;
	}

	d.state = KFG_STATE_ACTIVE;
	const int64_t Q = KFG_SCHED_SUB;
	const bool first = s->content == INT64_MIN;
	if (first) {
		s->content = newest->t - 1; // first output shows the newest frame
		s->frac = 0;
	}

	const int64_t p_before = s->content * Q + (int64_t)s->frac;  // for the telemetry counters
	// Too far behind while warming up (a new history), after a long stall (suspend, loading:
	// one source interval longer than max_delay), or hopelessly behind (twice max_delay):
	// resynchronise forward. A shorter
	// hitch (a long game frame, late analysis) is paid back by the plan's catch-up instead:
	// a resynchronisation skips everything between the shown content and the newest frame.
	if (newest->t - s->content > (int64_t)s->cfg.max_delay &&
	    (s->warming || newest->t - s->content > KFG_RESYNC_HARD * (int64_t)s->cfg.max_delay ||
	     kfg_sched_stall_gap(s, s->content) > KFG_STALL_GAP * (int64_t)s->cfg.max_delay)) {
		s->content = newest->t - 1;
		s->frac = 0;
		s->catchup = 0;
		s->ahead_streak = 0;
		s->wall_ref = -1;
		s->giveback_wait = 0;
		s->wall_floor = 0;
		s->giveback_k = INT64_MIN / 2;
		s->warming = true;
		s->plan_id = 0;
		s->n_resync++;
		d.skipped = true;
	}

	const int64_t base = s->content * Q + (int64_t)s->frac;
	// Silk 2: the content moves from real frame to real frame (kfg_sched_plan); a slow-down,
	// a catch-up and a give-back change how many outputs an interval gets, never where a real
	// frame lands.
	// The next source is overdue on the cap's cadence: a slow-down now is the game's, not a
	// give-back that went too far.
	const bool overdue = (newest->t < newest->latch ? newest->t : newest->latch) + kfg_sched_expect_gap(s) <= k;
	int64_t p = base + Q;
	int plan = KFG_PLAN_NORMAL;
	int64_t to_real = 0;  // what is left to the next real frame, 1/SUB vblank (0: none known)
	if (!s->warming) {
		const kfg_source_slot *a, *b;
		const int back = kfg_sched_locate(s, kfg_sched_vbl(base), &a, &b);
		if (back > 0) {
			to_real = b->t * Q - base;
			if (s->wall_ref >= 0 && k - s->trouble_k <= 2 * (int64_t)s->cfg.multiplier + 2) {
				// A slow-down or a repeat just added delay: the display is behind where its
				// steady delay would show the next real frame. It keeps up to KFG_REANCHOR
				// vblanks of that (the source runs later now: speeding up again right away
				// would only run short at the next late latch, and the give-back takes it back
				// after a calm stretch); the rest, a game stall, is paid back by the plan's
				// catch-up instead of piling up into a resynchronisation.
				const int64_t behind = k - 1 + (to_real + Q - 1) / Q - (b->t + s->wall_ref);
				int64_t cap = s->episode_ref + KFG_REANCHOR;
				if (cap > (int64_t)s->cfg.max_delay - 1)
					cap = (int64_t)s->cfg.max_delay - 1;
				int64_t to = s->wall_ref + behind;
				if (to > cap)
					to = cap;
				if (behind + s->wall_ref - s->episode_ref >= KFG_JUMP_DEBT && s->episode_ref >= 0) {
					// A long stall is jumped over (kfg_sched_plan): the steady delay it had stays.
					s->wall_ref = s->episode_ref;
					to = s->wall_ref;
				}
				if (to > s->wall_ref) {
					if (s->wall_ref <= s->wall_floor + 1 &&
					    k - s->giveback_k <= (s->episode_overdue ? (behind <= 1 ? 270 : -1) : 4 * (int64_t)s->cfg.multiplier + 4)) {
						// A give-back near the lowest delay ran short: right after it the
						// cadence needed more, or within 3 s a game frame came up to a vblank
						// late, which the vblank given back would have absorbed (a long stall
						// would not have been). Try it again later each time (up to every 30 s
						// at 90 Hz).
						s->giveback_wait = s->giveback_wait < 1350u ? 2u * s->giveback_wait : 2700u;
						s->fail_k = k;
					}
					s->wall_ref = to;
				}
			}
			// A catch-up across a short interval keeps its target until it gets there.
			int target = back - 1;
			bool span = true;
			if (s->plan_id) {
				for (int i = target - 1; i >= 0; i--) {
					if (kfg_sched_slot(s, (uint32_t)i)->id == s->plan_id) {
						target = i;
						span = false;
						break;
					}
				}
			}
			bool held;
			p = kfg_sched_plan(s, k, base, &target, span, &plan, &held);
			s->plan_id = target != back - 1 ? kfg_sched_slot(s, (uint32_t)target)->id : 0;
			if (held && s->wall_ref >= 0 && k - s->giveback_k <= 4 * (int64_t)s->cfg.multiplier + 4) {
				// The vblank just given back is not there: the next source is later than the
				// give-back assumed. Keep the delay, and try again later each time.
				s->wall_ref++;
				s->giveback_k = INT64_MIN / 2;
				s->giveback_wait = s->giveback_wait < 1350u ? 2u * s->giveback_wait : 2700u;
				s->fail_k = k;
				s->trouble_k = k;
				target = back - 1;
				s->plan_id = 0;
				p = kfg_sched_plan(s, k, base, &target, false, &plan, &held);
			}
		}
	}
	const bool slowed = plan == KFG_PLAN_SLOW;
	kfg_candidate cand = kfg_sched_eval_pos(s, p, k);
	if (!cand.ok && !s->warming && to_real > 0 && to_real <= Q &&
	    (cand.reason == KFG_REPEAT_SOURCE_LATE || cand.reason == KFG_REPEAT_ME_LATE)) {
		// The analysis of this pair is later than predicted, but the real frame that ends it
		// is within one normal step: show it instead of freezing.
		kfg_candidate tail = kfg_sched_eval_pos(s, base + to_real, k);
		if (tail.ok) {
			p = base + to_real;
			cand = tail;
		}
	}
	const int64_t step = p - base;

	// Latency give-back: after a calm stretch in which one more vblank of content was also
	// ready, the display owes one vblank less; the plan takes it back over the next interval.
	if (cand.ok) {
		kfg_candidate next = kfg_sched_eval_pos(s, p + Q, k);
		if (next.ok && plan != KFG_PLAN_FAST)
			s->ahead_streak++;
		else
			s->ahead_streak = 0;
		if (s->giveback_wait < s->cfg.giveback_calm)
			s->giveback_wait = s->cfg.giveback_calm;
		// Well above the lowest steady delay reached (after a game stall), soon after a calm
		// stretch; at it, only after a long streak, and later each time such a try failed.
		const bool far = s->wall_ref > s->wall_floor + 1;
		const int64_t wait = far ? (int64_t)s->cfg.giveback_calm : (int64_t)s->giveback_wait;
		const uint32_t streak = far ? s->cfg.giveback_calm / 2u : s->cfg.catchup_streak;
		// A source steadily under the cap (3- and 4-vblank intervals): never below what its
		// recent intervals needed, or it would slow down at every long interval after a
		// give-back and speed up again after the next. At the cap a long interval is a late
		// frame, and the give-back keeps the delay low.
		if (s->ahead_streak >= streak && k - s->trouble_k >= wait && s->wall_ref > 0 &&
		    (!kfg_sched_undercap(s) || s->wall_ref - 1 >= kfg_sched_need(s))) {
			// Only after a calm stretch (no slow-down, no repeat): a delay that keeps the
			// display just short of the source keeps it on the edge, and every late latch
			// would then slow it down and speed it up again.
			if (!far && k - s->fail_k > 4 * (int64_t)s->giveback_wait && s->giveback_wait > s->cfg.giveback_calm)
				s->giveback_wait /= 2u;  // the last tries held: try more often again
			s->wall_ref--;
			s->giveback_k = k;
			if (s->wall_ref < s->wall_floor)
				s->wall_floor = s->wall_ref;
			s->ahead_streak = 0;
			s->n_skip++;
		}
	} else {
		s->ahead_streak = 0;
	}

	if (!cand.ok && s->warming && cand.reason == KFG_REPEAT_SOURCE_LATE)
		cand.reason = KFG_REPEAT_WARMUP;

	const bool late = !cand.ok && (cand.reason == KFG_REPEAT_SOURCE_LATE || cand.reason == KFG_REPEAT_ME_LATE);
	if (late && s->wall_ref >= 0 && k - s->realign_k <= 2 * (int64_t)s->cfg.multiplier + 2) {
		// The last catch-up was too aggressive for current conditions: back off one vblank.
		if (s->wall_ref < (int64_t)s->cfg.max_delay)
			s->wall_ref++;
		s->realign_k = INT64_MIN / 2;
	}

	const int64_t pc = kfg_sched_vbl(p);
	if (!cand.ok || slowed) {
		// A new episode of trouble (a slow-down or a repeat) starts from the delay held so far.
		if (k - s->trouble_k > 2) {
			s->episode_ref = s->wall_ref;
			s->episode_overdue = false;
		}
		s->episode_overdue |= overdue;
		s->trouble_k = k;
	}
	if (!cand.ok) {
		d.show = KFG_SHOW_REPEAT;
		d.reason = cand.reason;
		s->n_repeat++;
		s->n_repeat_reason[cand.reason]++;
	} else {
		if (step != Q) {
			d.catchup = true;
			s->n_catchup++;
			if (step > Q) {
				s->realign_k = k;
				s->n_fast++;
			} else {
				s->n_slow++;
			}
		}
		s->content = pc;
		s->frac = (uint32_t)(p - pc * Q);
		if (cand.show == KFG_SHOW_GEN && cand.src_b == s->last_gen_pair && cand.num == s->last_gen_phase_num &&
		    cand.den == s->last_gen_phase_den) {
			// Same phase as the previous output: count as a repeat but let content advance so
			// the display does not fall behind.
			d.show = KFG_SHOW_REPEAT;
			d.reason = KFG_REPEAT_CADENCE;
			s->n_repeat++;
			s->n_repeat_reason[KFG_REPEAT_CADENCE]++;
		} else {
			d.show = cand.show;
			d.src_a = cand.src_a;
			d.src_b = cand.src_b;
			d.phase_num = cand.num;
			d.phase_den = cand.den;
			if (cand.show == KFG_SHOW_GEN) {
				s->last_gen_pair = cand.src_b;
				s->last_gen_phase_num = cand.num;
				s->last_gen_phase_den = cand.den;
				s->warming = false;
				s->n_gen++;
			} else {
				s->n_real++;
			}
		}
	}
	if (cand.reason == KFG_REPEAT_CADENCE && !cand.ok) {
		s->content = pc; // content advances on a cadence hole
		s->frac = (uint32_t)(p - pc * Q);
	}
	s->catchup = 0;
	if (cand.ok && !s->warming) {
		// Delay of this output in whole vblanks, rounded down (a position between two vblanks
		// of content counts as the later one).
		const int64_t now = k - (s->content + (s->frac ? 1 : 0));
		if (s->wall_ref < 0) {
			s->wall_ref = now;
			s->wall_floor = now;
		}
	}
	if (!first)
		kfg_sched_count_pass(s, &d, p_before, s->content * Q + (int64_t)s->frac);
	d.state = s->state;
	d.content = s->content;
	d.pos = s->content * Q + (int64_t)s->frac;
	d.delay = newest->t - s->content;
	return d;
}

static inline const char *kfg_show_name(enum kfg_show s)
{
	switch (s) {
	case KFG_SHOW_REAL: return "real";
	case KFG_SHOW_GEN: return "gen";
	case KFG_SHOW_REPEAT: return "repeat";
	default: return "none";
	}
}

static inline const char *kfg_reason_name(enum kfg_repeat_reason r)
{
	switch (r) {
	case KFG_REPEAT_SOURCE_LATE: return "source_late";
	case KFG_REPEAT_ME_LATE: return "me_late";
	case KFG_REPEAT_CADENCE: return "cadence";
	case KFG_REPEAT_WARMUP: return "warmup";
	default: return "";
	}
}

static inline const char *kfg_state_name(enum kfg_state s)
{
	switch (s) {
	case KFG_STATE_ACTIVE: return "active";
	case KFG_STATE_DEGRADED: return "degraded";
	default: return "off";
	}
}

#ifdef __cplusplus
}
#endif
#endif
