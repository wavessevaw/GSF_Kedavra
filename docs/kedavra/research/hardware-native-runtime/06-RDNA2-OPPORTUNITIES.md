# 06. Возможности RDNA 2, применимые к Kedavra

Steam Deck OLED имеет восемь RDNA 2 CU и общую LPDDR5 память; аппаратная спецификация — [Valve](https://www.steamdeck.com/en/tech/oled/). Новая оптимизация полезна, если сокращает работу или лучше использует ограниченный ресурс, сохраняя real FPS, quality и energy envelope. Перечень возможностей новых RDNA GPU не заменяет эту проверку.

## R1. Per-shader Wave32/Wave64 и VGPR pressure

**SUPPORTED:** исследованный RADV сообщает minSubgroupSize=32 на GFX10+, max=64, compute stage допускает required subgroup size: [src/amd/vulkan/radv_physical_device.c:1768](https://github.com/chaotic-cx/mesa-mirror/blob/d246f60ddf53089faa1d554865f7a38a737ed388/src/amd/vulkan/radv_physical_device.c#L1768). В shaders Kedavra нет явного требования wave size. **Не установлено**, какой wave фактически выбрал ACO для каждого production pipeline.

ME использует private `g_win[25]`, массивы forward/backward predictors и ветвящийся early exit: [kedavra/shaders/kfg_me.comp:20](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/shaders/kfg_me.comp#L20). Это конкретный кандидат на VGPR pressure/spills и divergence. Warp и reduce имеют другую нагрузку и не обязаны выигрывать от той же wave size.

Проект: добавить внутренний pipeline key `{shader,required_wave,workgroup_shape}`, query/enable subgroupSizeControl, цепочку `VkPipelineShaderStageRequiredSubgroupSizeCreateInfo` при pipeline creation. Перебрать default/32/64 по отдельности для ME, warp, stats. Не включать `REQUIRE_FULL_SUBGROUPS` автоматически для текущего local_size_x=8: Vulkan требует соответствующий X multiple. Можно запросить wave size без этого флага; варианты 32×2/64×1 — отдельные shader shapes с сохранёнными bounds/barriers.

Точки: [src/rendervulkan.cpp:1186](https://github.com/ValveSoftware/gamescope/blob/aa289ea5977d8b21b92b0f246f0e8f0c225d180f/src/rendervulkan.cpp#L1186), `createShaders/compileAllPipelines`; `kfg_vk.c` для изолированного host; `kfg_me.comp/kfg_warp.comp`. Не устанавливать глобальный RADV_PERFTEST для всей Steam session: это затронет игру и смешает причины изменения.

Доказательство: pipeline executable statistics / ACO diagnostics / SQTT capture при доступности, сохранить wave, VGPR/SGPR/LDS/scratch, elapsed и throughput в idle bench **и под игровой нагрузкой**. Максимальный occupancy не самоцель: меньшая wave или больше resident waves могут увеличить contention. [AMD occupancy guide](https://gpuopen.com/learn/occupancy-explained/) объясняет ограничения регистров/workgroups, но не предсказывает результат Deck.

## R2. Subgroup reductions для stats/global

**CONFIRMED код:** `kfg_stats.comp` использует один workgroup из 256 invocations, shared histogram 4096 uint, tree reductions и много barriers: [kedavra/shaders/kfg_stats.comp:14](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/kedavra/shaders/kfg_stats.comp#L14). Глобальный проход делает camera model, cut — другой branch того же shader. Один workgroup может быть ограничением utilization, однако текущую стоимость этих функций на 1.4.1 Deck надо измерить отдельно.

Проект R2a: для суммы/argmax применить subgroup operations, сохранить partial по subgroup в небольшой LDS и один финальный cross-subgroup reduce. Histogram atomics не исчезают сами собой. Для uint argmax требуется **тот же tie-break bin index**, для floats меняется порядок сложений — quality/cut threshold regression обязательна.

Проект R2b: если R2a не решает tail, разделить статистику на tile partials + compact second reduction, чтобы распараллелить чтение по WGP. Добавляется один dispatch и промежуточный buffer; принимать только если net elapsed/energy меньше. Camera robust fit с несколькими итерациями не сводится к одной сумме: не менять inlier selection ради удобной reduction.

Точки: `kfg_stats.comp`, `kfg_plan_analyze`, новые refs `STATS_PARTIAL` в plan/integration/standalone. VkPhysicalDeviceSubgroupProperties.supportedOperations и shader extensions проверить; wave-size-dependent indexing не делать константой 32. Fallback — current shader, shadow variant с теми же inputs. Пара/история по-прежнему retained до окончания обоих passes.

## R3. LDS reuse в motion search

**HYPOTHESIS.** Соседние invocations читают перекрывающиеся SAD windows. Уже есть private window cache и k_good early exits; повторять это как новую идею нельзя. Возможная новая работа — tile luma A в LDS с halo и coalesced loads; часть доступа B тоже tiled лишь когда диапазон displacement известен.

На L2 current window 3×3, radius refine=1; на coarsest window 5×5 и radius=6, temporal/global vectors могут уводить поиск за halo. Один большой LDS tile для всего предполагаемого диапазона может оказаться дороже texture cache. Кандидат: static source A window (g_win) загружать общим tile, B оставить через texture sampler, затем сравнить. Нельзя класть barrier после divergent per-invocation return для неполных групп: сначала collective load/barrier, только затем bounds exit/early search.

Точки: `kfg_me.comp::load_window/sad/search/main`; shape variants в pipeline key R1. Данные: shared luma tile+halo, без новых persistent GPU images. Сохраняются bilinear half-texel refinement и clamp/border semantics. [AMD RDNA performance guide](https://gpuopen.com/learn/rdna-performance-guide/) описывает banked LDS и coalesced image access; выигрыш зависит от реальных counters.

## Общий инженерный контракт R1–R3

| Пункт | R1 | R2 | R3 |
|---|---|---|---|
| Текущая архитектура | default ACO wave | один WG + LDS tree | private окна и texture taps |
| Ограничение | возможный VGPR/divergence | barriers и малый parallelism | overlapping loads |
| Изменение | per-pipeline wave variants | subgroup/partial reduction | shared source tile |
| Устраняемые затраты | spills/неудачная occupancy, если есть | часть barriers/shared traffic | повторные source taps, если texture cache не справляется |
| Файлы | render pipelines + shaders/test host | stats + plan + refs | ME + pipeline shape |
| Новые данные | PipelineVariantKey | partials и subgroup sums | LDS tile |
| Interfaces | subgroup size control | subgroup ops / storage partial buffer | ordinary compute shared memory |
| Владение | прежние images | partials per pair epoch | только workgroup lifetime |
| Sync | прежние image barriers | additional pass dependency при R2b | uniform workgroup barriers |
| Fallback | default pipeline | current stats | current texture path |
| Governor | отдельный cost epoch, trials frozen | то же | то же |
| Риск | driver compile/cache и spills | cut/float/tie regressions | OOB/barrier deadlock/occupancy |
| Этапы | query→compile→bench→game | R2a→quality→R2b gate | load-only prototype→parity→timing |
| Hardware proof | E8 counters+tails | E8 per-pass+cut corpus | E8 taps/LDS/VGPR+full workload |
| Stop | нет net gain | latency/quality хуже | больше LDS/столкновений или нулевой benefit |

## Что ещё проверено

- **Dispatch overhead:** analysis уже split на две submissions, но 12 passes. Уменьшать число dispatch можно только при сохранении межгрупповых зависимостей. Global camera stats нельзя объединить с ME L4 одним обычным dispatch, ожидая completion всех workgroups через `barrier()`: он локален workgroup. Такой «fusion» **REJECTED**. Copy+reduce с двумя targets — отдельная C3, [04](04-MEMORY-ZERO-COPY.md).
- **Display Core scaling/color/alpha:** перспективнее большинства micro-optimizations, потому что может убрать full-frame BLIT; только по A1/A2 gates. Hardware scaler не заменяет FSR/NIS/SGSR с равным качеством.
- **DCC/modifier:** SUPPORTED infrastructure, не универсальная настройка; display-compatible compression и storage writes проверять вместе. FP16 HDR удваивает colour bytes и сужает modifier options.
- **Async copy/SDMA:** общий memory bandwidth остаётся; сначала удалить copy, затем изучать более дешёвое исполнение оставшейся legal copy.
- **FP16 arithmetic:** storage RGBA16F уже есть, а арифметика float32 в shaders. Глобальный перевод SAD/camera/warp на FP16 меняет quality, особенно HDR и cost comparisons. Не первый кандидат; допускается только локальный эксперимент с неизменёнными quality gates, без обещаний экономии.
- **Ray accelerators, VRS, mesh shaders:** для текущих compute image passes не устраняют работу. **REJECTED** как направления этого проекта.
- **Аппаратный optical-flow API:** в исследованном маршруте RADV/Kedavra не обнаружен доступный Deck-specific backend, заменяющий motion analysis с доказанным качеством. **BLOCKED до предъявления конкретного API/capability**, не утверждение об отсутствии любых внутренних возможностей silicon.
- **Прямое программирование ACE/MEC, kernel patch ради priority, firmware/SMU:** не требуются предложенным решениям; **REJECTED** по сложности/риску и границам Research Only.

## Profiling

Возможные инструменты: Vulkan timestamps, pipeline executable statistics, Mesa SQTT/RGP, GPUVis + trace-cmd, read-only hwmon/fdinfo. Для каждой установленной Mesa проверить доступный способ trigger и capture; [актуальная Mesa документация](https://docs.mesa3d.org/envvars.html) не является спецификацией старой devel сборки. Captures могут менять clocks и stalls: результаты с profiling отделять от энергетического A/B/A. Если на VANGOGH нужный counter недоступен, публиковать unavailable и наблюдаемые elapsed/FPS/power, а не вычислять фиктивный bandwidth из shader taps.
