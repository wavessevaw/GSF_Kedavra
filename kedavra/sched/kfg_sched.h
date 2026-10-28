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
// vblanks: the content advances 1.25x (1.5x when far behind) for a few vblanks instead, so the
// motion speeds up briefly rather than skipping. Every output still costs one warp or one copy
// (no extra GPU work), and the steady delay is the same as before. Looking a few vblanks
// ahead, a display about to run out of ready content (a late game frame) slows down evenly
// instead of freezing, and the catch-up pays that back.
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
#define KFG_SCHED_SUB 8      // content sub-steps per vblank (catch-up granularity)
#define KFG_CATCH_STEP 2     // catch-up: +2/8 vblank of content per output (1.25x) ...
#define KFG_CATCH_FAST 4     // ... +4/8 (1.5x) while more than KFG_CATCH_FAR vblanks behind
#define KFG_CATCH_FAR 4

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
	int64_t trouble_k;         // last vblank with a slow-down or a repeat
	int64_t giveback_k;        // last vblank latency was given back
	int64_t fail_k;            // last vblank a give-back near wall_floor ran short
	uint32_t giveback_wait;    // calm vblanks needed before a give-back near wall_floor
	int64_t wall_floor;        // lowest steady delay reached since the cadence was set up
	// Counters for telemetry.
	uint64_t n_real, n_gen, n_repeat, n_skip, n_resync, n_degrade, n_catchup;
	uint64_t n_repeat_reason[5];
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
	s->catchup = 0;
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
	s->interval_avg16 = 0;  // the cap changed: the old cadence says nothing
	// The steady delay depends on the multiplier; rebuild it.
	s->wall_ref = -1;
	s->giveback_wait = 0;
	s->wall_floor = 0;
	s->giveback_k = INT64_MIN / 2;
	s->warming = true;
	s->catchup = 0;
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
	const uint32_t n = s->cfg.multiplier;
	uint32_t num = (uint32_t)off, den = (uint32_t)span_q;
	if (span > (int64_t)n && span > (int64_t)s->cfg.max_fill_span) {
		// Interval longer than the multiplier can fill: quantise to multiples of 1/n.
		const uint32_t j = (uint32_t)((off * (int64_t)n + span_q / 2) / span_q);
		if (j == 0 || j >= n) {
			r.reason = KFG_REPEAT_CADENCE;
			return r;
		}
		num = j;
		den = n;
	}
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

