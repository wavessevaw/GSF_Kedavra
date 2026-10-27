# 14. Механизмы системной эффективности: разбор SDWEAK и scx_lavd

Дата: 25 октября 2026. Цель — объяснить конкретные механизмы, проверить применимость к Kedavra/GFG и определить причинные эксперименты. Установленность или обновлённость SDWEAK не является целью. Пользователь уточнил практический результат: в разных играх около 30 → 33–35 **реальных** FPS. Этот результат принимается как полезная исходная база; разделение причин требуется для дальнейшего улучшения, а не для отклонения пользовательского наблюдения.

## База дальнейшей разработки: улучшать поверх SDWEAK

Основное A — рабочая конфигурация с полезными механизмами SDWEAK и текущим SteamOS; B — та же конфигурация плюс ровно одно новое изменение. Задача не требует сначала доказать, что пользователь видел прибавку. Она требует найти добавочную экономию относительно уже улучшенной системы. Переключение всего SDWEAK on/off не является первым экспериментом и не выполняется через его installer/uninstaller.

30 → 33–35 FPS соответствует изменению среднего source interval 33,333 → 30,303–28,571 мс, разнице 3,030–4,762 мс. Это разница периодов кадров, не измеренная экономия CPU execution и не GPU timestamp. В режиме cap=30 этот запас может проявиться как меньшее потребление и меньше поздних кадров, а не рост счётчика выше 30. Проверка при фиксированном cap и проверка максимального real FPS решают разные задачи.

Приоритеты улучшения:

1. Связать CPU wakeup/runqueue → game submission → GPU execution → compositor/KMS в единую timeline; сохранять точность clock mapping и coverage. Дорабатывать bottleneck, который задерживает текущий workload. Если нет корректных runqueue данных, диагноз CPU scheduling остаётся неизвестным. На устройстве `sched_schedstats=0`, а ftrace недоступен обычному SSH user; исторические ненулевые `schedstat` нельзя объявлять текущими измерениями wait.
2. Проверять известный механизм LAVD вместе с его управлением performance target. Улучшение распределения CPU времени и увеличение потребления — разные эффекты. Новый слой GFG не должен независимо переписывать настройки существующего scheduler/SteamOS.
3. Изменения reclaim/THP/streaming I/O проверять по наблюдаемой причине, отдельно от scheduler. Полезные настройки SDWEAK сохранять. На GPU-bound сцене без memory/I/O stalls тратить ресурсы на новый глобальный memory tuning не обосновано.
4. Устранять измеренную конкуренцию независимых фоновых приложений; сохранять Steam Input, звук и зависимости launcher/game. Не путать встроенный фон Steam с независимым приложением.
5. Совмещать системную экономию с уменьшением GPU work в Kedavra: predictor SAD cache, устранение copies, completion-aware presentation и условный DirectScan. Комбинированный вариант проверяется после независимых A/B, поскольку выигрыш двух механизмов не обязан складываться.

Контракт поставки: capability/state detection в plugin → один выбранный обратимый trial → readback фактического состояния → измерение → сохранить выигрыш либо восстановить baseline. Журнал содержит исходные значения, владельца настройки, build/schema, применённый механизм, фактическое состояние, результат сравнения и rollback. Ошибка применения не скрывается за включённым UI toggle. Восстановление действует и после сбоя plugin; периодических глобальных trials во время обычной игры по умолчанию нет.

В критерии эффективности входят два режима: больше полезных real frames при той же измеренной энергии/мощности либо меньше энергии при тех же real FPS. Для обоих остаются ограничения качества, input latency, presentation и стабильности. Снижение TDP cap не заменяет измерение, а рост GEN/flip FPS не заменяет рост real FPS. Конкретные параметры и функции описаны ниже; неизвестный downstream `--dd-max-wait-us` не настраивается до установления семантики.

Границы: не выполнялись installer/uninstaller, sysctl/sysfs writes, изменения scheduler, остановка приложений, замена kernel/Mesa/Gamescope, установка Power Split/GPU Clock. Последние два модуля исключены из предлагаемой архитектуры по указанию пользователя. Все будущие пользовательские действия, результаты и откат должны быть доступны через plugin.

## Версии и первичные источники

