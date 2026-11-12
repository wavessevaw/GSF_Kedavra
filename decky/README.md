# GSF Decky Test 0.1.7 — calibrated latency admission

0.1.7 includes trustworthy submit-to-GPU-start latency in the synthetic-work
budget. Once calibrated latency is known, visibility probes share the bounded
two-second recovery retry instead of bypassing an impossible budget every
12 source frames. Fresh measurements reopen normal generation when it fits.
No source FPS floor or game limiter is imposed. This protects source output;
it does not establish sustained Deck 2×.

The control `normal-no-latency-admission` retains calibration/watch telemetry
but selects the previous admission policy. `normal-no-calibration` also removes
the calibrated input to admission and is no longer an equal-policy overhead
comparison. Pair telemetry exposes `submit_delay_budget_ns` and
`submit_delay_budget_calibrated`. See the repository's
[0.1.7 evidence and acceptance](../docs/GSF_CALIBRATED_ADMISSION_0_1_7.md).

# GSF Decky Test 0.1.6 Diagnostic (GPU calibration + AMDGPU sensors)

REPORT.json includes calibration_status_counts, actual calibrated submit_to_gpu_start_median_ms and per-window global GPU busy; invalid calibration samples remain null, never assigned to queue wait. No game-specific GPU utilisation or source-FPS regression can be inferred without controlled baseline.

Opt-in VK_KHR/VK_EXT_calibrated_timestamps is enabled on the application's Vulkan device only when supported. For GPU pairs, te_submit_frame captures CLOCK_MONOTONIC directly after successful queue submission and GPU raw query 0 is exported by te_sequence. The field **submit_to_gpu_start_ns** is not a pure queue-wait measurement. Calibration is attempted every 10 generated frames or 100 ms, with per-pair maxDeviation gating (<=1ms), stale pair detection and null fields when untrustworthy. All timestamps are diagnostic, no change to frame_clock, LMC, admission or timeouts.

A launcher-side Python sampler records available /sys/class/drm/card*/device gpu_busy_percent, pp_dpm_sclk, power1_average and temp1_input every ~100ms in amdgpu.jsonl without root. Data is global GPU usage, not game-specific; ambiguous/missing GPU yields unavailable. Game Mode diagnostics include normal-no-calibration and normal-calibration-every-frame for A/B overhead checks.

GPU samples are joined to the pair that actually submitted synthetic work using
`(pid, generation, te_sequence)`. `pair.te_submit_sequence` is zero when that pair
did not submit TE work; the legacy `te_sequence` remains a cumulative counter.
Bypassed pairs cannot move an earlier job to a later reporting window. Older
records without a complete namespace are joined only when their origin is
unique. `gpu_sample_joins` reports matched, unmatched and ambiguous records for
each GPU event type; ambiguous samples remain absent from per-window medians.
Global calibrated medians still describe all valid samples, including those
whose reporting window cannot be identified.

# GSF Test 0.1.5 Diagnostic — Decky, Game Mode

**0.1.5 measures completion of real LMC GPU jobs after WSI missed its deadline.**
It does not change frame_clock, shader implementation, admission or output timeout. New `GFG_NATIVE_TIMELINE_WATCH=1` (in Game Mode) records per-te_sequence readiness and 200ms observation limits, and 3 control modes can be configured from the Decky panel (restart game).

`timeline_watch` telemetry records `wait_threshold`, `actual_signal_value`, `submit_status`, `deadline_kind`, `watch_end_reason`, `timeline_observed_ns`, `last_below_ns`, `watch_poll_interval_ns`. A `threshold_reached` event is a CPU observation of a monotonic Vulkan semaphore threshold, **not** independently verified execution of a particular command when signals skip values, GPU completion timestamp, WSI acceptance or physical OLED scanout.

`normal-no-watch`: identical normal 2× path with watcher disabled for overhead A/B.

`source-only`: preserve original presents through worker without TE synthetic generation. `compute-only`: existing unthrottled compute-only stress test. `matched-compute`: use normal generation admission policy without synthetic readiness wait or presentation; comparisons are NOT equal-load scans when Gamescope frame rate changes.

A/B benchmarks must use one saved scene and comparable thermal state; watcher overhead must be measured using WATCH=0 vs WATCH=1 under identical game workloads. Auto-generated `REPORT.json` includes `diagnostic_mode` and a watcher completion summary. Timeline watch only covers same-device GPU LMC (not CPU-fit or shared-fd).

# GSF Test 0.1.4 — Decky, игровой тест 2×

**0.1.4 — ограниченная проверка реального LMC-кадра в игровом WSI.**
В тестовом режиме установлен LMC tier 2 (сокращённый поиск движения),
чтобы снизить GPU-стоимость; качество интерполяции может быть ниже tier 0. Раз в 12 исходных
кадров при наличии промежуточного дисплейного слота и отсутствии очереди
делается короткая попытка GPU LMC, даже если обычная оценка GPU-cost отклонила
синтетику. Ожидание готовности GPU ограничено 12 мс, получение WSI-образа
ещё 2 мс. Кадры, не прошедшие фактическую готовность и WSI, отбрасываются.
`visibility_probe` в game.log показывает `synthetic_wsi` и `real_wsi`.
Это не гарантия 2×, не проверка физического scanout и не исправление
GPU-конкуренции. Приоритет исходных кадров сохраняется.

