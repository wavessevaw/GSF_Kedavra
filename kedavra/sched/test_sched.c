// SPDX-License-Identifier: GPL-3.0-or-later
// Deterministic and randomized invariant tests for kfg_sched.h.
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "kfg_sched.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(uint32_t n) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs % n); }

// Locked 30 -> 90: after warm-up the output is exactly real, 1/3, 2/3 per source frame.
static void test_locked_x3(void)
{
	kfg_sched_config cfg = kfg_sched_default_config(3);
	kfg_sched s;
	kfg_sched_init(&s, &cfg);
	uint64_t id = 0;
	int pattern_ok = 0, total = 0;
	int64_t prev_content = INT64_MIN;
	for (int64_t k = 0; k < 900; k++) {
		if (k % 3 == 0) {
			kfg_sched_on_latch(&s, ++id, k);
			kfg_sched_on_me(&s, id, k + 1, true, false);
		}
		kfg_decision d = kfg_sched_decide(&s, k);
		if (d.show == KFG_SHOW_REAL || d.show == KFG_SHOW_GEN) {
			CHECK(d.pos > prev_content, "content not increasing at %" PRId64, k);
			prev_content = d.pos;
		}
		if (k >= 30) {
			total++;
			const int64_t ph = (k % 3);
			// With one vblank of analysis lag the steady delay is 3 vblanks: real at k%3==0.
			if (ph == 0 && d.show == KFG_SHOW_REAL)
				pattern_ok++;
			else if (ph == 1 && d.show == KFG_SHOW_GEN && d.phase_num == 1 && d.phase_den == 3)
				pattern_ok++;
			else if (ph == 2 && d.show == KFG_SHOW_GEN && d.phase_num == 2 && d.phase_den == 3)
				pattern_ok++;
			CHECK(d.delay <= 3, "delay %" PRId64 " too large at %" PRId64, d.delay, k);
		}
	}
	CHECK(pattern_ok == total, "x3 pattern %d/%d", pattern_ok, total);
	CHECK(s.n_skip == 0, "unexpected skips %" PRIu64, s.n_skip);
}

// Locked 45 -> 90 x2.
static void test_locked_x2(void)
{
	kfg_sched_config cfg = kfg_sched_default_config(2);
	kfg_sched s;
	kfg_sched_init(&s, &cfg);
	uint64_t id = 0;
	int gens = 0, reals = 0, reps = 0;
	for (int64_t k = 0; k < 900; k++) {
		if (k % 2 == 0) {
			kfg_sched_on_latch(&s, ++id, k);
			kfg_sched_on_me(&s, id, k + 1, true, false);
		}
		kfg_decision d = kfg_sched_decide(&s, k);
		if (k < 20)
			continue;
		if (d.show == KFG_SHOW_GEN) {
			gens++;
			CHECK(d.phase_num == 1 && d.phase_den == 2, "x2 phase %u/%u", d.phase_num, d.phase_den);
		} else if (d.show == KFG_SHOW_REAL) {
			reals++;
		} else {
			reps++;
		}
	}
	CHECK(reps == 0 && gens == reals, "x2 gens=%d reals=%d repeats=%d", gens, reals, reps);
}

// Base cap that does not divide the refresh (40 at 90 Hz): the limiter releases frames on
// the vblanks where cap/90 steps (intervals 2 and 3). Every vblank must show a new real or
// generated frame and content must advance by exactly one vblank per output. 24 at 90 Hz is
// x3.75 (intervals 3 and 4, multiplier 4), the lowest cap Auto may use (kfg_auto_floor).
static void test_fractional_cap(void)
{
	static const struct { int64_t cap; uint32_t mult; } caps[] = { { 40, 3 }, { 35, 3 }, { 24, 4 } };
	for (size_t c = 0; c < sizeof caps / sizeof caps[0]; c++) {
		const int64_t cap = caps[c].cap;
		kfg_sched_config cfg = kfg_sched_default_config(caps[c].mult);
		kfg_sched s;
		kfg_sched_init(&s, &cfg);
		uint64_t id = 0;
		int bad = 0, total = 0;
		int64_t prev = INT64_MIN;
		for (int64_t k = 0; k < 1800; k++) {
			if (k == 0 || (k * cap) / 90 != ((k - 1) * cap) / 90) {
				kfg_sched_on_latch(&s, ++id, k);
				kfg_sched_on_me(&s, id, k + 2, true, false);
			}
			kfg_decision d = kfg_sched_decide(&s, k);
			if (k < 60)
				continue;
			total++;
			if (d.show == KFG_SHOW_REPEAT || (prev != INT64_MIN && d.content != prev + 1))
				bad++;
			prev = d.content;
		}
		CHECK(bad == 0, "%d fps cap at 90 Hz: %d of %d outputs repeat or skip", (int)cap, bad, total);
	}
}

