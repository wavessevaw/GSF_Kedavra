# 08. Воспроизводимые эксперименты

## Выполнено здесь

Среда исследования — Windows, физического Steam Deck/DRM output нет. Рабочий runtime не запускался и не изменялся. Проведены source audit и **повторный анализ архивных hardware traces** анализатором из Kedavra `5f22938b55f9853f764e927c847d9c1a8d9a8c04`.

| Архив | Сохранённый отчёт | Повторный разбор текущим analyzer | Интерпретация |
|---|---|---|---|
| 10:53 Auto, 839,93 s, 75 214 selected rows | missed 0,268567%; P95/P99 0,001617/0,002617 ms | missed **0,775153%**, 384 unobserved slots, 202 repeat rows | Historical FAIL по <0,5%; при предположении lossless logger |
| Run3 x2, 115,13 s, 7 482 rows | missed 3,688853% | missed **30,464151%**, 2 881 unobserved slots, 276 repeats | Сильная разница между repeat-row fraction и refresh-slot accounting |

Сохранённые отчёты других сессий тоже изучены: первый x2 run имеет P95≈44,47 ms, mixed manual x3≈11,11 ms, ранний Auto≈11,11 ms. Их цифры не являются baseline 1.4.1. Первые ручные сессии относятся к старым 0.2.x, Auto build не полностью зафиксирован. Части файлов/отчётов имеют origin=unlabelled; attribution к Deck берётся из сопровождающего README, а не из имени файла как аппаратного доказательства.

Повторно разобраны **два названных исходных файла**, остальные архивы оценены по сохранённым reports/README. JSON повторного разбора:

- [Auto reanalysis](evidence/reanalysis-auto-105329.json)
- [Run3 reanalysis](evidence/reanalysis-run3-x2.json)

