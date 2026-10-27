# 10. Источники и воспроизводимость

Дата обращения: 24.10.2026. Все ссылки на исследованные исходники фиксируют полный commit SHA; line anchors относятся к upstream файлам, а не к line offsets после hooks.patch. Актуальные API-страницы документации дополняются зафиксированными исходниками specification.

## Source roots

| ID | Проект / версия | Полный SHA | Область доказательства |
|---|---|---|---|
| K | GSF Kedavra 1.4.1 main при начале работы | `5f22938b55f9853f764e927c847d9c1a8d9a8c04` | Текущий runtime source, scripts, evidence |
| G | ValveSoftware/gamescope, pin из K apply.sh | `aa289ea5977d8b21b92b0f246f0e8f0c225d180f` | Реальная база hooks.patch |
| MD | Mesa, prefix из hardware bench git-d246f60ddf | `d246f60ddf53089faa1d554865f7a38a737ed388` | Source revision исторического драйвера; binary/distro patches отдельно не установлены |
| MT | Mesa tag mesa-26.0.0, контрольный snapshot | `c10cba7efaa003e11c5c832bb2347e55c619ae7a` | Сравнение изменений, не выдано за текущий Deck driver |
| L | Linux upstream v6.16 | `038d61fd642278bab63ee8ef722c50d10ab01e8f` | Reference AMDGPU/DCN/KMS implementation; не установленный kernel Deck |
| F | GFG Extreme main | `6b22036b503006f63956b8919cdaa9bf91f1a3c3` | Existing Governor/Autopilot/Split/Clock/service |
| V | Khronos Vulkan-Docs | `a8b088a3fae12c8575407ea90c7c09bd653db923` | Нормативные ограничения queues и external synchronization |