// GSF 1.4.2: with the cap's cadence known, a fractional cap whose frames now and then latch a
// vblank late must not keep changing the display's speed (Deck 1.4.1: x2.25 juddered).
static void test_fractional_cadence(void)
{
	static const struct { int64_t cap, hz; uint32_t mult; } caps[] = {
		{ 40, 90, 3 }, { 35, 90, 3 }, { 24, 90, 4 }, { 25, 60, 3 }, { 45, 90, 2 } };
	for (size_t c = 0; c < sizeof caps / sizeof caps[0]; c++) {
		const int64_t cap = caps[c].cap, hz = caps[c].hz;
		kfg_sched_config cfg = kfg_sched_default_config(caps[c].mult);
		kfg_sched s;
		kfg_sched_init(&s, &cfg);
		kfg_sched_set_cadence(&s, (uint32_t)hz, (uint32_t)cap);
		uint64_t id = 0;
		int changed = 0, total = 0;
		int64_t shown = INT64_MIN, late = -1;
		for (int64_t k = 0; k < 60 * hz; k++) {
			const bool release = k == 0 || (k * cap) / hz != ((k - 1) * cap) / hz;
			if (release && (id + 1) % 47 == 0 && late < 0) {
				late = k + 1;  // this frame comes one vblank after its release
			} else if (release || k == late) {
				kfg_sched_on_latch(&s, ++id, k);
				kfg_sched_on_me(&s, id, k + 2, true, false);
				if (k == late)
					late = -1;
			}
			kfg_decision d = kfg_sched_decide(&s, k);
			const int64_t here = (d.show == KFG_SHOW_REAL || d.show == KFG_SHOW_GEN) ? d.pos : shown;
			if (k > 2 * hz && shown != INT64_MIN) {
				total++;
				const int64_t st = here - shown;
				if (st < KFG_SCHED_SUB * 8 / 10 || st > KFG_SCHED_SUB * 12 / 10)
					changed++;
			}
			shown = here;
		}
		CHECK(changed * 100 <= total * 2, "%d fps at %d Hz: speed changed on %d of %d outputs", (int)cap, (int)hz,
		      changed, total);
	}
}

// A late source must never be displayed out of order, and latency returns after the stall.
static void test_late_source_recovery(void)
{
	kfg_sched_config cfg = kfg_sched_default_config(3);
	kfg_sched s;
	kfg_sched_init(&s, &cfg);
	uint64_t id = 0;
	int64_t next = 0, delay_before = -1, delay_after = -1;
	for (int64_t k = 0; k < 600; k++) {
		if (k == next) {
			kfg_sched_on_latch(&s, ++id, k);
			kfg_sched_on_me(&s, id, k + 1, true, false);
			next = k + (id == 50 ? 6 : 3); // one frame twice as long
		}
		kfg_decision d = kfg_sched_decide(&s, k);
		if (k == 141)
			delay_before = d.delay;
		if (k == 550)  // the calm stretch after the stall gives the extra vblank back
			delay_after = d.delay;
	}
	CHECK(delay_after <= delay_before, "delay before %" PRId64 " after %" PRId64, delay_before, delay_after);
	CHECK(s.n_repeat_reason[KFG_REPEAT_SOURCE_LATE] <= 4, "source-late repeats %" PRIu64, s.n_repeat_reason[KFG_REPEAT_SOURCE_LATE]);
}

// Silk pacing (GSF 1.4): a late source and the latency give-back never move the picture by a
// whole extra vblank at once. Since 1.4.2 a slow-down is not paid back with a speed-up: the
// display keeps at most one vblank more delay while late frames keep coming, instead of
// changing speed at each of them.
static void test_silk_catchup(void)
{
	for (uint32_t mult = 2; mult <= 3; mult++) {
		kfg_sched_config cfg = kfg_sched_default_config(mult);
		kfg_sched s;
		kfg_sched_init(&s, &cfg);
		uint64_t id = 0;
		int64_t next = 0, shown = INT64_MIN, delay_before = -1, delay_after = -1;
		int jumps = 0, freezes = 0;
		for (int64_t k = 0; k < 900; k++) {
			if (k == next) {
				kfg_sched_on_latch(&s, ++id, k);
				kfg_sched_on_me(&s, id, k + 2, true, false);
				// Every 60th frame is one vblank late, the next one back on time.
				next = k + mult + (id % 60 == 0 ? 1 : 0) - (id % 60 == 1 ? 1 : 0);
			}
			kfg_decision d = kfg_sched_decide(&s, k);
			int64_t here = (d.show == KFG_SHOW_REAL || d.show == KFG_SHOW_GEN) ? d.pos : shown;
			if (k > 60 && shown != INT64_MIN) {
				if (here - shown > 7 * KFG_SCHED_SUB / 4)
					jumps++;
				if (here - shown < KFG_SCHED_SUB / 4)
					freezes++;
			}
			shown = here;
			if (k == 170)
				delay_before = d.delay;
			if (k == 800)
				delay_after = d.delay;
		}
		CHECK(jumps == 0, "x%u: %d jumps", mult, jumps);
		CHECK(freezes <= 2, "x%u: %d freezes", mult, freezes);
		CHECK(delay_after <= delay_before + 1, "x%u: delay %" PRId64 " -> %" PRId64, mult, delay_before, delay_after);
		CHECK(s.n_catchup > 0, "x%u: no catch-up", mult);
	}
}

