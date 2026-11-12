# 09. Feasibility matrix и рейтинг

Оценки качественные и условные. «Высокая реализуемость» относится к объёму software integration, а не к подтверждённой аппаратной пользе. GPU ms, watts и end-to-end latency новых вариантов **Не измерено**. Не складывать benefit A1+A2 и B1+B4: это общие части одного маршрута.

| Ранг / идея | Feasibility на OLED | GPU Benefit | Power Benefit | Pacing Benefit | Complexity | Risk | Evidence |
|---|---|---|---|---|---|---|---|
| 1. A1+A2 DirectScan | Средняя; format/modifier/color/planes gates | Минус один full-frame composite на eligible кадр; ms Не измерено | Не измерено; меньше shader read/write, но DCN/tiling cost учитывать | Не измерено; короче final path | Высокая | Средний/высокий: lifetime, UI/HDR | CONFIRMED forced composite/allocator; SUPPORTED маршрут; HYPOTHESIS net benefit |
| 2. B4+B1 actual completion + admission | Высокая для one-queue версии | Не измерено; убирает backlog/необязательный late analysis | Не измерено; избегает бесполезной late work | Прямая цель: честная readiness и защита cutoff | Средняя | Средний: conservative age/starvation | CONFIRMED +2 bridge/timeline; HYPOTHESIS improvement |
| 3. C1 post final destination | Высокая | Минус одна copy на source с post; ms Не измерено | Не измерено; меньше logical bytes | Не измерено; меньше работы на source paint | Низкая/средняя | Низкий/средний: alias/filter parity | CONFIRMED copy; HYPOTHESIS measured benefit |
| 4. D deadline advisor | Средняя после B4 | Не сокращает shader count; ожидания можно лучше классифицировать | Не измерено; главная цель | Guard от power trials, портящих deadline | Средняя/высокая, два repo | Средний: control feedback | CONFIRMED existing lease/actuators; SUPPORTED telemetry; HYPOTHESIS controller |
| 5. R1 per-shader wave choice | Средняя/высокая при capability | Не измерено; возможны VGPR/occupancy gains | Не измерено | Не измерено; tails shader duration | Средняя | Низкий/средний: driver/variant cache | SUPPORTED subgroup API, HYPOTHESIS |
| 6. B2 same-family second queue | Средняя, queueCount gate | Не измерено; overlap может как помочь, так и мешать | Не измерено; contention может увеличить расход | Не измерено | Высокая: per-queue resource retirement | Высокий: races и starvation | SUPPORTED compute queues; HYPOTHESIS useful overlap |
| 7. R2 subgroup stats reduction | Средняя | Не измерено; меньше barriers/LDS traffic | Не измерено | Не измерено; часть analysis tail | Средняя | Средний: float/cut parity | CONFIRMED shared tree, SUPPORTED subgroup ops conditional |
| 8. C3 copy+reduce fusion | Средняя/низкая | Минус reread/dispatch, но multi-target overhead | Не измерено | Не измерено | Высокая: descriptor ABI | Средний/высокий: format/HDR parity | HYPOTHESIS |
| 9. R3 LDS luma tile | Средняя | Не измерено; texture cache уже может справляться | Не измерено | Не измерено | Средняя | Средний: barriers/LDS/occupancy | HYPOTHESIS |
| 10. B3 split priorities across families | Средняя/низкая | Shader work не уменьшается | Не измерено; preemption может стоить энергии | Не измерено; protected final vs game starvation | Высокая | Высокий | SUPPORTED contexts; HYPOTHESIS net benefit |
| 11. SDMA для оставшейся exact copy | Низкая/средняя, driver gate | Не измерено, shader work может уйти на другой engine | Не измерено; bandwidth остаётся | Не измерено; новые fences | Средняя/высокая | Средний | SUPPORTED hardware/API conditional, HYPOTHESIS |
| 12. Borrow game buffer вместо history copy | Низкая как общий путь | Copy исчезает, game acquire может stall | Не измерено | Может ухудшить real FPS | Высокая | Высокий: swapchain backpressure | REJECTED универсально; limited experiment в [04](04-MEMORY-ZERO-COPY.md) |

