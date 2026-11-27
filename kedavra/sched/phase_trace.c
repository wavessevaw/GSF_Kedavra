// SPDX-License-Identifier: GPL-3.0-or-later
// Deterministic source-to-display replay. Positions are measured in display periods.
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "kfg_sched.h"

int main(int argc, char **argv)
{
	if (argc != 4)
		return 2;
	const int hz = atoi(argv[1]), scenario = atoi(argv[3]);
	const double fps = atof(argv[2]);
	if ((hz != 60 && hz != 90) || !(fps >= 20 && fps <= 40))
		return 2;
	kfg_sched_config cfg = kfg_sched_default_config(hz / 30);
	cfg.prefer_real_90 = hz == 90;
	cfg.prefer_real_60 = hz == 60;
	kfg_sched sched;
	kfg_sched_init(&sched, &cfg);
	uint64_t id = 0;
	double time = 0, positions[KFG_SCHED_RING] = {0}, shown = 0;
	for (int k = 0; k < 180 * hz; ++k) {
		if (k + 1e-7 >= time) {
			positions[++id % KFG_SCHED_RING] = time;
			kfg_sched_on_latch(&sched, id, k);
			bool stalled = scenario == 2 && id % 61 == 0;
			kfg_sched_on_me(&sched, id, k + (stalled ? 6 : 2), true, false);
			double rate = scenario == 1 ? 30 + 2 * sin(time / hz * 0.63) : fps;
			time += hz / rate + (stalled ? 4 : 0);
		}
		kfg_decision d = kfg_sched_decide(&sched, k);
		double a = positions[d.src_a % KFG_SCHED_RING];
		if (d.show == KFG_SHOW_REAL)
			shown = a;
		else if (d.show == KFG_SHOW_GEN)
			shown = a + (positions[d.src_b % KFG_SCHED_RING] - a) * d.phase_num / d.phase_den;
		if (k > 4 * hz)
			printf("%d %.8f %d %u %u %" PRIu64 " %" PRIu64 "\n",
			       k, shown, d.show, d.phase_num, d.phase_den, d.src_a, d.src_b);
	}
	return 0;
}