Mesa читалась через `chaotic-cx/mesa-mirror` как зеркало исходного проекта. Canonical: [Mesa GitLab, MD revision](https://gitlab.freedesktop.org/mesa/mesa/-/tree/d246f60ddf53089faa1d554865f7a38a737ed388). Прямое чтение GitLab через web tool оказалось недоступно; использовано зеркало по object SHA. Это не доказательство тождественности установленного бинарника публичному commit: нужны пакетный build ID и distro source manifest из E0.

В исследовании нет «Linux v6.16 = SteamOS kernel». В сохранённых аппаратных evidence названы SteamOS 3.8.28 (20260922.1), VANGOGH и Mesa prefix; kernel build/config отсутствуют. Поэтому capability конкретного текущего Deck в спорных случаях остаётся SUPPORTED/unknown, а не CONFIRMED. Reference kernel исследован для конкретных механизмов и ограничений; vendor extensions HDR требуют runtime property inventory.

Полный каталог полученных текстовых источников с line counts, SHA и role: [source-manifest.json](evidence/source-manifest.json). Незатронутые оригинальные evidence остаются в родительском source tree.

## K — executable pipeline и host distinction

- [kedavra/gamescope/src/kfg/kfg.cpp](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/gamescope/src/kfg/kfg.cpp#L1)
- [kedavra/gamescope/src/kfg/kfg.hpp](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/gamescope/src/kfg/kfg.hpp#L1)
- [kedavra/gamescope/hooks.patch](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/gamescope/hooks.patch#L1)
- [kedavra/gamescope/apply.sh](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/gamescope/apply.sh#L1)
- [kedavra/engine/kfg_plan.h](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/engine/kfg_plan.h#L1)
- [kedavra/engine/kfg_post.h](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/engine/kfg_post.h#L1)
- [kedavra/engine/kfg_vk.c](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/engine/kfg_vk.c#L1)
- [kedavra/sched/kfg_sched.h](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/sched/kfg_sched.h#L1)
- [kedavra/sched/kfg_auto.h](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/sched/kfg_auto.h#L1)
- [kedavra/sched/test_completion_bridge.c](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/sched/test_completion_bridge.c#L1)
- [kedavra/shaders/kfg_common.glsl](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/shaders/kfg_common.glsl#L1)
- [kedavra/shaders/kfg_copy.comp](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/shaders/kfg_copy.comp#L1)
- [kedavra/shaders/kfg_reduce.comp](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/shaders/kfg_reduce.comp#L1)
- [kedavra/shaders/kfg_me.comp](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/shaders/kfg_me.comp#L1)
- [kedavra/shaders/kfg_stats.comp](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/shaders/kfg_stats.comp#L1)
- [kedavra/shaders/kfg_select.comp](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/shaders/kfg_select.comp#L1)
- [kedavra/shaders/kfg_warp.comp](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/shaders/kfg_warp.comp#L1)
- [kedavra/shaders/kfg_post.comp](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/shaders/kfg_post.comp#L1)
- [kedavra/tools/kfg_pacing.py](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/tools/kfg_pacing.py#L1)
- [kedavra/deck/probe.sh](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/deck/probe.sh#L1)
- [kedavra/deck/bench.sh](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/deck/bench.sh#L1)
- [kedavra/deck/install.sh](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/deck/install.sh#L1)
- [kedavra/deck/rollback.sh](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/deck/rollback.sh#L1)

Дополнительно изучены SMAA wrapper shaders, post plan/quality host, tests, `kedavra/Makefile`, `kedavra.yml`, фазовые architecture/evidence документы и Deck protocols. Заголовки, README и комментарии проверялись по executable calls: cached pair selection и pre-history post расходятся с частью старых descriptions.

## G — Gamescope

- [src/rendervulkan.hpp](https://github.com/ValveSoftware/gamescope/blob/aa289ea5977d8b21b92b0f246f0e8f0c225d180f/src/rendervulkan.hpp#L1)
- [src/Backends/DRMBackend.cpp](https://github.com/ValveSoftware/gamescope/blob/aa289ea5977d8b21b92b0f246f0e8f0c225d180f/src/Backends/DRMBackend.cpp#L1)
- [src/rendervulkan.cpp](https://github.com/ValveSoftware/gamescope/blob/aa289ea5977d8b21b92b0f246f0e8f0c225d180f/src/rendervulkan.cpp#L1)
- [src/backend.h](https://github.com/ValveSoftware/gamescope/blob/aa289ea5977d8b21b92b0f246f0e8f0c225d180f/src/backend.h#L1)
- [src/steamcompmgr.cpp](https://github.com/ValveSoftware/gamescope/blob/aa289ea5977d8b21b92b0f246f0e8f0c225d180f/src/steamcompmgr.cpp#L1)
- [src/commit.h](https://github.com/ValveSoftware/gamescope/blob/aa289ea5977d8b21b92b0f246f0e8f0c225d180f/src/commit.h#L1)
- [src/Timeline.cpp](https://github.com/ValveSoftware/gamescope/blob/aa289ea5977d8b21b92b0f246f0e8f0c225d180f/src/Timeline.cpp#L1)
- [src/commit.cpp](https://github.com/ValveSoftware/gamescope/blob/aa289ea5977d8b21b92b0f246f0e8f0c225d180f/src/commit.cpp#L1)
- [src/Timeline.h](https://github.com/ValveSoftware/gamescope/blob/aa289ea5977d8b21b92b0f246f0e8f0c225d180f/src/Timeline.h#L1)
- [scripts/00-gamescope/displays/valve.steamdeck.oled.lua](https://github.com/ValveSoftware/gamescope/blob/aa289ea5977d8b21b92b0f246f0e8f0c225d180f/scripts/00-gamescope/displays/valve.steamdeck.oled.lua#L1)

Ключевые места: renderer BInit/submitInternal/wait/barrier tracking/output pool; backend Present/drm_prepare_liftoff/Commit/page_flip_handler; incoming commit readiness; Timeline import/eventfd. `DRMBackend.h` не использовался — такого пути в исследованном tree нет; declarations находятся в backend.cpp и `backend.h`.

## MD — RADV и AMD common

- [src/amd/vulkan/radv_physical_device.c](https://github.com/chaotic-cx/mesa-mirror/blob/d246f60ddf53089faa1d554865f7a38a737ed388/src/amd/vulkan/radv_physical_device.c#L1)
- [src/amd/vulkan/radv_queue.c](https://github.com/chaotic-cx/mesa-mirror/blob/d246f60ddf53089faa1d554865f7a38a737ed388/src/amd/vulkan/radv_queue.c#L1)
- [src/amd/vulkan/radv_queue.h](https://github.com/chaotic-cx/mesa-mirror/blob/d246f60ddf53089faa1d554865f7a38a737ed388/src/amd/vulkan/radv_queue.h#L1)
- [src/amd/vulkan/radv_image.c](https://github.com/chaotic-cx/mesa-mirror/blob/d246f60ddf53089faa1d554865f7a38a737ed388/src/amd/vulkan/radv_image.c#L1)
- [src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c](https://github.com/chaotic-cx/mesa-mirror/blob/d246f60ddf53089faa1d554865f7a38a737ed388/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L1)
- [src/amd/common/ac_gpu_info.c](https://github.com/chaotic-cx/mesa-mirror/blob/d246f60ddf53089faa1d554865f7a38a737ed388/src/amd/common/ac_gpu_info.c#L1)
- [src/amd/vulkan/radv_pipeline_compute.c](https://github.com/chaotic-cx/mesa-mirror/blob/d246f60ddf53089faa1d554865f7a38a737ed388/src/amd/vulkan/radv_pipeline_compute.c#L1)

Для MT просмотрены corresponding physical_device/queue/image/ac_gpu_info и winsys files. В частности transfer queue gating различается между snapshots; рекомендации не копируют defaults из moving latest docs. Generic hardware queue count выводится из kernel-reported available rings, а не из числа CU или MEC slots.

## L — AMDGPU, DCN и KMS

- [drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c](https://github.com/torvalds/linux/blob/038d61fd642278bab63ee8ef722c50d10ab01e8f/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c#L1)
- [drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c](https://github.com/torvalds/linux/blob/038d61fd642278bab63ee8ef722c50d10ab01e8f/drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c#L1)
- [drivers/gpu/drm/amd/amdgpu/amdgpu_ctx.c](https://github.com/torvalds/linux/blob/038d61fd642278bab63ee8ef722c50d10ab01e8f/drivers/gpu/drm/amd/amdgpu/amdgpu_ctx.c#L1)
- [drivers/gpu/drm/amd/amdgpu/amdgpu_cs.c](https://github.com/torvalds/linux/blob/038d61fd642278bab63ee8ef722c50d10ab01e8f/drivers/gpu/drm/amd/amdgpu/amdgpu_cs.c#L1)
- [drivers/gpu/drm/amd/amdgpu/amdgpu_job.c](https://github.com/torvalds/linux/blob/038d61fd642278bab63ee8ef722c50d10ab01e8f/drivers/gpu/drm/amd/amdgpu/amdgpu_job.c#L1)
- [drivers/gpu/drm/scheduler/sched_main.c](https://github.com/torvalds/linux/blob/038d61fd642278bab63ee8ef722c50d10ab01e8f/drivers/gpu/drm/scheduler/sched_main.c#L1)
- [drivers/gpu/drm/amd/display/dc/core/dc_resource.c](https://github.com/torvalds/linux/blob/038d61fd642278bab63ee8ef722c50d10ab01e8f/drivers/gpu/drm/amd/display/dc/core/dc_resource.c#L1)
- [drivers/gpu/drm/amd/display/dc/resource/dcn301/dcn301_resource.c](https://github.com/torvalds/linux/blob/038d61fd642278bab63ee8ef722c50d10ab01e8f/drivers/gpu/drm/amd/display/dc/resource/dcn301/dcn301_resource.c#L1)
- [drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm.c](https://github.com/torvalds/linux/blob/038d61fd642278bab63ee8ef722c50d10ab01e8f/drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm.c#L1)
- [drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm_plane.c](https://github.com/torvalds/linux/blob/038d61fd642278bab63ee8ef722c50d10ab01e8f/drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm_plane.c#L1)
- [drivers/gpu/drm/drm_atomic_uapi.c](https://github.com/torvalds/linux/blob/038d61fd642278bab63ee8ef722c50d10ab01e8f/drivers/gpu/drm/drm_atomic_uapi.c#L1)
- [drivers/gpu/drm/drm_prime.c](https://github.com/torvalds/linux/blob/038d61fd642278bab63ee8ef722c50d10ab01e8f/drivers/gpu/drm/drm_prime.c#L1)

Дополнительно: `amdgpu_trace.h`, `amdgpu_dm_trace.h`, `amdgpu_drv.c`, `include/uapi/drm/amdgpu_drm.h`, `Documentation/gpu/amdgpu/thermal.rst`, `Documentation/gpu/drm-kms.rst`. PRIME import/register framebuffer — sharing/lifetime, не обязательный pixel copy. Atomic validation проверяет конфигурацию, а не полезность её производительности.

## F — существующий power controller

- [py_modules/gfg_plugin/autopilot.py](https://github.com/wavessevaw/GFG-Extreme/blob/6b22036b503006f63956b8919cdaa9bf91f1a3c3/py_modules/gfg_plugin/autopilot.py#L1)
- [py_modules/gfg_plugin/power_split.py](https://github.com/wavessevaw/GFG-Extreme/blob/6b22036b503006f63956b8919cdaa9bf91f1a3c3/py_modules/gfg_plugin/power_split.py#L1)
- [py_modules/gfg_plugin/steamos_tdp.py](https://github.com/wavessevaw/GFG-Extreme/blob/6b22036b503006f63956b8919cdaa9bf91f1a3c3/py_modules/gfg_plugin/steamos_tdp.py#L1)
- [py_modules/gfg_plugin/governor_power.py](https://github.com/wavessevaw/GFG-Extreme/blob/6b22036b503006f63956b8919cdaa9bf91f1a3c3/py_modules/gfg_plugin/governor_power.py#L1)
- [py_modules/gfg_plugin/governor_device.py](https://github.com/wavessevaw/GFG-Extreme/blob/6b22036b503006f63956b8919cdaa9bf91f1a3c3/py_modules/gfg_plugin/governor_device.py#L1)
- [py_modules/gfg_plugin/gpu_clock_control.py](https://github.com/wavessevaw/GFG-Extreme/blob/6b22036b503006f63956b8919cdaa9bf91f1a3c3/py_modules/gfg_plugin/gpu_clock_control.py#L1)
- [py_modules/gfg_plugin/governor.py](https://github.com/wavessevaw/GFG-Extreme/blob/6b22036b503006f63956b8919cdaa9bf91f1a3c3/py_modules/gfg_plugin/governor.py#L1)
- [py_modules/gfg_plugin/gpu_clock.py](https://github.com/wavessevaw/GFG-Extreme/blob/6b22036b503006f63956b8919cdaa9bf91f1a3c3/py_modules/gfg_plugin/gpu_clock.py#L1)
- [py_modules/gfg_plugin/policy.py](https://github.com/wavessevaw/GFG-Extreme/blob/6b22036b503006f63956b8919cdaa9bf91f1a3c3/py_modules/gfg_plugin/policy.py#L1)
- [py_modules/gfg_plugin/pilot.py](https://github.com/wavessevaw/GFG-Extreme/blob/6b22036b503006f63956b8919cdaa9bf91f1a3c3/py_modules/gfg_plugin/pilot.py#L1)
- [py_modules/gfg_plugin/kedavra/ctl.py](https://github.com/wavessevaw/GFG-Extreme/blob/6b22036b503006f63956b8919cdaa9bf91f1a3c3/py_modules/gfg_plugin/kedavra/ctl.py#L1)
- [py_modules/gfg_plugin/engine_gate.py](https://github.com/wavessevaw/GFG-Extreme/blob/6b22036b503006f63956b8919cdaa9bf91f1a3c3/py_modules/gfg_plugin/engine_gate.py#L1)
- [py_modules/gfg_plugin/service.py](https://github.com/wavessevaw/GFG-Extreme/blob/6b22036b503006f63956b8919cdaa9bf91f1a3c3/py_modules/gfg_plugin/service.py#L1)

Изменения F в этой ветке отсутствуют. Source service.py подтверждает фактическое построение sample/epoch и вызовы controllers; docstrings не принимались за действующие thresholds.

## V — Vulkan specification

- [Queue create infos: uniqueness и одинаковый global priority одной family](https://github.com/KhronosGroup/Vulkan-Docs/blob/a8b088a3fae12c8575407ea90c7c09bd653db923/chapters/devsandqueues.adoc#L2436)
- [SYNC_FD copy transference и binary semaphore](https://github.com/KhronosGroup/Vulkan-Docs/blob/a8b088a3fae12c8575407ea90c7c09bd653db923/chapters/synchronization.adoc#L4012)
- [External semaphore handle transfer types](https://github.com/KhronosGroup/Vulkan-Docs/blob/a8b088a3fae12c8575407ea90c7c09bd653db923/chapters/synchronization.adoc#L5190)
- [appendices/VK_KHR_external_semaphore_fd.adoc](https://github.com/KhronosGroup/Vulkan-Docs/blob/a8b088a3fae12c8575407ea90c7c09bd653db923/appendices/VK_KHR_external_semaphore_fd.adoc#L1)
- [appendices/VK_EXT_subgroup_size_control.adoc](https://github.com/KhronosGroup/Vulkan-Docs/blob/a8b088a3fae12c8575407ea90c7c09bd653db923/appendices/VK_EXT_subgroup_size_control.adoc#L1)

API reference: [VkDeviceCreateInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkDeviceCreateInfo.html), [VkSemaphoreGetFdInfoKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkSemaphoreGetFdInfoKHR.html), [external handle types](https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalSemaphoreHandleTypeFlagBits.html), [subgroup size control](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_subgroup_size_control.html). Последние страницы могут обновляться; нормативные source links выше закреплены.

## Hardware и profiling documentation

- [Valve Steam Deck OLED specs](https://www.steamdeck.com/en/tech/oled/): восемь CU, 1280×800, up to 90 Hz, LPDDR5, APU envelope. Не источник числа KMS planes или ACE queues.
- [Mesa RADV documentation](https://docs.mesa3d.org/drivers/radv.html): структура драйвера и ACO, moving overview.
- [Mesa environment/profiling variables](https://docs.mesa3d.org/envvars.html): profiling discovery, не утверждение наличия каждой переменной в MD.
- [AMD RDNA performance guide](https://gpuopen.com/learn/rdna-performance-guide/): LDS/access patterns; не замер Kedavra.
- [AMD occupancy explained](https://gpuopen.com/learn/occupancy-explained/): wave/register/workgroup constraints.
- [Linux v6.16 power/thermal controls](https://www.kernel.org/doc/html/v6.16/gpu/amdgpu/thermal.html): units/ABI; sensor availability определяется на устройстве.
- [Linux v6.16 KMS](https://www.kernel.org/doc/html/v6.16/gpu/drm-kms.html): atomic/plane concepts, подкреплены pinned kernel code.

Статьи/маркетинг MAKO или Lossless Scaling не использовались как доказательство применимости к Steam Deck. Никакие результаты dGPU или нового RDNA не подставлены вместо VANGOGH.

## Hardware evidence и integrity

- [docs/kedavra/evidence/deck-20261021/README.md](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/docs/kedavra/evidence/deck-20261021/README.md#L1)
- [docs/kedavra/evidence/deck-20261021/bench.json](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/docs/kedavra/evidence/deck-20261021/bench.json#L1)
- [docs/kedavra/evidence/deck-20261021-auto/README.md](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/docs/kedavra/evidence/deck-20261021-auto/README.md#L1)
- [docs/kedavra/evidence/1.4.1-rc.1/README.md](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/docs/kedavra/evidence/1.4.1-rc.1/README.md#L1)
- [docs/kedavra/evidence/deck-20261021/trace-run3-x2.csv](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/docs/kedavra/evidence/deck-20261021/trace-run3-x2.csv#L1)
- [docs/kedavra/evidence/deck-20261021-auto/trace-20261021-105329-auto-x3.csv.gz](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/docs/kedavra/evidence/deck-20261021-auto/trace-20261021-105329-auto-x3.csv.gz#L1)

Повторно разобранные raw inputs сверены с Git blob IDs:
`trace-run3-x2.csv = 4aedda9132941918b05b9acd5ae38d4d15b9eca2`;
`trace-20261021-105329-auto-x3.csv.gz = a750e6b75fb595b0eedfccdf1fb90fef52df7c50`.
Analyzer blob: `5c436f4e707044d8433508c02f72c6cdfb535e8a`. SHA256 фактически обработанных inputs — в приложенных JSON. Эти файлы не переписывают исторические reports.

## Оставшиеся пробелы и порядок их закрытия

1. Manifest текущего Steam Deck OLED, kernel source/config и Mesa binary build ID — E0.
2. Реальное пересечение storage/sample/export/plane modifiers, rotation и color pipeline — E1.
3. Разделение submit/dependency/queue/elapsed/flip времени в 1.4.1 — этап 0, E4.
4. Уникальность видимых generated images и end-to-end latency — optical E2/E3.
5. GPU ms, DRAM bandwidth и APU energy новых вариантов — E2/E5/E7/E8/E9.

Отсутствие этих данных отражено в feasibility/evidence classes. Source-level исследование и инженерный handoff завершены; hardware validation предложенных изменений остаётся обязательным следующим этапом.

Проверка целостности поставки: [validation.json](evidence/validation.json). Отдельный local source cache и рабочие validation scripts не входят в GitHub commits; опубликованы только файлы этого исследовательского каталога.