## Первые три задания разработчику

### A. DirectScan

Что: сохранить motion/warp, изменить destination и presentation routing. Где: `kfg.cpp/kfg.hpp/hooks.patch`, Gamescope renderer output lease и DRM Present. Почему: исчезает доказуемый последний read+write pass, если условия соблюдены. Как доказать: E1/E2/E3, command trace без промежуточного pass + A/B/A ms/Wh + colour/UI/Mura parity. Исходное и предложенное владение, synchronization и stop criteria — [02](02-DIRECTSCAN.md).

Предварительная implementation task: prepare-once API на forced-composite пути; allocator capability matrix; затем eligible scanout. Нельзя сначала удалить WantsComposite и искать причину отсутствия generation.

### B. Completion + deadline admission

Что: per-pair timeline completion вместо фиксированного +2; анализ небольшими admission-controlled chunks. Где: `OnPaint/SubmitAnalysis/ResetHistory/RecordGenerate`, scheduler bridge, stage markers. Почему: доступная пара определяется actual GPU state, late queued analysis не удерживает final frame без необходимости. Как доказать: E4 с job IDs/epoch, submit-start decomposition и real FPS/content age. Это prerequisite корректной GPU feedback Governor. Подробности — [03](03-GPU-SCHEDULING.md).

### C. Post → history destination

Что: только последняя destination plan направляется в history, отдельный RecordCopy удаляется. Где: `RecordPost` и optional final-output API `kfg_post.h`. Почему: тот же final pixel пишется один раз. Как доказать: E7 exact/quality parity, один исчезнувший dispatch, unchanged controls и source FPS. Подробности — [04](04-MEMORY-ZERO-COPY.md).

## Явно отклонённые/заблокированные решения

| Идея | Статус | Конкретная причина / выход |
|---|---|---|
| Просто убрать WantsComposite | REJECTED | RecordComposite перестанет вызываться; нужен prepare stage |
| Передать текущий pOutput в atomic commit | BLOCKED в текущем коде | Нет flippable/export/backend FB; решить allocation+lifetime |
| Один extra full-frame copy назвать zero-copy DirectScan | REJECTED как доказательство | Не сокращает pass count; требуется net measurement |
| DirectScan при невоспроизводимой HDR/Mura color pipeline | BLOCKED для конфигурации | Обязательная композиция; не обходить color correction |
| Low и realtime queues одной family в одном VkDevice | BLOCKED спецификацией | VUID 06654/02802; same priority либо разные families |
| MES deadline control на VANGOGH | BLOCKED в reference driver route | Нет GFX10.3.1 MES discovery; не переключать firmware |
| Гарантированный vblank от global priority | REJECTED | Priority не hard realtime reservation/preemption bound |
| Timeline opaque FD прямо в IN_FENCE_FD | REJECTED | KMS ожидает sync_file, нужен конкретный fence export/bridge |
| Убрать CPU wait, оставив always-signaled fence | REJECTED | KMS может читать ещё записываемый image |
| Уменьшить bit depth/resolution/search/выключить Mura ради результата | REJECTED | Нарушает качество/условия сравнения |
| Частотный boost на каждый vblank / unsafe SMU | REJECTED | Нет доказанного timing control, конфликт контуров и запреты ТЗ |
| Kernel patch без установленного user-space ограничения | REJECTED | Все первые проекты используют существующие интерфейсы |
| Полный source zero-copy с удержанием всех game images | REJECTED как default | Может исчерпать swapchain, ухудшая real FPS |
| R2 global fusion через один workgroup barrier для всей картинки | REJECTED | Нет меж-workgroup sync внутри обычного dispatch |
| «lavapipe стало быстрее → Deck станет быстрее» | REJECTED | Другой исполнитель; software quality и hardware cost различны |

## Как повышать Evidence

SUPPORTED→CONFIRMED integration требует capability report, идентичность фактического binary/source, correctness stress и реальные трассы Deck. HYPOTHESIS benefit→CONFIRMED benefit требует повторяемого A/B/A с raw inputs и без ухудшения полезной плавности. Отсутствующая telemetry не превращается в нулевые costs. Отрицательный эксперимент сохраняется вместе с положительным.
