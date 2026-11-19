// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include "kfg_sched.h"

static void compare(int fps, bool stalls)
{
	kfg_sched_config cfg = kfg_sched_default_config(3);
	kfg_sched baseline, candidate;
	kfg_sched_init(&baseline, &cfg);
	cfg.prefer_real_90 = true;
	kfg_sched_init(&candidate, &cfg);
	int last = -1, repeats[2] = {0}, advanced = 0;
	for (int k = 0; k < 65 * 90; ++k) {
		int time = k;
		if (stalls && k % 180 >= 80 && k % 180 < 88)
			time -= k % 180 - 79;
		int id = time * fps / 90 + 1;
		if (id != last) {
			kfg_sched_on_latch(&baseline, id, k);
			kfg_sched_on_latch(&candidate, id, k);
			kfg_sched_on_me(&baseline, id, k + 2, true, false);
			kfg_sched_on_me(&candidate, id, k + 2, true, false);
			last = id;
		}
		kfg_decision a = kfg_sched_decide(&baseline, k);
		kfg_decision b = kfg_sched_decide(&candidate, k);
		if (k < 450)
			continue;
		if (fps == 30 || stalls)
			assert(a.show == b.show && a.pos == b.pos && a.src_a == b.src_a);
		repeats[0] += a.show == KFG_SHOW_REPEAT;
		repeats[1] += b.show == KFG_SHOW_REPEAT;
		advanced += b.show == KFG_SHOW_REAL && b.pos > a.pos;
	}
	assert(repeats[0] == repeats[1]);
	if (fps < 30 && !stalls)
		assert(advanced > 0);
}

int main(void)
{
	for (int fps = 27; fps <= 30; ++fps)
		compare(fps, false);
	compare(28, true);
	return 0;
}