// Analysis that never keeps up must suspend generation (passthrough), then retry.
static void test_degrade(void)
{
	kfg_sched_config cfg = kfg_sched_default_config(3);
	kfg_sched s;
	kfg_sched_init(&s, &cfg);
	uint64_t id = 0;
	bool degraded = false, passthrough_real = false;
	for (int64_t k = 0; k < 600; k++) {
		if (k % 3 == 0) {
			kfg_sched_on_latch(&s, ++id, k);
			kfg_sched_on_me(&s, id, k + 7, true, false); // analysis takes > 2 source frames
		}
		kfg_decision d = kfg_sched_decide(&s, k);
		if (d.state == KFG_STATE_DEGRADED) {
			degraded = true;
			if (d.show == KFG_SHOW_REAL)
				passthrough_real = true;
		}
	}
	CHECK(degraded, "never degraded");
	CHECK(passthrough_real, "degraded state shows no frames");
	CHECK(s.n_degrade >= 2, "no retry after recovery period (%" PRIu64 ")", s.n_degrade);
}

// Scene cut: never blend across it.
static void test_cut(void)
{
	kfg_sched_config cfg = kfg_sched_default_config(3);
	kfg_sched s;
	kfg_sched_init(&s, &cfg);
	uint64_t id = 0;
	for (int64_t k = 0; k < 120; k++) {
		if (k % 3 == 0) {
			kfg_sched_on_latch(&s, ++id, k);
			kfg_sched_on_me(&s, id, k + 1, true, id == 20);
		}
		kfg_decision d = kfg_sched_decide(&s, k);
		CHECK(!(d.show == KFG_SHOW_GEN && d.src_b == 20), "generated across a cut");
	}
}

// Three refreshes remain before the next pair. Spend the last source vblank
// over all three instead of two half-steps followed by a frozen output.
static void test_short_runway(void)
{
	kfg_sched_config cfg = kfg_sched_default_config(3);
	kfg_sched s;
	kfg_sched_init(&s, &cfg);
	kfg_sched_on_latch(&s, 1, 0);
	kfg_sched_on_me(&s, 1, 1, true, false);
	kfg_sched_on_latch(&s, 2, 3);
	kfg_sched_on_me(&s, 2, 4, true, false);
	s.state = KFG_STATE_ACTIVE;
	s.warming = false;
	s.content = 2;
	int64_t previous = 2 * KFG_SCHED_SUB;
	for (int64_t k = 4; k <= 6; k++) {
		kfg_decision d = kfg_sched_decide(&s, k);
		CHECK(d.show == KFG_SHOW_GEN || d.show == KFG_SHOW_REAL, "short runway froze at %" PRId64, k);
		CHECK(d.pos - previous >= KFG_SCHED_SUB / 4, "runway step too small");
		previous = d.pos;
	}
	CHECK(previous <= 3 * KFG_SCHED_SUB, "runway exceeded the ready source");
}

// Pending analysis can miss its predicted completion. The exact real endpoint
// is still usable even though the normal step would enter the unfinished pair.
static void test_ready_tail(void)
{
	kfg_sched_config cfg = kfg_sched_default_config(3);
	kfg_sched s;
	kfg_sched_init(&s, &cfg);
	kfg_sched_on_latch(&s, 1, 0);
	kfg_sched_on_me(&s, 1, 1, true, false);
	kfg_sched_on_latch(&s, 2, 3);
	kfg_sched_on_me(&s, 2, 4, true, false);
	kfg_sched_on_latch(&s, 3, 6); // deliberately pending
	s.state = KFG_STATE_ACTIVE;
	s.warming = false;
	s.content = 2;
	s.frac = KFG_SCHED_SUB / 2;
	kfg_decision d = kfg_sched_decide(&s, 8);
	CHECK(d.show == KFG_SHOW_REAL && d.src_a == 2, "ready endpoint wasted while analysis is pending");
	CHECK(d.pos == 3 * KFG_SCHED_SUB, "tail step must end exactly at the ready source");
	d = kfg_sched_decide(&s, 9);
	CHECK(d.show == KFG_SHOW_REPEAT, "must not enter a pair without completed analysis");
}

