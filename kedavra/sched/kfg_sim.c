// SPDX-License-Identifier: GPL-3.0-or-later
//
// Discrete-event model of the compositor loop that drives kfg_sched.h.
//
// It models what the scheduler cannot control: a fixed-refresh vblank grid,
// compositor wakeups a redzone before each vblank, a game rendered under the
// compositor's FIFO FPS limiter with random frame times and spikes, motion
// analysis started when a source is latched, and per-output composition time.
// The output is one CSV row per display refresh, in the same format the
// patched compositor writes from real KMS page-flip events, so the same
// analyzer (kfg_pacing.py) evaluates both.
//
// This is a model, not a measurement: it validates the scheduling policy under
// stated assumptions. Hardware acceptance needs traces from the Steam Deck.
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kfg_sched.h"

typedef struct rng { uint64_t s; } rng;
static uint64_t rng_u64(rng *r)
{
	// splitmix64
	uint64_t z = (r->s += 0x9e3779b97f4a7c15ull);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}
static double rng_unit(rng *r) { return (double)(rng_u64(r) >> 11) * (1.0 / 9007199254740992.0); }
static double rng_normal(rng *r)
{
	double u1 = rng_unit(r), u2 = rng_unit(r);
	if (u1 < 1e-300)
		u1 = 1e-300;
	return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}
// Log-normal with given median and 99th percentile.
static double rng_lognormal(rng *r, double median, double p99)
{
	if (median <= 0)
		return 0;
	double sigma = p99 > median ? log(p99 / median) / 2.326347874 : 0.0;
	return median * exp(sigma * rng_normal(r));
}

#define MAX_EVENTS 64
typedef struct event {
	double t_s;
	char what[16];
	double v;
} event;

typedef struct params {
	double refresh_hz;
	uint32_t mult;
	double cap_fps;          // compositor FPS limiter (0 = none)
	double game_ms;          // median render time
	double game_p99_ms;
	double spike_prob;       // per-frame probability of a long frame
	double spike_ms;
	double me_ms, me_p99_ms; // motion analysis per pair
	double warp_ms;          // per generated output, added to composition
	double compose_ms, compose_p99_ms;
	double redzone_ms;       // compositor wakes this long before vblank
	double fg_slowdown;      // fractional slowdown of the game while generation runs
	double vblank_jitter_us;
	double cut_prob;         // per-source probability of a scene cut
	double me_min_vblanks;   // analysis usable no earlier than this many vblanks after the latch
	                         // (2: the analysis split over two submissions, GSF 1.4.1)
	double duration_s;
	uint64_t seed;
	event ev[MAX_EVENTS];
	int nev;
	const char *trace_path;
	const char *source_path;
} params;

static void usage(void)
{
	fprintf(stderr,
		"kfg_sim [--refresh HZ] [--mult N] [--cap FPS] [--game-ms MS] [--game-p99-ms MS]\n"
		"        [--spike-prob P] [--spike-ms MS] [--me-ms MS] [--me-p99-ms MS] [--me-min-vblanks N] [--warp-ms MS]\n"
		"        [--compose-ms MS] [--compose-p99-ms MS] [--redzone-ms MS] [--fg-slowdown F]\n"
		"        [--cut-prob P] [--duration S] [--seed N] [--event T:what:value]... --trace out.csv [--sources out.csv]\n"
		"events: cap, mult, off, on, refresh, gap (ms of no game output), me_scale, game_ms\n");
}

static int parse(int argc, char **argv, params *p)
{
	*p = (params){
		.refresh_hz = 90, .mult = 3, .cap_fps = 30, .game_ms = 20, .game_p99_ms = 26,
		.spike_prob = 0.002, .spike_ms = 45, .me_ms = 2.0, .me_p99_ms = 3.5, .warp_ms = 0.45,
		.compose_ms = 0.55, .compose_p99_ms = 0.9, .redzone_ms = 1.65, .me_min_vblanks = 1, .fg_slowdown = 0.08,
		.vblank_jitter_us = 5, .duration_s = 60, .seed = 1,
	};
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define NUM(flag, field) if (!strcmp(a, flag) && v) { p->field = strtod(v, NULL); i++; continue; }
		NUM("--refresh", refresh_hz) NUM("--cap", cap_fps) NUM("--game-ms", game_ms)
		NUM("--game-p99-ms", game_p99_ms) NUM("--spike-prob", spike_prob) NUM("--spike-ms", spike_ms)
		NUM("--me-ms", me_ms) NUM("--me-p99-ms", me_p99_ms) NUM("--warp-ms", warp_ms)
		NUM("--compose-ms", compose_ms) NUM("--compose-p99-ms", compose_p99_ms)
		NUM("--redzone-ms", redzone_ms) NUM("--fg-slowdown", fg_slowdown)
		NUM("--vblank-jitter-us", vblank_jitter_us) NUM("--duration", duration_s) NUM("--cut-prob", cut_prob) NUM("--me-min-vblanks", me_min_vblanks)
#undef NUM
		if (!strcmp(a, "--mult") && v) { p->mult = (uint32_t)atoi(v); i++; continue; }
		if (!strcmp(a, "--seed") && v) { p->seed = strtoull(v, NULL, 10); i++; continue; }
		if (!strcmp(a, "--trace") && v) { p->trace_path = v; i++; continue; }
		if (!strcmp(a, "--sources") && v) { p->source_path = v; i++; continue; }
		if (!strcmp(a, "--event") && v && p->nev < MAX_EVENTS) {
			event *e = &p->ev[p->nev];
			char what[16] = {0};
			double t = 0, val = 0;
			if (sscanf(v, "%lf:%15[a-z_]:%lf", &t, what, &val) < 2) {
				fprintf(stderr, "bad event %s\n", v);
				return -1;
			}
			e->t_s = t;
			snprintf(e->what, sizeof(e->what), "%s", what);
			e->v = val;
			p->nev++;
			i++;
			continue;
		}
		usage();
		return -1;
	}
	if (!p->trace_path) {
		usage();
		return -1;
	}
	return 0;
}

