// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline replay of recorded source timing through the scheduler header.
//
// Input (CSV with header): one row per compositor decision
//
//   vblank,latch_id,me_ready,mult,cap_fps,refresh_hz[,decide]
//
// vblank: decision vblank index (strictly increasing; gaps are paints the compositor did not
// make). decide 0: a source latched on a vblank without a decision (the compositor re-showed
// a late composition), default 1. latch_id: source frame latched at this vblank (0: none). me_ready: first vblank its
// analysed pair may be used (ignored without a latch). mult, cap_fps, refresh_hz: what the
// compositor told the scheduler at this vblank (cap 0: the whole multiplier's cadence).
//
// Latch and analysis timing do not depend on the scheduler's decisions (the game and the
// limiter release frames, the analysis runs after each latch), so the same input can be
// played through any scheduler version: build this file against that version's kfg_sched.h
// (-I its directory). -DREPLAY_NO_CADENCE for headers before 1.4.2, -DREPLAY_NO_POS before
// 1.4.0.
//
// Output: the compositor's trace format (kedavra/tools/kfg_pacing.py, kfg_replay.py), one row
// per decision, flip time = vblank * refresh. This replays decisions, not the display: late
// compositions and missed flips of the recording are not reproduced.
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kfg_sched.h"

#ifndef KFG_SCHED_SUB
#define KFG_SCHED_SUB 1
#endif

int main(int argc, char **argv)
{
	const char *in = NULL, *out = NULL;
	int me_lag = -1, cap_override = -1, strict = 0, mult_override = -1;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--events") && i + 1 < argc)
			in = argv[++i];
		else if (!strcmp(argv[i], "--trace") && i + 1 < argc)
			out = argv[++i];
		else if (!strcmp(argv[i], "--me-lag") && i + 1 < argc)
			me_lag = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--cap") && i + 1 < argc)
			cap_override = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mult") && i + 1 < argc)
			mult_override = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--strict") && i + 1 < argc)
			strict = atoi(argv[++i]);
		else {
			fprintf(stderr, "kfg_replay --events in.csv --trace out.csv [--me-lag N] [--cap FPS] [--mult N] [--strict 0|1|2|3]\n"
				"  --mult N: replace the recorded multiplier (where it is above 1)\n"
				"  --strict M: strict multiplier mode (0 off, 1 stretch, 2 hold; headers before GSF 1.5: -DREPLAY_NO_STRICT)\n"
				"  --me-lag N: me_ready = latch + N instead of the recorded value\n"
				"  --cap FPS: base cap passed to the scheduler instead of the recorded one\n");
			return 2;
		}
	}
	if (!in || !out) {
		fprintf(stderr, "kfg_replay --events in.csv --trace out.csv [--me-lag N] [--cap FPS]\n");
		return 2;
	}
	FILE *fi = fopen(in, "r");
	if (!fi) {
		perror(in);
		return 1;
	}
	FILE *fo = fopen(out, "w");
	if (!fo) {
		perror(out);
		fclose(fi);
		return 1;
	}
	fprintf(fo, "vblank,flip_ns,kind,src_a,src_b,phase_num,phase_den,content,delay,reason,state,refresh_ns,mult,compose_late,skipped,pos,latch_id,latch_vblank,me_ready,cap_fps,decisions\n");
	char line[512];
	if (!fgets(line, sizeof(line), fi)) {
		fclose(fi);
		fclose(fo);
		return 1;
	}
	kfg_sched s;
	bool init = false;
	uint32_t mult = 0, refresh = 0;
	int64_t last_k = INT64_MIN;
	uint64_t rows = 0;
	while (fgets(line, sizeof(line), fi)) {
		int64_t k = 0, me = 0;
		uint64_t id = 0;
		unsigned m = 0, cap = 0, hz = 0;
		int decide = 1;
		const int got = sscanf(line, "%" SCNd64 ",%" SCNu64 ",%" SCNd64 ",%u,%u,%u,%d", &k, &id, &me, &m, &cap, &hz, &decide);
		if (got < 6)
			continue;
		if (got < 7)
			decide = 1;
		if (k <= last_k)
			continue;
		last_k = k;
		if (!hz)
			hz = 90;
		if (cap_override >= 0)
			cap = (unsigned)cap_override;
		if (mult_override > 1 && m > 1)
			m = (unsigned)mult_override;
		if (!init) {
			if (m <= 1)
				continue;
			kfg_sched_config cfg = kfg_sched_default_config(m);
#ifndef REPLAY_NO_STRICT
			cfg.strict = (uint32_t)strict;
#else
			(void)strict;
#endif
			kfg_sched_init(&s, &cfg);
			init = true;
			mult = m;
			refresh = hz;
		}
		if (m != mult) {
			kfg_sched_set_multiplier(&s, m);
			mult = m;
		}
		if (hz != refresh) {
			kfg_sched_reset_history(&s);
			refresh = hz;
		}
#ifndef REPLAY_NO_CADENCE
		kfg_sched_set_cadence(&s, hz, cap);
#endif
		if (id) {
			if (me_lag >= 0)
				me = k + me_lag;
			if (kfg_sched_on_latch(&s, id, k))
				kfg_sched_on_me(&s, id, me, true, false);
		}
		if (!decide)
			continue;
		kfg_decision d = kfg_sched_decide(&s, k);
		if (d.show == KFG_SHOW_NONE)
			continue;
		const double refresh_ns = 1e9 / (double)hz;
#ifdef REPLAY_NO_POS
		const double pos = (double)d.content;
#else
		const double pos = (double)d.pos / KFG_SCHED_SUB;
#endif
		fprintf(fo, "%" PRId64 ",%.0f,%s,%" PRIu64 ",%" PRIu64 ",%u,%u,%" PRId64 ",%" PRId64 ",%s,%s,%.0f,%u,0,%d,%.3f,%" PRIu64 ",%" PRId64 ",%" PRId64 ",%u,1\n",
			k, (double)k * refresh_ns, kfg_show_name(d.show), d.src_a, d.src_b, d.phase_num, d.phase_den, d.content, d.delay,
			kfg_reason_name(d.reason), kfg_state_name(d.state), refresh_ns, mult, d.skipped ? 1 : 0, pos,
			id, id ? k : (int64_t)0, id ? me : (int64_t)0, cap);
		rows++;
	}
	fclose(fi);
	fclose(fo);
	if (init)
		fprintf(stderr, "rows=%" PRIu64 " real=%" PRIu64 " gen=%" PRIu64 " repeat=%" PRIu64 " skip=%" PRIu64 " resync=%" PRIu64 "\n",
			rows, s.n_real, s.n_gen, s.n_repeat, s.n_skip, s.n_resync);
	return 0;
}
