# SPDX-License-Identifier: GPL-3.0-or-later
# KEDAVRA: scheduler model, interpolation engine and tools.
#
#   make            build everything into build/
#   make test       scheduler unit tests, model stress matrix, engine correctness (needs a Vulkan ICD)
#   make bench      engine GPU timing (run on the Steam Deck for real numbers)

BUILD ?= build
CC ?= cc
CFLAGS ?= -O2 -g
WARN = -Wall -Wextra -Wno-missing-field-initializers
GLSLANG ?= glslangValidator
PYTHON ?= python3

SHADERS = kfg_copy kfg_reduce kfg_me kfg_stats kfg_select kfg_warp kfg_post kfg_smaa_edge kfg_smaa_weight kfg_smaa_blend
SPV = $(SHADERS:%=$(BUILD)/%.spv)

all: $(BUILD)/kfg_sim $(BUILD)/kfg_replay $(BUILD)/test_sched $(BUILD)/test_auto $(BUILD)/kfg_vktest $(BUILD)/kfg_posttest $(SPV)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.spv: shaders/%.comp shaders/kfg_common.glsl shaders/kfg_smaa.glsl shaders/smaa/SMAA.hlsl | $(BUILD)
	$(GLSLANG) -V --target-env vulkan1.1 -Ishaders -o $@ $<

$(BUILD)/kfg_sim: sched/kfg_sim.c sched/kfg_sched.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -std=c11 -o $@ sched/kfg_sim.c -lm

$(BUILD)/kfg_replay: sched/kfg_replay.c sched/kfg_sched.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -std=c11 -o $@ sched/kfg_replay.c

$(BUILD)/test_sched: sched/test_sched.c sched/kfg_sched.h | $(BUILD)
	$(CC) -O1 -g -fsanitize=address,undefined $(WARN) -std=c11 -o $@ sched/test_sched.c

$(BUILD)/test_auto: sched/test_auto.c sched/kfg_auto.h sched/kfg_sched.h | $(BUILD)
	$(CC) -O1 -g -fsanitize=address,undefined $(WARN) -std=c11 -o $@ sched/test_auto.c -lm

$(BUILD)/kfg_vktest: engine/kfg_vktest.c engine/kfg_vk.c engine/kfg_vk.h engine/kfg_plan.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -std=c11 -Iengine -o $@ engine/kfg_vktest.c engine/kfg_vk.c -lvulkan -lm

$(BUILD)/kfg_posttest: engine/kfg_posttest.c engine/kfg_vk.c engine/kfg_vk.h engine/kfg_plan.h engine/kfg_post.h engine/kfg_smaa_tex.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -std=c11 -Iengine -o $@ engine/kfg_posttest.c engine/kfg_vk.c -lvulkan -lm

$(BUILD)/test_completion_bridge: sched/test_completion_bridge.c sched/kfg_sched.h | $(BUILD)
	$(CC) -O1 -g -fsanitize=address,undefined $(WARN) -std=c11 -o $@ sched/test_completion_bridge.c

test-pacing:
	$(PYTHON) -m unittest discover -s tools -p 'test_pacing.py'

test-sched: $(BUILD)/test_sched $(BUILD)/test_auto $(BUILD)/kfg_sim $(BUILD)/kfg_replay $(BUILD)/test_completion_bridge
	$(BUILD)/test_completion_bridge
	$(BUILD)/test_sched
	$(BUILD)/test_auto
	$(PYTHON) sched/run_scenarios.py --sim $(BUILD)/kfg_sim --seeds 3 --out $(BUILD)/scenarios.json
	# Replay smoke test: an 89 ms game frame every 10 s (Deck run B) is paid back, not skipped.
	$(PYTHON) sched/kfg_replay.py hitch -o $(BUILD)/hitch_events.csv --me-lag 2
	$(PYTHON) sched/kfg_replay.py compare --bin new=$(BUILD)/kfg_replay --json $(BUILD)/hitch_replay.json $(BUILD)/hitch_events.csv
	$(PYTHON) -c "import json,sys; r=json.load(open('$(BUILD)/hitch_replay.json'))['results']['hitch_events.csv']['new']; sys.exit(0 if r['resync_per_min']==0 and r['jump_per_min']==0 and r['real_shown']>0.99 else 1)"

test-engine: $(BUILD)/kfg_vktest $(BUILD)/kfg_posttest $(SPV)
	$(BUILD)/kfg_vktest --spv $(BUILD) --json $(BUILD)/engine.json
	# Base 24 at 90 Hz (x3.75): motion x1.25 per source frame, phases of 1/15.
	$(BUILD)/kfg_vktest --spv $(BUILD) --lowbase --json $(BUILD)/engine-lowbase.json
	$(BUILD)/kfg_posttest --spv $(BUILD)

test: test-pacing test-sched test-engine

bench: $(BUILD)/kfg_vktest $(BUILD)/kfg_posttest $(SPV)
	$(BUILD)/kfg_vktest --spv $(BUILD) --json $(BUILD)/engine-bench.json --bench 200
	$(BUILD)/kfg_posttest --spv $(BUILD) --bench 50

clean:
	rm -rf $(BUILD)

.PHONY: all test test-pacing test-sched test-engine bench clean