- SDWEAK: [Taskerer/SDWEAK](https://github.com/Taskerer/SDWEAK/tree/8c32e86fff3c754ac4819524edf7fff4c4e04de3), SHA `8c32e86fff3c754ac4819524edf7fff4c4e04de3`, SDWEAK 2.1. Прочитаны installer, uninstaller, runtime script, common, scx config, energy service/path, ZRAM config и release workflow.
- scx_lavd: upstream tag v1.1.3 → commit [`c8728c6b6a3fde451f0f10b95f99aad7a32a750a`](https://github.com/sched-ext/scx/tree/c8728c6b6a3fde451f0f10b95f99aad7a32a750a). Прочитаны `main.rs`, `main.bpf.c`, `lat_cri.bpf.c`, `power.bpf.c`.
- Это не точные исходники downstream `scx-scheds 1.1.3.linux.steamos.2-1`. Наблюдаемый параметр `--dd-max-wait-us` не найден в исследованном upstream. Его семантика и downstream patch **не установлены**. Попытка read-only `--help` завершилась кодом 255 без stdout и не дала подтверждения. Выдумывать значение флага по имени нельзя.
- Нормативные ABI: [THP](https://docs.kernel.org/admin-guide/mm/transhuge.html), [sysctl VM](https://docs.kernel.org/admin-guide/sysctl/vm.html), [DRM client statistics](https://docs.kernel.org/gpu/drm-usage-stats.html), [systemd freeze/thaw](https://www.freedesktop.org/software/systemd/man/latest/systemctl.html).

## 1. Критический путь CPU, а не общий CPU utilization

Рассматриваем цепочку: игровой producer → render/submission thread → драйвер/GPU completion → Gamescope paint/submission → KMS commit. Для каждого CPU участка различаются: время работы, время runnable в runqueue, сон на зависимости и page fault. Свободное ядро или низкий средний CPU busy не доказывают, что критичный поток своевременно получил исполнение.

В [`lat_cri.bpf.c::calc_lat_cri`](https://github.com/sched-ext/scx/blob/c8728c6b6a3fde451f0f10b95f99aad7a32a750a/scheds/rust/scx_lavd/src/bpf/lat_cri.bpf.c#L172) используются реальные поля `wait_freq`, `wake_freq`, `avg_runtime_invr` и weight. В исследованном коде сначала вычисляются `log2(wait_factor*wake_factor)` и `log2(runtime_factor*weight_factor)`, их сумма возводится в квадрат. Комментарий про sqrt не совпадает с этой формулой; вывод строится по исполняемому коду.

`calc_weight_factor()` усиливает wakeup/synchronous wakeup, IRQ wakeup, некоторые kernel workers, affinitized/pinned tasks и lock holders. В `calc_lat_cri()` есть ограниченное наследование критичности от waker и wakee; [`main.bpf.c`](https://github.com/sched-ext/scx/blob/c8728c6b6a3fde451f0f10b95f99aad7a32a750a/scheds/rust/scx_lavd/src/bpf/main.bpf.c#L1617) передаёт эти значения при пробуждении. Следовательно, механизм обслуживает зависимости, а не просто повышает nice одного exe. Это может помочь CPU producer-consumer цепочке без ускорения GPU shader.

В GFG нельзя выводить «поднять приоритет Gamescope» из этого описания: игра, Steam Input и драйверные workers могут находиться раньше в критическом пути. Общий boost compositor способен отнять у них CPU. Предлагаемое первое изменение — **телеметрия причин задержки**, затем проверка существующего scheduler; новый универсальный priority writer пока не предлагается.

Гипотеза пользы: если лишнее CPU-ожидание сдвигает готовый кадр за cutoff, небольшой выигрыш может сохранить целое обновление дисплея. Для 90 Hz период около 11,11 мс. Это условное пересечение deadline, а не обещание, что каждое сокращение CPU wait на 1 мс уменьшает задержку на 11 мс.

Эксперимент: сопоставить `sched_waking/sched_wakeup` и `sched_switch` по TID с source commit, CPU submit и GPU timestamps. Рассчитать wakeup→run p50/p95/p99 критичных потоков; отдельно sleep-on-fence и runnable wait. На одном маршруте повторить A/B/A с неизменным TDP/profile, shader build и graphics settings. Не переключать scheduler или менять его параметры во время чистого shader A/B. Если FPS изменился вместе с частотами/энергией, это ещё не чистый scheduling gain.

## 2. CPU scheduler сам может участвовать в управлении производительностью

[`power.bpf.c::update_cpuperf_target`](https://github.com/sched-ext/scx/blob/c8728c6b6a3fde451f0f10b95f99aad7a32a750a/scheds/rust/scx_lavd/src/bpf/power.bpf.c#L807) берёт максимум recent и average utilization, переводит высокую загрузку в полный performance target и вызывает `scx_bpf_cpuperf_set()`. В ветке `no_freq_scaling` задаётся `SCX_CPUPERF_ONE`, а не пропускается вызов.

**CONFIRMED для upstream:** название `--no-freq-scaling` нельзя трактовать как «отдать управление SteamOS без вмешательства». При переносе идеи нужен точный source текущего binary и проверка влияния на конкретный cpufreq driver; вызов performance target сам по себе не доказывает фактическую частоту. Также `Opts::proc()` способен включить autopilot по умолчанию, даже если слово `--autopilot` отсутствует в argv.

Применение к GFG: в диагностике показывать scheduler/режим, cpufreq driver, EPP, requested/actual clocks и ownership существующих controls. Не добавлять поверх этого Power Split/GPU Clock. Признак эффективности — меньше энергии на одинаковое завершённое действие при сохранении real FPS, wakeup tails и input latency; меньше requested clock отдельно не является результатом.

## 3. Память: reclaim/compaction на критическом пути

[`assets/SDWEAK.sh`](https://github.com/Taskerer/SDWEAK/blob/8c32e86fff3c754ac4819524edf7fff4c4e04de3/assets/SDWEAK.sh) изменяет watermarks, swappiness, MGLRU, THP, compaction и writeback. Эти настройки действуют разными путями, поэтому один preset нельзя считать одним доказанным ускорителем.

- **THP:** более крупные mappings потенциально уменьшают TLB/page-table overhead у CPU-памяти игры. Но выделение/уплотнение памяти тоже имеет стоимость. `defrag=defer+madvise` допускает прямой reclaim/compaction для advised mappings; это не гарантия отсутствия allocation stalls. THP anonymous memory не равен GPU image tiling или гарантированному укрупнению страниц всех Vulkan allocations.
- **Watermarks/MGLRU:** меняют запас свободной памяти и выбор страниц для reclaim. Они могут сместить работу в background либо уменьшить задержку direct reclaim, но могут увеличить фоновую активность/занятую память. На системе без memory pressure выгода не предполагается автоматически.
- **ZRAM:** обмен CPU-времени компрессии на меньший расход памяти/дискового swap. `ram*2` задаёт логическую ёмкость устройства, не дополнительную физическую RAM и не немедленное выделение такого объёма. LZ4/Zstd сравниваются под реальным давлением памяти; освобождение памяти ради красивого счётчика не критерий.
- **Writeback:** thresholds и периоды могут менять длительность грязных writeback bursts. Подгрузка ресурсов игры и запись shader cache должны разбираться отдельно от steady render.

Эксперимент: измерять deltas `pgmajfault`, `allocstall*`, `compact_stall/success/fail`, `pswpin/out`, PSI memory/io, THP counters и per-process faults с frame-time timeline. Конкретное доказательство: уменьшился tail stalled-source кадров, в тех же местах упали reclaim/page-fault stalls, расход энергии не вырос. Само уменьшение swap usage или рост AnonHugePages недостаточны.

Проверять одну группу настроек за раз на тяжёлой игре, контроле без давления и загрузке/смене локации. Нельзя менять всю VM конфигурацию и приписывать разницу одной THP. Отменять при росте CPU compression cost, memory footprint, OOM или tails. Никаких автоматических глобальных sysctl trials из render thread.

## 4. I/O scheduling: конкуренция загрузки игры и фоновой записи

Скрипт выбирает Kyber на NVMe, BFQ на microSD, readahead 1024 KiB и меняет writeback throttling/очереди. Механизм — порядок и размер операций, задержка чтений при конкуренции и стоимость лишнего предчтения. Это может помогать asset streaming/загрузке и не менять shader time.

Нужны четыре контекста: NVMe vs microSD, streaming vs warmed scene. Сравнивать read latency tails, PSI I/O, major faults, длительность source stalls и block request trace, если доступен. Read-ahead 1 MiB может помогать последовательному чтению и тратить bandwidth на ненужные данные при случайном доступе; универсальный вывод неприемлем.

Скрипт выставляет block `iostats=0`. При таком режиме нулевые/необновляющиеся disk counters нельзя объявлять отсутствием I/O. `wbt_lat_usec=0` выключает соответствующее ограничение writeback, что не тождественно снижению задержки read в любой нагрузке. Для корректного эксперимента предусмотреть отдельную observability конфигурацию и измерить её overhead; не менять её между A и B.

## 5. Фоновые процессы: доказать конкуренцию, затем выбирать действие

Read-only 8,19-секундный срез сделан в Steam UI: среди лидеров Steam и webhelper, один DRM client webhelper показал около 16,35% graphics-engine busy-time за интервал. Это не игровой baseline и не основание завершать эти процессы. CPU проценты в этом срезе нормированы на одно логическое CPU. Было около 9,76 GiB MemAvailable и нулевое avg10 PSI memory; сам факт наличия RSS не доказывает нехватку RAM.

Подтверждено по cgroup: Steam и найденные webhelper вместе находятся в `steam-launcher.service`. `systemctl freeze` замораживает все процессы unit. Такой unit нельзя использовать как «заморозить лишние вкладки Steam». Найдены отдельные network/audio roles; comm или argv role также не доказывают отсутствие зависимостей игры. Не нужен автоматический killer по имени, UID или CPU threshold.

Предлагаемый модуль plugin: таблица приложений с CPU time, DRM client engine time, RSS/PSS coverage, I/O и подтверждённой ролью; действия только для явно выбранных независимых приложений. Сначала встроенная pause/stop download функция приложения, затем штатное закрытие с обработкой сохранения, либо freezer только для проверенного отдельного unit. Freezer сохраняет память и не отменяет уже отправленную GPU работу; нельзя объявлять его способом освободить RAM или немедленно очистить GPU очередь.

Для обратимого действия нужны `AppIdentity{unit,pid,starttime,exe}`, previous state, action receipt и независимый restore path при выходе игры/ошибке plugin. PID reuse проверяется непосредственно перед действием. Защищены игра/Proton/launcher dependency tree, Gamescope, Steam Input, звук, сеть, session/power/thermal services. Если приложение разделяет группу/зависимость с protected процессом, действие недоступно с явной причиной.

DRM statistics дедуплицируются по device + client ID, включая shared FDs между процессами; не суммировать одно GPU-время дважды. При отсутствии доступа/счётчика показывать unavailable. Проверка эффекта — одинаковая сцена с фоном A, выбранное действие B, восстановление A. Сравнивать CPU wakeup tails, реальный FPS, GPU очередь, frame-time tails и energy. Увеличение свободной памяти само по себе не результат.

## 6. Что не переносится в GFG как «оптимизация»

SDWEAK installer также меняет package trust, устанавливает необязательные Gamescope/RADV binaries, может отключать mitigations, меняет polling и отключает часть диагностики. Это отдельные механизмы с отдельной ценой. Польза shader/runtime не должна зависеть от отключения защит, логов или watchdog. `perf_event_max_sample_rate=1` и stack limits особенно важны при планировании sampling profiler: такой baseline не подходит для обычного высокочастотного sampling без учёта ограничений. Vulkan timestamps этим sysctl напрямую не заменяются и не выключаются.

В опубликованном дереве есть бинарные packages и installer с checksum, но не найден source patch/build recipe, объясняющий их frame-time fix. Один `.PKGINFO` устанавливает имя/version/build metadata, не содержание изменения. До получения точного patch «повторить frametime fix» — **BLOCKED по происхождению кода**, а не признание бесполезности идеи.

Не переносить весь installer в plugin и не запускать uninstall как A/B rollback: uninstall устанавливает конкретные пакеты SteamOS 3.8 и не восстанавливает полный исходный snapshot произвольной новой системы. Механизмы исследуются по одному, без замены рабочего стека.

## Наблюдение устройства — дополнительный контекст, не цель исследования

Runtime SDWEAK.sh и installer на устройстве совпали по Git blob SHA с указанным SDWEAK commit; sdweak oneshot выполнился с summary `OK=68 FAIL=0 SKIP=0`. Активен `scx_lavd` с downstream flags `--pinned-slice-us 500 --dd-max-wait-us 0`. Global CPU EPP — balance_performance; ZRAM — zstd, около половины RAM; GPU module options file отсутствует; установлены более новые Mesa/Gamescope, чем вложенные SDWEAK packages. Это смешанное состояние не описывается одной галочкой «SDWEAK on/off» и не позволяет атрибутировать +FPS.

## Очередность доказательств

1. **CPU wakeup/dependency chain**: связать source stalls с runnable wait; проверить scheduler влияние, отдельно от shader и clocks. Наиболее прямое объяснение возможного FPS/tail улучшения при неизменном GPU алгоритме.
2. **Фоновые конкуренты**: найти реальную конкуренцию во время игры; обратимый A/B только независимого приложения. Не повторять грубую политику «убить всё кроме игры».
3. **Memory/reclaim и asset I/O**: выбирать эксперимент по наблюдаемым stall counters. На warmed GPU-bound сцене сначала проверять, есть ли вообще проблема этих слоёв.
4. Сохранить одинаковое окружение и перейти к shader proposals из [12](12-RELEASE-1.4.1-OPTIMIZATION-AUDIT.md); смешанный эксперимент делает вывод недостоверным.

Для всех этапов обязательны real FPS, temporal/image quality, presentation tails, external input latency и энергия из [13](13-RELEASE-1.4.1-VALIDATION.md). Пока нет причинного A/B — эффект **HYPOTHESIS**, даже когда сам механизм подтверждён кодом. Снижение p99 без роста average FPS также полезно, но должно называться именно снижением p99.
