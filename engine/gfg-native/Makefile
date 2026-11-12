CC ?= gcc
.DEFAULT_GOAL := layer
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -fPIC
BUILD ?= build
VULKAN_HEADERS ?= /usr/include
ENABLE_TE ?= 0
ENABLE_CPU_BRIDGE ?= 0
TE_LIB ?= ../gfg-temporal/build/libgfg-temporal.a
ifeq ($(ENABLE_TE),1)
NATIVE_SOURCES := src/layer.c src/gpu_bridge.c src/output_worker.c
NATIVE_HEADERS := src/refresh_observation.h src/queue_observation.h src/cpu_bridge.h src/gpu_bridge.h src/output_worker.h src/frame_clock.h src/timeline_watch.h src/calibrated_clock.h src/generation_budget.h src/wsi_queries.inc src/present_ids.h src/present_chain.h src/past_timing.h src/memory_policy.h src/swapchain_chain.h ../gfg-temporal/src/te_engine.h
NATIVE_CPPFLAGS := -DGFG_NATIVE_WITH_TE -I../gfg-temporal/src
ifeq ($(ENABLE_CPU_BRIDGE),1)
NATIVE_SOURCES += src/cpu_bridge.c
NATIVE_CPPFLAGS += -DGFG_NATIVE_WITH_CPU_BRIDGE
endif
NATIVE_LDLIBS := $(TE_LIB) -ldl -lm
NATIVE_DEPS := $(TE_LIB)
else
NATIVE_SOURCES := src/layer.c
NATIVE_HEADERS := src/queue_observation.h src/swapchain_chain.h
NATIVE_CPPFLAGS :=
NATIVE_LDLIBS :=
NATIVE_DEPS :=
endif

.PHONY: layer abi smoke wsi-smoke wsi-smoke-te wsi-smoke-gpu swapchain-policy memory-policy present-ids present-chain gpu-ring output-worker clean
$(BUILD)/frame-clock: tests/frame_clock.c src/frame_clock.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/frame_clock.c

frame-clock: $(BUILD)/frame-clock
	$(BUILD)/frame-clock tests/fixtures/ac-origins-deadline-slack.csv
.PHONY: frame-clock

$(BUILD)/queue-observation: tests/queue_observation.c src/queue_observation.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(VULKAN_HEADERS) -o $@ $<

queue-observation: $(BUILD)/queue-observation
	$(BUILD)/queue-observation
.PHONY: queue-observation

# Test-only VkImage source allocations; NOT wired into live swapchain create.
$(BUILD)/offscreen-source-pool: tests/offscreen_source_pool.c src/offscreen_source_pool.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(VULKAN_HEADERS) -o $@ tests/offscreen_source_pool.c

offscreen-source-pool: $(BUILD)/offscreen-source-pool
	$(BUILD)/offscreen-source-pool
.PHONY: offscreen-source-pool

# Exercise the actual test-only native vkCreateSwapchainKHR entrypoint, which
# allocates offscreen GPU source images and one private downstream WSI.
$(BUILD)/output-proxy-layer-create: tests/output_proxy_layer_create.c src/layer.c src/output_proxy_bootstrap.h src/offscreen_source_pool.h src/virtual_wsi_facade.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-function -g -fno-omit-frame-pointer -fsanitize=address,undefined -pthread -I$(VULKAN_HEADERS) -o $@ tests/output_proxy_layer_create.c -lvulkan

output-proxy-layer-create: $(BUILD)/output-proxy-layer-create
	$(BUILD)/output-proxy-layer-create
.PHONY: output-proxy-layer-create

# Real Vulkan driver plus TEST-registered actual native layer dispatch.
# Source images and the single output WSI are GPU-backed; the Vulkan
# loader is NOT installing the layer for an external game.
$(BUILD)/owned-wsi-real-layer: tests/wsi_gpu_observer.c src/layer.c src/output_proxy_bootstrap.h src/offscreen_source_pool.h src/async_capture_retire.h tests/owned_source_transport.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-function -pthread -DGFG_NATIVE_TEST_SMALL -DGFG_NATIVE_TEST_ASYNC -DGFG_NATIVE_TEST_OWNED_WSI -DGFG_NATIVE_TEST_LAYER_PROXY_REAL -DVK_USE_PLATFORM_XCB_KHR -I$(VULKAN_HEADERS) -o $@ tests/wsi_gpu_observer.c -lvulkan -lxcb -ldl