## Изменения 0.1.3 и 0.1.2

Экспериментальный GPU LMC для Steam Deck OLED. Пакет работает автономно,
содержит GPU слой и исходники, ничего не скачивает при включении.
Поддерживаемый первый сценарий: игра x64 через Vulkan, DXVK или VKD3D-Proton.
Игра и конкретный Proton ещё требуют аппаратной проверки.
0.1.3 убирает ожидание единственного слота очереди и занятого GPU-буфера.
При накоплении исходных кадров генерация уступает им время вывода; исходные
кадры сохраняются в порядке отправки. Подробный лог сбрасывается пакетами.
Это исправление задержек, а не подтверждение устойчивых 2× на Steam Deck.

0.1.2 добавляет проверенные UNORM/SRGB mutable-format swapchain DXVK.
Если GPU слой отказался подключить генерацию, панель показывает причину отказа;
одна лишь команда запуска не считается подтверждением работающего генератора.

1. В настройках Decky включите режим разработчика и установите этот ZIP через
   **Install Plugin from ZIP / Установить плагин из ZIP**. Не распаковывайте ZIP
   внутрь другого ZIP. Плагин называется **GSF Test**.
2. Откройте GSF Test в боковом меню, выберите установленную Steam-игру.
   Появятся её название и подтверждение выбора. Выбор сохраняется при закрытии
   панели и перезапуске Decky. Сам выбор ещё не меняет параметры запуска.
   Для отдельного эксперимента уберите запуск через GFG/MAKO/LSFG у этой игры.
3. Нажмите **Включить 2×** сразу под названием игры. Плагин установит слой в пользовательскую
   папку, сохранит прежние параметры запуска и проверит запись новых через API
   Steam. После подтверждения вы увидите точную записанную команду вида
   `python3 /home/deck/.local/share/gsf-test/launch.py --appid APPID -- %command%`.
   Существующие параметры игры сохраняются в этой строке. Ничего вручную
   вставлять не нужно. Перезапустите выбранную игру. На OLED для первого теста выберите 90 Гц.
4. Играйте с её обычным меняющимся FPS. GSF не задаёт игре цель 30/40/45 FPS,
   не меняет TDP, частоты GPU, разрешение и лимитер Steam. Режим 2× пытается
   добавить один кадр между исходными, пока хватает времени и слотов экрана.
   На 90 Гц выше 45 исходных FPS полный 2× не помещается: часть генерации
   пропускается, оригинальные кадры продолжают идти.
5. Смотрите состояние в плагине. «2× настроен» означает только параметры
   следующего запуска. «Генерация кадров подтверждена» появляется по живым
   счётчикам GPU слоя, а не по положению переключателя.
6. После выхода автоматически сохраняется
   `~/GSF-Test-Results/GSF-Game-Latest.zip`. **Сохранить отчёт** делает снимок
   текущего/последнего прогона. Отчёт никуда не отправляется автоматически.
7. **Восстановить запуск без GSF** возвращает прежнюю строку запуска, если
   вы не изменили её самостоятельно. После изменения нужен перезапуск игры.

Это тест генератора в настоящей игре, не гарантия совместимости каждой игры.
Счётчики обозначают кадры, принятые WSI, и не равны доказанному физическому
FPS панели. На этом этапе есть нерешённый риск потери окна/swapchain в Game
Mode; такие сбои сохраняются в отчёт, а не объявляются успешным тестом.
Автоматический тест использует реальный Vulkan и LMC на программном драйвере;
он не заменяет Steam, Proton или OLED.

При удалении плагина остаётся маленький отключённый launcher: Steam может
ссылаться на него, и он пропускает запуск игры без GSF даже без GPU файлов.
До удаления удобнее восстановить параметры кнопкой в плагине.
Журналы текущего запуска ограничиваются примерно 16 МиБ для game.log и
32 МиБ для JSONL; при обрезке это явно записывается в отчёт.

GSF: GPL-3.0-or-later. Исходники этой сборки — SOURCE.tar.gz, список пакетов
сборки и точные версии — decky/package-lock.json в исходниках. Decky UI и
React предоставляет установленный Decky Loader. Исходники и лицензии
использованного API/loader shim приложены в third_party.

The opt-in `normal-queue-observe` diagnostic records application submit completion evidence at TE enqueue. It preserves normal admission and calibration. Counts are upper bounds, not hardware queue depth; see `docs/GSF_QUEUE_OBSERVATION_DIAGNOSTIC.md`. The normal profile leaves it disabled. Launcher-prefixed GFG_NATIVE variables are intentionally filtered; select the diagnostic profile instead.