typedef struct game {
	uint64_t next_id;
	double start_ns;      // render start of the frame in flight
	double done_ns;       // GPU completion of the frame in flight
	bool in_flight;
	bool queued_done;     // frame done but not latched yet
	uint64_t queued_id;
	double queued_done_ns;
	double resume_ns;     // no output before this (suspend / gap)
} game;

int main(int argc, char **argv)
{
	params p;
	if (parse(argc, argv, &p))
		return 2;
	FILE *trace = fopen(p.trace_path, "w");
	if (!trace) {
		perror(p.trace_path);
		return 1;
	}
	FILE *src = p.source_path ? fopen(p.source_path, "w") : NULL;
	fprintf(trace, "vblank,flip_ns,kind,src_a,src_b,phase_num,phase_den,content,delay,reason,state,refresh_ns,mult,compose_late,skipped,pos\n");
	if (src)
		fprintf(src, "id,done_ns,latch_vblank,latch_ns\n");

	rng r = { p.seed * 0x2545F4914F6CDD1Dull + 7 };
	double refresh_ns = 1e9 / p.refresh_hz;
	double me_scale = 1.0;
	kfg_sched_config cfg = kfg_sched_default_config(p.mult);
	kfg_sched s;
	kfg_sched_init(&s, &cfg);

	game g = { .next_id = 1 };
	double grid_origin = 0;     // vblank index 0 of the current refresh grid
	int64_t grid_base = 0;      // vblank index offset for the current grid
	int64_t k = 0;
	int64_t last_latch = INT64_MIN / 2;
	double t_end = p.duration_s * 1e9;
	bool fg_on = p.mult > 1;
	uint32_t mult = p.mult;
	bool pending_late = false;  // composition of previous decision overran its vblank
	kfg_decision late_dec;
	memset(&late_dec, 0, sizeof(late_dec));
	int next_event = 0;

	for (;;) {
		double vbl_ns = grid_origin + (double)(k - grid_base) * refresh_ns + p.vblank_jitter_us * 1e3 * rng_normal(&r) * 0.2;
		if (vbl_ns > t_end)
			break;
		double wake_ns = vbl_ns - p.redzone_ms * 1e6;

		// Apply scenario events whose time has come.
		while (next_event < p.nev && p.ev[next_event].t_s * 1e9 <= wake_ns) {
			event *e = &p.ev[next_event++];
			if (!strcmp(e->what, "cap")) p.cap_fps = e->v;
			else if (!strcmp(e->what, "mult")) { mult = (uint32_t)e->v; kfg_sched_set_multiplier(&s, mult); fg_on = mult > 1; }
			else if (!strcmp(e->what, "off")) { kfg_sched_set_multiplier(&s, 1); fg_on = false; }
			else if (!strcmp(e->what, "on")) { kfg_sched_set_multiplier(&s, mult > 1 ? mult : 2); fg_on = true; }
			else if (!strcmp(e->what, "refresh")) {
				// New grid: vblank indices continue, history is discarded.
				grid_origin = vbl_ns;
				grid_base = k;
				p.refresh_hz = e->v;
				refresh_ns = 1e9 / p.refresh_hz;
				kfg_sched_reset_history(&s);
				last_latch = INT64_MIN / 2;
			}
			else if (!strcmp(e->what, "gap")) g.resume_ns = wake_ns + e->v * 1e6;
			else if (!strcmp(e->what, "me_scale")) me_scale = e->v;
			else if (!strcmp(e->what, "game_ms")) { p.game_p99_ms = p.game_ms > 0 ? e->v * p.game_p99_ms / p.game_ms : e->v; p.game_ms = e->v; }
		}

		// Game: the first frame starts at the first wakeup; later frames start when the
		// previous one is latched (the FIFO limiter blocks the game's present).
		if (!g.in_flight && !g.queued_done) {
			double x = rng_lognormal(&r, p.game_ms, p.game_p99_ms);
			g.start_ns = wake_ns > g.resume_ns ? wake_ns : g.resume_ns;
			g.done_ns = g.start_ns + x * 1e6;
			g.in_flight = true;
		}
		if (g.in_flight && g.done_ns <= wake_ns) {
			g.in_flight = false;
			g.queued_done = true;
			g.queued_id = g.next_id++;
			g.queued_done_ns = g.done_ns;
		}

		// The compositor tells the scheduler the cap's cadence every vblank.
		kfg_sched_set_cadence(&s, (uint32_t)llround(p.refresh_hz), p.cap_fps > 0 ? (uint32_t)llround(p.cap_fps) : 0u);

		// Compositor wake for vblank k: latch (FIFO limiter), analysis, decision.
		// Gamescope's limiter: a whole divisor of the refresh releases every n-th vblank; a cap
		// that does not divide it (40 at 90 Hz) releases on the vblanks where cap / refresh
		// steps (2, 2, 2, 3, ...), as the KEDAVRA hook in steamcompmgr does.
		bool released = true;
		if (p.cap_fps > 0 && p.cap_fps < p.refresh_hz) {
			const int64_t T = llround(p.cap_fps), R = llround(p.refresh_hz);
			if (R % T == 0)
				released = k >= last_latch + R / T;
			else
				released = last_latch < 0 || (k * T) / R != (last_latch * T) / R;
		}
		if (g.queued_done && released) {
			kfg_sched_on_latch(&s, g.queued_id, k);
			last_latch = k;
			if (src)
				fprintf(src, "%" PRIu64 ",%.0f,%" PRId64 ",%.0f\n", g.queued_id, g.queued_done_ns, k, wake_ns);
			// Motion analysis starts now on the compositor's realtime compute queue.
			double me = rng_lognormal(&r, p.me_ms * me_scale, p.me_p99_ms * me_scale) * 1e6;
			double ready_ns = wake_ns + me;
			// First decision (wake) at or after ready_ns.
			int64_t ready_k = k + (p.me_min_vblanks > 1 ? llround(p.me_min_vblanks) : 1);
			while (grid_origin + (double)(ready_k - grid_base) * refresh_ns - p.redzone_ms * 1e6 < ready_ns)
				ready_k++;
			kfg_sched_on_me(&s, g.queued_id, ready_k, true, rng_unit(&r) < p.cut_prob);
			g.queued_done = false;
			// The game may start its next frame right away (one frame queued).
			double x = rng_lognormal(&r, p.game_ms, p.game_p99_ms);
			if (rng_unit(&r) < p.spike_prob)
				x += p.spike_ms;
			if (fg_on)
				x *= 1.0 + p.fg_slowdown;
			double start = wake_ns > g.resume_ns ? wake_ns : g.resume_ns;
			g.start_ns = start;
			g.done_ns = start + x * 1e6;
			g.in_flight = true;
		}

		kfg_decision d;
		bool late_now = false;
		if (pending_late) {
			// The previous composition missed vblank k-1 and lands on this vblank instead;
			// the compositor does not paint a new frame for this wakeup.
			d = late_dec;
			pending_late = false;
		} else {
			d = kfg_sched_decide(&s, k);
			double c = rng_lognormal(&r, p.compose_ms, p.compose_p99_ms) * 1e6;
			if (d.show == KFG_SHOW_GEN)
				c += p.warp_ms * 1e6;
			if (d.show != KFG_SHOW_REPEAT && wake_ns + c > vbl_ns) {
				late_now = true;
				pending_late = true;
				late_dec = d;
			}
		}
		if (late_now) {
			fprintf(trace, "%" PRId64 ",%.0f,repeat,0,0,0,0,%" PRId64 ",%" PRId64 ",compose_late,%s,%.0f,%u,1,0,%.3f\n",
				k, vbl_ns, d.content, d.delay, kfg_state_name(d.state), refresh_ns, mult, (double)d.pos / KFG_SCHED_SUB);
		} else if (d.show != KFG_SHOW_NONE) {
			fprintf(trace, "%" PRId64 ",%.0f,%s,%" PRIu64 ",%" PRIu64 ",%u,%u,%" PRId64 ",%" PRId64 ",%s,%s,%.0f,%u,0,%d,%.3f\n",
				k, vbl_ns, kfg_show_name(d.show), d.src_a, d.src_b, d.phase_num, d.phase_den, d.content, d.delay,
				kfg_reason_name(d.reason), kfg_state_name(d.state), refresh_ns, mult, d.skipped ? 1 : 0, (double)d.pos / KFG_SCHED_SUB);
		}
		k++;
	}
	fclose(trace);
	if (src)
		fclose(src);
	fprintf(stderr, "real=%" PRIu64 " gen=%" PRIu64 " repeat=%" PRIu64 " skip=%" PRIu64 " resync=%" PRIu64 " degrade=%" PRIu64 " catchup=%" PRIu64 "\n",
		s.n_real, s.n_gen, s.n_repeat, s.n_skip, s.n_resync, s.n_degrade, s.n_catchup);
	return 0;
}