# Isolated Vulkan loader-installed PROBE. The native layer is compiled
# with test WSI hooks but the normal installation and stable release are
# untouched. Only a create-only virtual app swapchain is supported.
$(BUILD)/loader-probe/libVkLayer_gfg_native.so: src/layer.c src/output_proxy_bootstrap.h src/offscreen_source_pool.h src/virtual_wsi_facade.h
	mkdir -p $(BUILD)/loader-probe
	$(CC) $(CFLAGS) -Wno-unused-function -fvisibility=hidden -pthread -DGFG_NATIVE_TEST_OUTPUT_OWNER_HOOKS -I$(VULKAN_HEADERS) -shared -Wl,-z,defs -o $@ src/layer.c -ldl -lvulkan

$(BUILD)/loader-probe/VkLayer_gfg_native.json: VkLayer_gfg_native.json
	mkdir -p $(BUILD)/loader-probe
	cp $< $@

$(BUILD)/loader-proxy-create: tests/loader_proxy_create.c src/offscreen_source_pool.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DVK_USE_PLATFORM_XCB_KHR -I$(VULKAN_HEADERS) -o $@ $< -lvulkan -lxcb

loader-proxy-create: $(BUILD)/loader-proxy-create $(BUILD)/loader-probe/libVkLayer_gfg_native.so $(BUILD)/loader-probe/VkLayer_gfg_native.json
	GFG_NATIVE_FG=1 GFG_NATIVE_TEST_LOADER_CREATE_PROBE=1 GFG_NATIVE_TEST_PROXY_CREATE=1 \
	  VK_LAYER_PATH="$(abspath $(BUILD)/loader-probe):/usr/share/vulkan/explicit_layer.d" \
	  VK_INSTANCE_LAYERS=VK_LAYER_GFG_native $(BUILD)/loader-proxy-create
.PHONY: loader-proxy-create

# Loader-installed live GPU proof: application's normal Acquire/Render/Present
# is intercepted by the isolated layer, worker presents through one real WSI.
$(BUILD)/loader-live-output: tests/loader_live_output.c tests/x11_output_probe.h src/offscreen_source_pool.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DVK_USE_PLATFORM_XCB_KHR -I$(VULKAN_HEADERS) -o $@ tests/loader_live_output.c -lvulkan -lxcb

# Convert game swapchain PRESENT layout requests without touching the
# true compositor swapchain, including Vulkan sync2 and renderpasses.
$(BUILD)/virtual-layouts: tests/virtual_layouts.c src/virtual_layouts.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(VULKAN_HEADERS) -o $@ tests/virtual_layouts.c

virtual-layouts: $(BUILD)/virtual-layouts
	$(BUILD)/virtual-layouts
.PHONY: virtual-layouts

# The actual native layer's GIPA/GDPA interception routes controlled
# virtual GetSwapchainImages/Acquire/Present (not direct facade calls).
# Built only with the explicit TEST macro; production layer unaffected.
$(BUILD)/output-layer-dispatch: tests/output_layer_dispatch.c src/layer.c src/virtual_wsi_facade.h src/virtual_source.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-function -pthread -I$(VULKAN_HEADERS) -o $@ tests/output_layer_dispatch.c -lvulkan

output-layer-dispatch: $(BUILD)/output-layer-dispatch
	$(BUILD)/output-layer-dispatch
.PHONY: output-layer-dispatch

# WSI-shaped virtual app source. No loader interception or real Deck install.
$(BUILD)/virtual-wsi-facade: tests/virtual_wsi_facade.c src/virtual_wsi_facade.h src/virtual_source.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -pthread -I$(VULKAN_HEADERS) -o $@ tests/virtual_wsi_facade.c

virtual-wsi-facade: $(BUILD)/virtual-wsi-facade
	$(BUILD)/virtual-wsi-facade
.PHONY: virtual-wsi-facade