// Silk look-ahead: the largest content step (1/SUB vblank) for this output after which normal
// steps stay within what will be ready on each of the next vblanks, assuming no new source
// before the game's cadence brings one (the next vblank when it is already overdue); *spread is
// the even step that reaches the same point. INT64_MAX: no limit known (no cadence yet).
static inline int64_t kfg_sched_lookahead(const kfg_sched *s, int64_t k, int64_t base, int64_t *spread)
{
	const kfg_source_slot *newest = kfg_sched_slot(s, 0);
	*spread = INT64_MAX;
	if (!newest || s->interval_avg16 <= 0 || s->count < 2)
		return INT64_MAX;
	const int64_t Q = KFG_SCHED_SUB;
	// The analysis lag the last pairs actually had (the budget while none is known yet).
	int64_t lag = (int64_t)s->cfg.me_lag_budget;
	for (uint32_t back = 0; back < s->count && back < 4; back++) {
		const kfg_source_slot *slot = kfg_sched_slot(s, back);
		if (slot->me_ready != INT64_MAX) {
			lag = slot->me_ready - slot->latch;
			break;
		}
	}
	if (lag < 0)
		lag = 0;
	// The next source where the cap's cadence puts it (a fractional cap's own gap, not the mean).
	const int64_t gap = kfg_sched_next_gap(s);
	// From the cadence grid (a late latch snapped back onto it does not delay the next one).
	int64_t next_latch = (newest->t < newest->latch ? newest->t : newest->latch) + gap;
	if (next_latch <= k)
		next_latch = k + 1;  // overdue: the earliest it can still come
	const int64_t next_t = newest->t + gap;
	const int64_t next_ready = next_latch + lag;
	int64_t best = INT64_MAX;
	// The horizon only moves forward: visit each retained pair once, rather than
	// rescanning the entire ring for every predicted output.
	int back = (int)s->count - 1;
	int64_t r = kfg_sched_slot(s, (uint32_t)back)->t;
	back--;
	for (int64_t i = 1; i <= lag + 3; i++) {
		const int64_t j = k + i - 1;  // the vblank of the i-th output from now
		// Content showable at vblank j: up to the newest source whose pair is analysed by then.
		while (back >= 0) {
			const kfg_source_slot *slot = kfg_sched_slot(s, (uint32_t)back);
			const int64_t ready = slot->me_ready != INT64_MAX ? slot->me_ready : slot->latch + lag;
			if (ready > j)
				break;
			r = slot->t;
			back--;
		}
		const int64_t available = r == newest->t && next_ready <= j ? next_t : r;
		// This step followed by normal ones stays within what is ready ...
		const int64_t now = available * Q - base - (i - 1) * Q;
		if (now < best)
			best = now;
		// ... and the deficit spread evenly over the vblanks until then.
		const int64_t even = (available * Q - base) / i;
		if (even < *spread)
			*spread = even;
	}
	return best;
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
	if (s->content == INT64_MIN) {
		s->content = newest->t - 1; // first output shows the newest frame
		s->frac = 0;
	}

	// Too far behind (long stall, suspend): resynchronise forward.
	if (newest->t - s->content > (int64_t)s->cfg.max_delay) {
		s->content = newest->t - 1;
		s->frac = 0;
		s->catchup = 0;
		s->ahead_streak = 0;
		s->wall_ref = -1;
		s->giveback_wait = 0;
		s->wall_floor = 0;
		s->giveback_k = INT64_MIN / 2;
		s->warming = true;
		s->n_resync++;
		d.skipped = true;
	}

	const int64_t base = s->content * Q + (int64_t)s->frac;
	// What the display owes the wall clock. When it fell further behind than its established
	// cadence (it waited for a late source or analysis, or a late flip displaced one decision),
	// it catches up by running the content slightly faster for a few vblanks (silk catch-up)
	// instead of jumping to where the steady cadence is now: a brief speed-up, not a skip.
	if (s->wall_ref >= 0 && !s->warming) {
		const int64_t behind = (k - s->wall_ref) * Q - (base + Q);
		if (behind > s->catchup)
			s->catchup = behind;
	}
	int64_t extra = 0;
	if (s->catchup > 0) {
		extra = s->catchup > (int64_t)KFG_CATCH_FAR * Q ? KFG_CATCH_FAST : KFG_CATCH_STEP;
		if (extra > s->catchup)
			extra = s->catchup;
	}
	int64_t step = Q + extra;
	// Silk slow-down: look a few vblanks ahead at the content that will be ready to show (the
	// pairs analysed by then, at the lag the last pairs had, and the next source where the
	// game's own cadence puts it, or on the next vblank when it is already overdue). A catch-up
	// takes only the room this leaves; when the display would run into a freeze on the way, it
	// spreads the shortfall evenly (quarter speed at the most) and the catch-up pays the slower
	// vblanks back once the source is there.
	int64_t spread;
	bool slowed = false;
	const int64_t limit = kfg_sched_lookahead(s, k, base, &spread);
	// The next source is overdue on the cap's cadence: a slow-down now is the game's, not a
	// give-back that went too far.
	const bool overdue = newest && (newest->t < newest->latch ? newest->t : newest->latch) + kfg_sched_next_gap(s) <= k;
	if (!s->warming && limit < step) {
		const int64_t want = limit >= Q ? limit : spread;
		step = want < Q / 4 ? Q / 4 : want;
		if (step > Q + extra)
			step = Q + extra;
		extra = step > Q ? step - Q : 0;
		if (step < Q) {
			d.catchup = true;
			s->n_catchup++;
			slowed = true;
		}
	}
	int64_t p = base + step;
	kfg_candidate cand = kfg_sched_eval_pos(s, p, k);
	if (!cand.ok && step != Q) {
		// The chosen position is not ready: the normal step, the catch-up waits.
		extra = 0;
		p = base + Q;
		cand = kfg_sched_eval_pos(s, p, k);
	}

	if (!cand.ok && !s->warming &&
	    (cand.reason == KFG_REPEAT_SOURCE_LATE || cand.reason == KFG_REPEAT_ME_LATE)) {
		// A prediction may be optimistic. Use a remaining half-step when it is
		// actually ready, instead of freezing with valid content still available.
		const int64_t slow = base + Q / 2;
		kfg_candidate tail = kfg_sched_eval_pos(s, slow, k);
		if (tail.ok) {
			p = slow;
			cand = tail;
			extra = 0;
			slowed = true;
			if (!d.catchup) {
				d.catchup = true;
				s->n_catchup++;
			}
		}
	}

	// Latency give-back: after a calm stretch in which one more vblank of content was also
	// ready, the display owes one vblank less; it is taken back by the catch-up above. The
	// steady delay stays a whole number of vblanks, so real frames keep landing on vblanks.
	if (cand.ok) {
		kfg_candidate next = kfg_sched_eval_pos(s, p + Q, k);
		if (next.ok && s->catchup <= 0)
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
		if (s->ahead_streak >= streak && k - s->trouble_k >= wait && s->wall_ref > 0) {
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

	if (!cand.ok && s->wall_ref >= 0 && (cand.reason == KFG_REPEAT_SOURCE_LATE || cand.reason == KFG_REPEAT_ME_LATE) &&
	    k - s->realign_k <= 2 * (int64_t)s->cfg.multiplier + 2) {
		// The last catch-up was too aggressive for current conditions: back off one vblank.
		if (s->wall_ref < (int64_t)s->cfg.max_delay)
			s->wall_ref++;
		s->catchup = 0;
		s->realign_k = INT64_MIN / 2;
	}

	const int64_t pc = kfg_sched_vbl(p);
	if (!cand.ok || slowed)
		s->trouble_k = k;
	if (!cand.ok) {
		d.show = KFG_SHOW_REPEAT;
		d.reason = cand.reason;
		s->n_repeat++;
		s->n_repeat_reason[cand.reason]++;
	} else {
		if (extra > 0) {
			s->catchup -= extra;
			s->realign_k = k;
			d.catchup = true;
			s->n_catchup++;
		}
		s->content = pc;
		s->frac = (uint32_t)(p - pc * Q);
		if (cand.show == KFG_SHOW_GEN && cand.src_b == s->last_gen_pair && cand.num == s->last_gen_phase_num &&
		    cand.den == s->last_gen_phase_den) {
			// Same quantised phase as the previous output (cadence mismatch): count as a repeat
			// but let content advance so the display does not fall behind.
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
	if (s->catchup < 0)
		s->catchup = 0;
	if (cand.ok && !s->warming) {
		const int64_t now = k - s->content;
		if (s->wall_ref < 0) {
			s->wall_ref = now;
			s->wall_floor = now;
		} else if (slowed && now > s->wall_ref) {
			// A slow-down is not paid back: the display keeps the delay it settled at, so
			// it does not speed up again right away only to run short at the next late
			// latch. The give-back above takes it back after a calm stretch.
			if (s->wall_ref <= s->wall_floor + 1 &&
			    k - s->giveback_k <= (overdue ? (now - s->wall_ref <= 1 ? 270 : -1) : 4 * (int64_t)s->cfg.multiplier + 4)) {
				// A give-back near the lowest delay ran short: right after it the cadence
				// needed more, or within 3 s a game frame came up to a vblank late, which the
				// vblank given back would have absorbed (a long stall would not have been).
				// Try it again later each time (up to every 30 s at 90 Hz).
				s->giveback_wait = s->giveback_wait < 1350u ? 2u * s->giveback_wait : 2700u;
				s->fail_k = k;
			}
			s->wall_ref = now;
			s->catchup = 0;
		}
	}
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