Первый аппаратный `bench.json`: analysis_ms=4,7731 и two_outputs_ms=0,9907, device RADV VANGOGH, 1280×800, 200 repetitions. В списке проходов нет нынешнего global camera pass и нынешних repair/cached-pair stages. Поэтому эти числа — исторический standalone workload, **не стоимость текущей 1.4.1 под игрой**. Сумма медиан отдельных passes также не является медианой end-to-end pair time. Источник: [docs/kedavra/evidence/deck-20261021/bench.json:1](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/docs/kedavra/evidence/deck-20261021/bench.json#L1).

1.4.1-rc.1 evidence прямо указывает lavapipe; timing variance больше наблюдаемой разницы. Этот набор полезен для quality regression и программной корректности, не для AMD GPU ms/watts: [docs/kedavra/evidence/1.4.1-rc.1/README.md:1](https://github.com/wavessevaw/GSF_Kedavra/blob/5f22938b55f9853f764e927c847d9c1a8d9a8c04/docs/kedavra/evidence/1.4.1-rc.1/README.md#L1).

### Почему старые reports отличаются

Актуальный `kfg_pacing.py` оценивает недостающие fixed-refresh слоты по расстоянию между flip timestamps и включает их вместе с repeat rows в denominator/numerator. Старые сохранённые missed_fraction совпадают с repeat/n rows и не показывают unobserved slots. Текущий analyzer также иначе сохраняет chain/нормализует фазы, поэтому могут измениться max interval и phase histogram.

Формула: `missed=(repeat_rows+Σ(max(0,round(dt/period)-1)))/(rows+unobserved)` в пределах непрерывного evaluation segment. Это **inferred missing slots**, а не optical missed images. Logger loss и отсутствующий flip без sequence/present identity неразличимы. В Auto исходные 202 repeats/75214 rows = старые 0,268567%; с 384 inferred slots — 586/75598=0,775153%. Не выдавать это за новое ухудшение и не исправлять старые evidence файлы в текущей ветке.

`compose_late=0` в старых трассах не доказывает отсутствие опоздавшей GPU работы. `kind=gen` отражает decision, а не пиксельный факт. `me_late` относится к scheduler input readiness contract данной сборки.

### Повторение архивного анализа

Использовать checkout **по полному SHA**, не движущуюся main. Распакованный trace не менять; wrapper читает .gz напрямую.

```sh
ROOT=/path/to/GSF_Kedavra-at-5f22938
R="$ROOT/docs/kedavra/research/hardware-native-runtime"
python3 "$R/tools/recompute_historical.py" \
  --analyzer "$ROOT/kedavra/tools/kfg_pacing.py" \
  --trace "$ROOT/docs/kedavra/evidence/deck-20261021-auto/trace-20261021-105329-auto-x3.csv.gz" \
  > reanalysis-auto.json
python3 "$R/tools/recompute_historical.py" \
  --analyzer "$ROOT/kedavra/tools/kfg_pacing.py" \
  --trace "$ROOT/docs/kedavra/evidence/deck-20261021/trace-run3-x2.csv" \
  > reanalysis-run3.json
```

Если baseline checkout ещё не содержит wrapper, брать его из research branch, а analyzer и trace — из зафиксированного base checkout. Wrapper выводит hashes обоих входов, source SHA и `new_hardware_measurement=false`. В Linux/Windows line endings analyzer могут отличаться; сохранять фактический hash и git revision. Архивная gzip проверена по Git blob SHA, input SHA256 — в JSON.

## E0. Manifest и A/A noise floor

Перед любым будущим hardware тестом:

1. Запустить [read-only capability probe](tools/capability_probe.sh) на выделенном стенде. Он не получает DRM master, не делает modeset, не пишет sysfs. Не добавлять sudo автоматически ради отсутствующего sensor/debugfs.
2. Сохранить OS BUILD_ID, kernel uname/config/package build ID, Mesa package/full Git SHA при доступности и shared-library Build ID, Vulkan driverID/deviceUUID/deviceID/revision, game/Proton/version, patched Gamescope SHA и hash binary/patch set. Записать kernel fork source/config mapping, а не только upstream version.
3. Полный vulkaninfo: families/count, timestampValidBits/period, allowed global priorities, timeline, external memory/semaphore handle properties, subgroup operations/size control, calibrated timestamps/time domains. `vulkaninfo --summary` недостаточен.
4. DRM inventory read-only `drm_info` либо `modetest -M amdgpu -c -p` **без -s/-w**: connector/CRTC/planes, IN_FORMATS blob, modifiers, formats, rotation, color properties, fences. Если tool/device access отсутствует — unavailable; не красть DRM master у рабочей session.
5. На отдельном Vulkan allocator probe получить полное intersection usage=SAMPLED|STORAGE + DMA_BUF export по каждому format/modifier; сохранить selected modifier/planes/stride/memory size. Этот специализированный query probe ещё должен быть реализован в будущем prototype; один vulkaninfo не заменяет его.
6. Записать all control settings: refresh, source cap, HDR/brightness/Mura, HUD/overlays, filters, game resolution/quality, TDP policy/ceiling, GPU/CPU limits, fan/ambient/charge state, AC/battery.
7. Сначала две/три одинаковые baseline A sessions для оценки шума. Instrumented A сравнить с uninstrumented A: timestamps/counters/profile captures тоже создают overhead.

Файлы этой ветки не являются исполняемым DirectScan prototype. Отсутствующие hardware capabilities не заполняются результатами lavapipe.

## Режимы

| Режим | Source period | Display period | Проверка |
|---|---:|---:|---|
| ×2 45→90 | 22,222… ms | 11,111… ms | 1 distinct intermediate на cadence-locked pair |
| ×3 30→90 | 33,333… ms | 11,111… ms | 2 distinct intermediates на locked pair |
| ×2 30→60 | 33,333… ms | 16,666… ms | 1 intermediate, другой cutoff/redzone |
| 24→90 | 41,666… ms | 11,111… ms | Нецелый 3,75 cadence: интервалы 3/4 vblank, без требования «×3 ровно» |

24→90 настраивать custom base_fps=24 либо соответствующим Auto floor, фиксировать фактически подтверждённую policy. Нельзя сравнивать 24→90 и 30→90 как один multiplier. Для fixed ×2/×3 analyzer `--expect-mult` применять только к steady locked участкам, Auto transitions — отдельными segments.

## Обязательный набор измерений

Новая per-job trace schema:

```text
engine_session, epoch, paint_id, pair_a, pair_b, frame_content_id
stage, queue_family, queue_index, requested_priority, granted_priority
cpu_record_begin/end, cpu_submit_enter/return, submission_id
wait_points, signal_point, gpu_begin/end, query_availability
calibration_pair, clock_domain, max_deviation
target_vblank_ns, commit_cutoff_ns, ready_observed_ns
route, format, modifier, pool_generation, slot_id
present_id, crtc_id, atomic_result, ready_fence_id, flip_sequence, flip_ns
kind, actual_applied, phase, content_pos, repeat_reason, loss_count
```

Scalar metadata, no full-frame readback in measured path. Timestamps вокруг capture/copy, post, каждой analysis part, warp, Gamescope composite и export transition. Existing composite timing может **включать Kedavra**, не вычитать несопоставимые агрегаты. Query availability читать nonblocking после нескольких кадров, query slot нельзя переиспользовать до completion.

Собрать P50/P95/P99/P99.9 и max, count/coverage, streaks, real FPS, unique image/content steps, missed refresh slots, result age. Для periodic scheduling interval распределения полезнее только среднего FPS.

## E1. Allocation и DirectScan correctness

Future dedicated lab prototype, не текущая пользовательская session.

- Выделить pool без отправки display commit; проверить export/import FB и storage writes на SDR formats/modifiers.
- Записать known pattern с frame ID и CRC, readback **в отдельной correctness фазе**, сверить каналы/alpha/stride/rotation.
- TEST_ONLY на реальном display configuration, когда session уже корректно принадлежит test Gamescope. Не считать успешный TEST_ONLY доказательством готовности fences или цвета.
- Чередовать generated outputs, REAL и REPEAT; event token и visible slot должны соответствовать. Fill/reuse stress не должен менять ещё visible image.
- Отрицательные случаи: unsupported modifier/format, resize, HDR toggle, no spare slot, denied explicit fence, failed commit, VT/session loss.

Диагностический признак пользы: в eligible GEN кадре ровно один warp и **нет** full-frame BLIT/copy между ним и FB. Проверить через command trace/capture, не только boolean `direct=true`.

## E2. DirectScan A/B/A и энергия

A — Kedavra 1.4.1; B — ровно A1/A2 с таким же shader algorithm и initially таким же CPU wait. Сначала SDR native size, затем штатный HUD/Mura, Steam overlay/notifications, external display и HDR по отдельным eligibility groups. Доля eligible frames публикуется; не выключать features для общего результата.

Одинаковый game replay/save route/camera script. Warm-up минимум 10 минут до thermal/clock steady state, затем A 30 min → B 30 min → A 30 min; повторить минимум три блока в разном порядке/дни для power drift. Если cooling/scene не стабилизировались — продлить, записать правило исключения заранее. Между legs одинаковое возвращение к scene/thermal start, одно состояние governor/limits. Краткие диагностические тесты не заменяют длительные.

Сравнить полный workload: real + generated + repeats + routing/fallback/plane cost. Измерять composite elapsed, total GPU elapsed regions, game real throughput, deadline tails, APU J/Wh. В SDR подсчитать логические image bytes, actual DRAM bytes только если доступен валидированный hardware counter.

Критерий: меньше измеренных cost/energy при сохранении изображения, real FPS и pacing; доверительный интервал разницы не пересекает практический noise floor A/A. Процент заранее не назначается. Если min detectable effect выше возможного эффекта — увеличить trials либо честно «не различимо», не объявлять ускорение.

## E3. Transitions и стабильность

Длительный цикл overlay on/off, QAM/Steam menus, notification, cursor, Mura/brightness updates, HDR/PQ/scRGB/SDR, capture start/stop, resize/upscale, 60/90 Hz, suspend/resume, dock/hotplug на лабораторном устройстве. Для каждой операции заранее записать expected route и epoch. Переходы сравнивать с production A; ни один слой не исчезает и не интерполируется вместе с игрой.

Проверять FD/FB/VkDeviceMemory counts после сотен переходов, no device-lost/AMDGPU reset, no stale completion. Restore test возвращает прежнюю session и controls. При GPU hang/black screen остановить run и использовать преднастроенный session recovery/проверенный boot; не продолжать tuning.

## E4. B4/B1 readiness и deadlines

A0 — production +2, A1 — instrumented +2, B — observed completion; затем отдельно admission chunks. Искусственные задержки допускаются как **software failure injection**, не hardware performance result.

Логировать actual analysis completion и earliest decision using pair. Инвариант B: ни одной GEN со stale epoch/неполной парой; late job не блокирует fallback ready frame. Считать frames до valid pair, missed deadline, content age/latency и real FPS. Число `me_late` может вырасти из-за честной readiness; это не само по себе regression. Сравнивать pixel novelty/pacing/end-to-end.

Для B1 определить cutoff margin по измеренной baseline distribution; sweep chunk size по pass boundaries. Не смешивать с shader algorithm changes. Stop при росте очереди анализа, starving pairs или ухудшении игры.

## E5. Queues и priority

Сравнить: serial queue → две compute queues той же family/priority → отдельно compute analysis normal и general final high/realtime (разные families). Последняя конфигурация включает ownership barriers в стоимость. Две queues одной family с разными global priorities невалидны.

Под одинаковой game load проверить hardware overlap, delays, CU/cache/memory contention и shader elapsed. В kernel trace сохранять ctx/ring/job IDs. Возможные tracepoints reference kernel: [drivers/gpu/drm/amd/amdgpu/amdgpu_trace.h:166](https://github.com/torvalds/linux/blob/038d61fd642278bab63ee8ef722c50d10ab01e8f/drivers/gpu/drm/amd/amdgpu/amdgpu_trace.h#L166). Сначала enumerate available_events, не предполагать наличие конкретного имени/permission в SteamOS. `run_job` не равен первой shader wave. Profiler captures короткие и отдельно от power run.

Stop: queueCount<2 для same-family опыта, permissions denied без корректного fallback, overlap не даёт net ms/Wh, real FPS падает. Не сохранять постоянный realtime analysis ради меньшего показателя warp.

## E6. KMS fence handoff

Сравнить wait-before-commit и actual IN_FENCE, при неизменном A1 pass count. Проверить unsignaled, late, export-failed и completed fence, TEST_ONLY/real commit/error. `OUT_FENCE_PTR` на успешно принятом commit и event ID сопоставить; release старого FB только после replacement. При повторе same FB slot продолжает считаться visible. Проверять leak/open FD/BO counts.

Success: CPU wait действительно исчез из критичного участка, картинка не рвётся и deadlines/energy не хуже. Fence signaled не означает, что кадр успел в конкретный vblank.

## E7. C1 post destination parity

Все last-pass варианты, post-only и generation-on, SDR/PQ и scRGB bypass. Один immutable source corpus в standalone RADV host; current quality gates из 1.4.1 без ослабления. Readback вне performance runs, сравнить реальные пиксели и cut/motion results. Убедиться, что последний shader samples другой image, а не свой dst. Command trace подтверждает исчезновение copy. Затем E2 с одинаковыми фильтрами и реальным source FPS.

## E8. Shader / memory / SDMA profiling

По одному R1/R2/R3/C3 варианту. Сначала per-pass bench на RADV с driver/device IDs, потом game-contended workload. Сохранить wave/VGPR/LDS/scratch и counters, если доступны. SDMA engine usage доказывать trace, не TRANSFER_BIT. DCC: реальные selected modifiers, compression/decompression transitions и total cost.

Если bandwidth counter отсутствует — «Не измерено», отдельно расчёт логических bytes из [04](04-MEMORY-ZERO-COPY.md). Загрузка GPU не даёт GB/s. CPU lavapipe quality допускается, её timing не переносить на RDNA.

## E9. Governor

Сначала shadow advisor без writes. Разметить compute-heavy, queue-contended, dependency-late, CPU-bound и thermal portions. Проверить precision classification по trace, unknown при пропусках данных. Потом один reversible actuator trial через существующий lease; current ceilings/thermal limits неизменны. Сравнение A/B/A на session energy, useful real FPS/content updates и deadline tails.

Проверить одновременно запрещённые trials, внешний Steam slider, restart/crash receipt, stale schema, battery/AC transition, suspend. При failed restore прекратить эксперимент; не пытаться компенсировать соседним actuator.

## Единые критерии приёмки и остановки

P95 interval error ≤1 ms, P99≤2 ms, missed<0,5% — цели исходного ТЗ, применяются вместе с полнотой данных и отдельным содержимым. Не заменять их округлением KMS timestamps. Content ordering строго монотонно; repeat/slowdown/speedup/jump публикуются отдельно. Quality corpus и HDR/Mura визуальная эквивалентность обязательны.

Для optical: coded frame ID/phase marker внутри final output в correctness-only build + камера с известным shutter/частотой; 240 fps даёт шаг 4,17 ms и не доказывает sub-ms end-to-end. Для точных latency tails — высокоскоростная камера/photodiode с input trigger и calibration. При отсутствии метода end-to-end latency = unavailable.

Любая новая instability, corruption, stale write, device reset, потеря UI, рост real-frame latency без разрешённого budget или energy gain через ухудшение картинки — stop. Нулевой/неразличимый net benefit после воспроизводимого испытания — основание отклонить candidate.

Итоговый report хранит raw trace hash, binary/source IDs, все настройки, exclusions с причинами, duration/coverage, per-run и aggregate CI. Не удалять неудачные trials и не выбирать только хороший участок игры.