# Virtual app swapchain source ownership (FREE -> ACQUIRED -> COPYING -> FREE)
# is a hard precondition to returning an application image after snapshotting.
$(BUILD)/virtual-source: tests/virtual_source.c src/virtual_source.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/virtual_source.c

virtual-source: $(BUILD)/virtual-source
	$(BUILD)/virtual-source
.PHONY: virtual-source

# Transport-neutral proof that an exclusively owned output surface can
# schedule one real/interpolated stream without dropping real frames.
# This is NOT a Steam Deck scanout or Vulkan swapchain test.
$(BUILD)/output-owner: tests/output_owner.c src/output_owner.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/output_owner.c

output-owner: $(BUILD)/output-owner
	$(BUILD)/output-owner
.PHONY: output-owner

layer: $(BUILD)/libVkLayer_gfg_native.so $(BUILD)/VkLayer_gfg_native.json

$(BUILD)/libVkLayer_gfg_native.so: $(NATIVE_SOURCES) $(NATIVE_HEADERS) $(NATIVE_DEPS)
	@test -f $(VULKAN_HEADERS)/vulkan/vk_layer.h || { echo "Vulkan headers required at $(VULKAN_HEADERS)"; exit 1; }
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fvisibility=hidden -I$(VULKAN_HEADERS) -shared -Wl,-z,defs $(NATIVE_CPPFLAGS) -o $@ $(NATIVE_SOURCES) $(NATIVE_LDLIBS) -lpthread

$(BUILD)/VkLayer_gfg_native.json: VkLayer_gfg_native.json
	mkdir -p $(BUILD)
	cp $< $@

$(BUILD)/loader-smoke: tests/loader_smoke.c
	@test -f $(VULKAN_HEADERS)/vulkan/vulkan.h || { echo "Vulkan headers required"; exit 1; }
	mkdir -p $(BUILD)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -I$(VULKAN_HEADERS) -o $@ $< -lvulkan

smoke: layer $(BUILD)/loader-smoke
	VK_INSTANCE_LAYERS=VK_LAYER_GFG_native GFG_NATIVE_FG=1 GFG_NATIVE_TRACE=1 \
	VK_LAYER_PATH="$(abspath $(BUILD))" $(BUILD)/loader-smoke

$(BUILD)/memory-policy: tests/memory_policy.c src/memory_policy.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(VULKAN_HEADERS) -o $@ tests/memory_policy.c

memory-policy: $(BUILD)/memory-policy
	$(BUILD)/memory-policy

$(BUILD)/gpu-wsi-observer: tests/wsi_gpu_observer.c tests/gpu_observer.h src/gpu_bridge.c src/gpu_bridge.h src/present_chain.h $(TE_LIB)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DVK_USE_PLATFORM_XCB_KHR -DGFG_NATIVE_TEST_SMALL -DGFG_NATIVE_TEST_GPU_OBSERVER -I$(VULKAN_HEADERS) -Isrc -I../gfg-temporal/src -o $@ tests/wsi_gpu_observer.c src/gpu_bridge.c $(TE_LIB) -lvulkan -lxcb -ldl -lpthread -lm

$(BUILD)/borrowed-device: tests/borrowed_device.c $(TE_LIB)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(VULKAN_HEADERS) -I../gfg-temporal/src -o $@ $< $(TE_LIB) -lvulkan -ldl -lpthread -lm

$(BUILD)/present-ids: tests/present_ids.c src/present_ids.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/present_ids.c

present-ids: $(BUILD)/present-ids
	$(BUILD)/present-ids

$(BUILD)/present-chain: tests/present_chain.c src/present_chain.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(VULKAN_HEADERS) -o $@ tests/present_chain.c

present-chain: $(BUILD)/present-chain
	$(BUILD)/present-chain

$(BUILD)/gpu-ring: tests/gpu_ring.c src/gpu_bridge.c src/gpu_bridge.h src/frame_clock.h src/present_chain.h src/calibrated_clock.h src/generation_budget.h $(TE_LIB)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(VULKAN_HEADERS) -I../gfg-temporal/src -o $@ tests/gpu_ring.c $(TE_LIB) -ldl -lpthread -lm

gpu-ring: $(BUILD)/gpu-ring
	$(BUILD)/gpu-ring