// Randomized: arbitrary latch spacing, analysis delay, failures, cuts, multiplier changes,
// toggles, resets and missed flips. Invariants:
//  * content of displayed frames strictly increases (no reordering, no repeats counted as new),
//  * GEN only between two consecutively latched sources with ready analysis and no cut,
//  * phases strictly inside (0, 1),
//  * delay never exceeds max_delay.
static void test_fuzz(void)
{
	for (int run = 0; run < 400; run++) {
		uint32_t mult = 1 + rnd(4);
		kfg_sched_config cfg = kfg_sched_default_config(mult);
		cfg.catchup_streak = 1 + rnd(200);
		kfg_sched s;
		kfg_sched_init(&s, &cfg);
		uint64_t id = 0;
		int64_t next = rnd(4);
		int64_t prev_content = INT64_MIN;
		int64_t me_ready[4096] = {0};
		bool cut[4096] = {0}, ok[4096] = {0};
		int64_t latch[4096] = {0};
		for (int64_t k = 0; k < 3000; k++) {
			if (k == next && id < 4000) {
				id++;
				kfg_sched_on_latch(&s, id, k);
				latch[id] = kfg_sched_slot(&s, 0)->t; // content-timeline position
				int64_t lag = 1 + rnd(rnd(10) == 0 ? 12 : 3);
				me_ready[id] = k + lag;
				ok[id] = rnd(50) != 0;
				cut[id] = rnd(80) == 0;
				kfg_sched_on_me(&s, id, me_ready[id], ok[id], cut[id]);
				next = k + 1 + rnd(rnd(8) == 0 ? 12 : 4);
			}
			switch (rnd(400)) {
			case 0: kfg_sched_set_multiplier(&s, 1 + rnd(4)); break;
			case 1: kfg_sched_set_multiplier(&s, 1); break;
			case 2: k++; break; // a late flip displaced one decision
			case 3:
				kfg_sched_reset_history(&s);
				break;
			default: break;
			}
			kfg_decision d = kfg_sched_decide(&s, k);
			if (d.show == KFG_SHOW_REAL || d.show == KFG_SHOW_GEN) {
				CHECK(d.pos > prev_content, "run %d k %" PRId64 ": content %" PRId64 " after %" PRId64, run, k, d.pos, prev_content);
				prev_content = d.pos;
			}
			if (d.show == KFG_SHOW_GEN) {
				CHECK(d.src_b == d.src_a + 1, "run %d: gen across non-consecutive %" PRIu64 "->%" PRIu64, run, d.src_a, d.src_b);
				CHECK(me_ready[d.src_b] <= k && ok[d.src_b] && !cut[d.src_b], "run %d: gen without valid analysis", run);
				CHECK(d.phase_num > 0 && d.phase_num < d.phase_den, "run %d: phase %u/%u", run, d.phase_num, d.phase_den);
				CHECK(d.pos > latch[d.src_a] * KFG_SCHED_SUB && d.pos < latch[d.src_b] * KFG_SCHED_SUB, "run %d: gen content outside its pair", run);
				CHECK((int64_t)d.phase_num * (latch[d.src_b] - latch[d.src_a]) * KFG_SCHED_SUB == (d.pos - latch[d.src_a] * KFG_SCHED_SUB) * (int64_t)d.phase_den
				      || (latch[d.src_b] - latch[d.src_a]) > (int64_t)s.cfg.max_fill_span, "run %d: phase does not match the content position", run);
			}
			if (d.show == KFG_SHOW_REAL && d.state == KFG_STATE_ACTIVE && d.content < latch[d.src_a])
				CHECK(cut[d.src_a] || !ok[d.src_a], "run %d: real shown early without a cut", run);
			CHECK(d.show == KFG_SHOW_NONE || d.delay <= (int64_t)cfg.max_delay + 16, "run %d: delay %" PRId64, run, d.delay);
		}
	}
}

int main(void)
{
	test_locked_x3();
	test_locked_x2();
	test_fractional_cap();
	test_fractional_cadence();
	test_late_source_recovery();
	test_silk_catchup();
	test_degrade();
	test_cut();
	test_short_runway();
	test_ready_tail();
	test_fuzz();
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("kfg_sched: all tests passed\n");
	return 0;
}