$(BUILD)/output-worker: tests/output_worker.c src/output_worker.c src/output_worker.h src/present_chain.h src/gpu_bridge.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(VULKAN_HEADERS) -o $@ tests/output_worker.c -lpthread

output-worker: $(BUILD)/output-worker
	$(BUILD)/output-worker

$(BUILD)/queue-serialization: tests/queue_serialization.c src/layer.c src/queue_observation.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-function -I$(VULKAN_HEADERS) -o $@ tests/queue_serialization.c -lpthread

queue-serialization: $(BUILD)/queue-serialization
	$(BUILD)/queue-serialization

# Test-only layer: inspects actual async output pixels before lower WSI.
# Keep it in its own layer directory; it must never enter the shipped package.
$(BUILD)/observer-layer/libVkLayer_gfg_native.so: $(NATIVE_SOURCES) $(NATIVE_HEADERS) tests/gpu_observer.h $(TE_LIB)
	mkdir -p $(BUILD)/observer-layer
	$(CC) $(CFLAGS) -fvisibility=hidden -I$(VULKAN_HEADERS) -Isrc -DGFG_NATIVE_WITH_TE -DGFG_NATIVE_TEST_OBSERVE_LAYER -I../gfg-temporal/src -shared -Wl,-z,defs -o $@ $(NATIVE_SOURCES) $(TE_LIB) -lvulkan -ldl -lm -lpthread
	cp VkLayer_gfg_native.json $(BUILD)/observer-layer/

$(BUILD)/async-observer-app: tests/wsi_gpu_observer.c
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DGFG_NATIVE_TEST_SMALL -DGFG_NATIVE_TEST_ASYNC -DVK_USE_PLATFORM_XCB_KHR -I$(VULKAN_HEADERS) -o $@ $< -lvulkan -lxcb

$(BUILD)/swapchain-policy: tests/swapchain_policy.c src/layer.c
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-function -I$(VULKAN_HEADERS) -o $@ tests/swapchain_policy.c -lpthread

swapchain-policy: $(BUILD)/swapchain-policy
	$(BUILD)/swapchain-policy

$(BUILD)/wsi-smoke: tests/wsi_smoke.c
	@test -f $(VULKAN_HEADERS)/vulkan/vulkan_xcb.h || { echo "Vulkan XCB headers required"; exit 1; }
	mkdir -p $(BUILD)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -DVK_USE_PLATFORM_XCB_KHR $(WSI_EXTRA_CFLAGS) -I$(VULKAN_HEADERS) -o $@ $< -lvulkan -lxcb

wsi-smoke: layer $(BUILD)/wsi-smoke
	GFG_NATIVE_FG=1 GFG_NATIVE_TRACE=1 VK_INSTANCE_LAYERS=VK_LAYER_GFG_native \
	VK_LAYER_PATH="$(abspath $(BUILD))" $(BUILD)/wsi-smoke

wsi-smoke-te:
	$(MAKE) ENABLE_TE=1 ENABLE_CPU_BRIDGE=1 BUILD=build-cpu layer
	$(MAKE) ENABLE_TE=1 ENABLE_CPU_BRIDGE=1 BUILD=build-cpu WSI_EXTRA_CFLAGS=-DGFG_NATIVE_TEST_SMALL build-cpu/wsi-smoke
	GFG_NATIVE_FG=1 GFG_NATIVE_CPU_BRIDGE=1 GFG_NATIVE_BUDGET_MS=5000 GFG_NATIVE_MAX_FRAME_MS=10000 GFG_NATIVE_ALGO=tiles GFG_NATIVE_LMC_TIER=2 GFG_LMC_ORPHAN_CAP=0 GFG_NATIVE_TRACE=1 \
	VK_INSTANCE_LAYERS=VK_LAYER_GFG_native VK_LAYER_PATH="$(abspath build-cpu)" \
	build-cpu/wsi-smoke

# Test-only one-surface Vulkan WSI path: input frames are not independently
# sent to WSI. Real + synthetic tickets go to one final output queue.
# Validates transport in lavapipe, NOT physical OLED scanout.
$(BUILD)/owned-wsi-smoke: tests/wsi_gpu_observer.c src/output_owner.h src/virtual_source.h tests/owned_source_transport.h src/virtual_layouts.h src/virtual_wsi_facade.h
	@test -f $(VULKAN_HEADERS)/vulkan/vulkan_xcb.h || { echo "Vulkan XCB headers required"; exit 1; }
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -pthread -DGFG_NATIVE_TEST_SMALL -DGFG_NATIVE_TEST_ASYNC -DGFG_NATIVE_TEST_OWNED_WSI -DVK_USE_PLATFORM_XCB_KHR -I$(VULKAN_HEADERS) -o $@ $< -lvulkan -lxcb -ldl

owned-wsi-smoke: $(BUILD)/owned-wsi-smoke
	$(BUILD)/owned-wsi-smoke
.PHONY: owned-wsi-smoke

$(BUILD)/wsi-gpu-smoke: tests/wsi_gpu_smoke.c tests/render_readiness_gate.h
	@test -f $(VULKAN_HEADERS)/vulkan/vulkan_xcb.h || { echo "Vulkan XCB headers required"; exit 1; }
	mkdir -p $(BUILD)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -DVK_USE_PLATFORM_XCB_KHR -I$(VULKAN_HEADERS) -o $@ $< -lvulkan -lxcb -ldl -pthread

# GPU-resident bridge on a real XCB swapchain (lavapipe under Xvfb in CI).
# Run 1: validation layer below ours (+ sync validation) and the test-only
#        GFG_NATIVE_GPU_BRIDGE_VERIFY distinctness readback.
# Run 1b: test-only fault injection of both skip paths (TE busy, ring busy)
#        under validation: binary-semaphore invariants on every skip path.
# Run 2: no validation/verify, layer CPU-time measurement (GFG_NATIVE_TIMING).
# The skipped run-1 validation is reported, never silently passed.
GPU_SMOKE_FRAMES ?= 24
VALIDATION_LAYER_DIR ?= /usr/share/vulkan/explicit_layer.d
GPU_SMOKE_ENV := GFG_NATIVE_FG=1 GFG_NATIVE_GPU_BRIDGE=1 GFG_NATIVE_TRACE=1 GFG_NATIVE_ALGO=tiles \
	GFG_TEST_EXPECT_GPU=1 GFG_TEST_FRAMES=$(GPU_SMOKE_FRAMES)
wsi-smoke-gpu:
	$(MAKE) ENABLE_TE=1 BUILD=build-te layer
	$(MAKE) ENABLE_TE=1 BUILD=build-te build-te/wsi-gpu-smoke
	@if [ -f "$(VALIDATION_LAYER_DIR)/VkLayer_khronos_validation.json" ]; then \
	  echo "wsi-smoke-gpu: run 1 with VK_LAYER_KHRONOS_validation"; \
	  $(GPU_SMOKE_ENV) GFG_NATIVE_GPU_BRIDGE_VERIFY=1 GFG_NATIVE_GPU_BRIDGE_VERIFY_EVERY=4 \
	  GFG_TEST_VALIDATION=1 GFG_TEST_CHAIN_VK12=1 GFG_TEST_READONLY_FEATURES=1 \
	  VK_INSTANCE_LAYERS=VK_LAYER_GFG_native:VK_LAYER_KHRONOS_validation \
	  VK_LAYER_PATH="$(abspath build-te):$(VALIDATION_LAYER_DIR)" build-te/wsi-gpu-smoke && \
	  echo "wsi-smoke-gpu: run 1b skip-path fault injection with VK_LAYER_KHRONOS_validation" && \
	  $(GPU_SMOKE_ENV) GFG_NATIVE_GPU_BRIDGE_VERIFY=1 GFG_NATIVE_GPU_BRIDGE_VERIFY_EVERY=3 \
	  GFG_NATIVE_GPU_BRIDGE_INJECT_SKIP=5 GFG_TEST_ALLOW_SKIPS=1 GFG_TEST_FRAMES=30 GFG_TEST_VALIDATION=1 \
	  VK_INSTANCE_LAYERS=VK_LAYER_GFG_native:VK_LAYER_KHRONOS_validation \
	  VK_LAYER_PATH="$(abspath build-te):$(VALIDATION_LAYER_DIR)" build-te/wsi-gpu-smoke; \
	else \
	  echo "wsi-smoke-gpu: SKIP validation (VK_LAYER_KHRONOS_validation not installed)"; \
	  $(GPU_SMOKE_ENV) GFG_NATIVE_GPU_BRIDGE_VERIFY=1 GFG_NATIVE_GPU_BRIDGE_VERIFY_EVERY=4 \
	  GFG_TEST_CHAIN_VK12=1 GFG_TEST_READONLY_FEATURES=1 \
	  VK_INSTANCE_LAYERS=VK_LAYER_GFG_native VK_LAYER_PATH="$(abspath build-te)" build-te/wsi-gpu-smoke; \
	fi
	$(GPU_SMOKE_ENV) GFG_NATIVE_TIMING=1 \
	VK_INSTANCE_LAYERS=VK_LAYER_GFG_native VK_LAYER_PATH="$(abspath build-te)" build-te/wsi-gpu-smoke

GLIBC_MAX ?= 2.31
abi: $(BUILD)/libVkLayer_gfg_native.so
	@need=$$(objdump -T $< | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -V | tail -1); \
	echo "GFG native layer requires glibc $$need (limit $(GLIBC_MAX))"; \
	[ "$$(printf '%s\n%s\n' "$$need" "$(GLIBC_MAX)" | sort -V | tail -1)" = "$(GLIBC_MAX)" ]

clean:
	rm -rf $(BUILD)

$(BUILD)/hdr-precision: tests/hdr_precision.c $(TE_LIB)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(VULKAN_HEADERS) -I../gfg-temporal/src -o $@ $< $(TE_LIB) -lvulkan -ldl -lpthread -lm

$(BUILD)/presentation-timing: tests/presentation_timing.c src/gpu_bridge.c src/gpu_bridge.h src/frame_clock.h src/present_chain.h src/calibrated_clock.h src/generation_budget.h $(TE_LIB)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(VULKAN_HEADERS) -I../gfg-temporal/src -o $@ $< $(TE_LIB) -ldl -lm -lpthread

presentation-timing: $(BUILD)/presentation-timing
	$(BUILD)/presentation-timing
.PHONY: presentation-timing

$(BUILD)/past-timing: tests/past_timing.c src/past_timing.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(VULKAN_HEADERS) -o $@ $<

past-timing: $(BUILD)/past-timing
	$(BUILD)/past-timing
.PHONY: past-timing

$(BUILD)/device-fallback: tests/device_fallback.c src/layer.c
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-function -I$(VULKAN_HEADERS) -o $@ $< -lpthread

device-fallback: $(BUILD)/device-fallback
	$(BUILD)/device-fallback
.PHONY: device-fallback


# Diagnostic watcher: deterministic mocked Vulkan semaphore counter. No ICD.
$(BUILD)/timeline-watch: tests/timeline_watch.c src/timeline_watch.h tests/stubs/vulkan/vulkan.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -pthread -Itests/stubs -o $@ tests/timeline_watch.c

timeline-watch: $(BUILD)/timeline-watch
	$(BUILD)/timeline-watch
.PHONY: timeline-watch


# Pure fake Vulkan calibration clock tests, no driver or real swapchain.
$(BUILD)/calibration-clock: tests/calibration_clock.c src/calibrated_clock.h tests/stubs/vulkan/vulkan.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -pthread -Itests/stubs -o $@ tests/calibration_clock.c

calibration-clock: $(BUILD)/calibration-clock
	$(BUILD)/calibration-clock tests/fixtures/software-submit-overlap-20261019.csv
.PHONY: calibration-clock

$(BUILD)/generation-budget: tests/generation_budget.c src/generation_budget.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/generation_budget.c

generation-budget: $(BUILD)/generation-budget
	$(BUILD)/generation-budget tests/fixtures/deck-calibrated-slack-20261019.csv
.PHONY: generation-budget

$(BUILD)/refresh-observation: tests/refresh_observation.c src/refresh_observation.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(VULKAN_HEADERS) -o $@ $<

refresh-observation: $(BUILD)/refresh-observation
	$(BUILD)/refresh-observation
.PHONY: refresh-observation
