# 13. Проверка новых предложений против официального v1.4.1

## Что выполнено

Исходники 24 файлов зафиксированы на `5f22938b55f9853f764e927c847d9c1a8d9a8c04`; Git blob SHA локальных копий проверены. Последний официальный stable — v1.4.1. Официальный ZIP: 19 491 058 bytes, SHA256 `53c6fa32507b18b004c7f18350a2acecb4213a830ff360e832d3c9e3b383227c`. Бинарник установленного GFG runtime не подменялся и не считается автоматически идентичным ZIP.

| Проверка | Результат | Ограничение |
|---|---|---|
| Predictor winner/origin/cost, CPU finite-input model | 32 187 случаев, совпадение | Не исполняет весь shader, texture filtering и ACO |
| OOB selection против exhaustive reference | 50 000 случаев, совпадение; отдельный release counterexample | Не измеряет PSNR и артефакты игры |
| GLSL baseline и proposal | Все 4 shaders скомпилированы на Deck | Нет GPU dispatch |
| SPIR-V validation | Все 4 прошли `spirv-val --target-env vulkan1.1 --scalar-block-layout` | Не Vulkan runtime validation и не native ISA validation |
| RADV capabilities | Очереди, timestamps, subgroup control проверены `vulkaninfo` | Возможность не равна выигрышу |
| Live status | 31 свежий sample, около 30 секунд | Наблюдение установленной версии, без A/B |
| GPU timing A/B, pixel readback, energy A/B, input latency A/B | **Не выполнено** | Игра активна; конкурентный benchmark испортил бы измерение и игровой процесс |

В live отрезке: 885 sources, 801 REAL, 1862 GEN, 4 REPEAT, 2667 flips; ring_miss и over_budget не увеличились, resync +1. Это deltas cumulative counters за 30,06 секунды. Поля `gpu_analysis_us_last`, опрошенные раз в секунду, лежали в 2,6534–9,8105 мс. Это не min/max всех analysis jobs и не p95. Лимит мощности менялся между 7/9/11 Вт; причина изменений этим сборщиком не устанавливалась. Следовательно, отрезок непригоден для заключения о цене shader при постоянных условиях. Среднее hwmon `power1_average` около 9,77 Вт не равно общей мощности системы и не доказывает экономию.

## Воспроизведение CPU checks

В отдельном checkout baseline выполнить:

```text
python docs/kedavra/research/hardware-native-runtime/evidence/release-audit-1.4.1/prove_candidates.py --source-root /absolute/path/to/baseline-checkout
```

Скрипт создаёт proposals и JSON рядом с собой. Он не применяет patches к baseline. Фиксирован seed `14120261025`, есть exhaustive duplicate/tie patterns, отдельное правило origin последнего predictor и randomized visibility/residual cases. Метрика «число SAD calls» — аналитический счётчик модели, не GPU benchmark. Для проверки самих patches применить каждый только к отдельной копии baseline и сравнить полученный shader SHA256 с `candidate-proof.json`.

## Изолированный GPU стенд

1. Завершить игру и дождаться температурного равновесия. Не запускать второй GPU workload поверх измеряемой игры. Не устанавливать новый runtime ради standalone теста.
2. Создать отдельный каталог в домашнем research workspace. Скачать официальный ZIP по URL релиза; проверить полный SHA256, размер и CRC всех ZIP entries. Извлечь только bench и необходимые bundled libraries в этот каталог. Не запускать installer, не писать в `/var/lib/kedavra` или plugin installation.
3. Использовать один и тот же официальный harness binary для A и B. Сверить его происхождение с ZIP; проверить `--spv` path. Installed bench разрешён только после сравнения hash с официальным, иначе это иной baseline.
4. Создать A и B каталоги SPIR-V. Все неизменённые shaders побайтово одинаковы; A — исходники официального SHA, B — ровно один patch. Компилировать одним glslang, одинаковыми options. Для исследования cache A и B warp одинаков; для OOB A и B ME одинаков. Combined вариант — отдельный этап.
5. Убедиться по `device`/`driver` JSON, что выбран именно RADV VANGOGH, а не lavapipe; сохранить UUID/deviceID, driverInfo, tool versions и hashes. Не форсировать семейство GPU через переменные старых версий Mesa.
6. GPU timestamp checks отделить от CPU wall. Сохранить baseline `kfg_vktest --spv A --size 1280x800 --bench 128 --json A.json`, вариант B и повтор A; повторить минимум 5 блоков. Harness benchmark включает одну пару анализа и два outputs ×3: читать имена pass, а не объявлять всю сумму «стоимостью warp». Warm-up не включать в результат.
7. Штатные scene/sequence checks запускать дополнительно: `--scene` пропускает часть sequence/stairs checks. Набор включает static, pan, fast pan, dolly/stairs, objects/HUD, climb, lowbase и цветовые SDR/PQ/scRGB проверки. Сохранять все outcomes, не выбирать только благоприятный pan.
8. Cache требует побайтового сравнения flow L2/L3, pair selection, generated outputs для одинакового input. Если текущий harness не экспортирует все images, дополнить только исследовательский host readback этих ролей. Наличие PSNR не заменяет equality. Итоговая картинка может совпасть и при разных внутренних vectors.
9. OOB patch требует отдельного GPU fixture: p.x=2, phase=.5, mixed residuals .19/.01, третий distinct vector, известные A/B images и ground truth. Проверить выбор и конечный pixel/полосу границы. Далее полный corpus; не требовать byte equality с ошибочным release для исправляемого случая.
10. Проверить validation errors, host memory safety и timeout recovery. Standalone failure прекращает эксперимент; возврат — закрыть тестовый процесс. Установленный compositor не изменён.

## Измерение очереди и presentation

Существующих status полей недостаточно. В исследовательском host bridge сохранять per job: pair_id/epoch, source_commit_id, CPU record begin/end, submit enter/return, GPU first/last timestamps, completion observation, target deadline и KMS token/flip. Структура `GpuJobTrace` и отдельная bounded очередь событий; completion stale epoch не вправе менять переиспользованный ring slot.

Сопоставлять CPU/GPU часы через calibrated timestamps с учётом `maxDeviation`. CPU submit timestamp и GPU TOP timestamp не дают «чистую очередь» без погрешности, dependency waits и driver overhead. Tracepoints DRM/AMDGPU использовать только если доступны в данной kernel build; отсутствие permission/tracepoint записать как unavailable. Не выдавать разность двух несопоставимых часов за scheduling delay.

Меры: CPU submission duration, GPU region duration по pass, submit→first с bounds, dependency completion→start при доступной трассе, ready→flip, flip intervals, source cadence, repeated image/content steps, scheduler delay/resync. Внутренний timestamp interval может содержать GPU-side stalls. Отдельно сохранять полный composite duration, который не входит в `gpu_us_avg` KFG.

Не уменьшать admission reserve или scheduler delay только на основании mean ME: это может улучшить среднюю задержку ценой поздних p99 кадров. Сначала completion correctness и верхние квантили. DirectScan измерять отдельно; copy proxy не заменяет реальный atomic path.

## Игровое A/B/A и обязательные критерии

Режимы: 45→90 ×2, 30→90 ×3, 30→60 ×2, 24→90. Сценарии: неподвижный HUD, медленный/быстрый поворот, тонкие линии/ступени, disocclusion, загрузка, Steam overlay/уведомления, HDR/SDR, разрешение, suspend/resume. Не менять игру, graphics preset, refresh, brightness, governor profile, fan/power policy и background workload между A/B/A. Сначала фиксированный **одинаковый существующий безопасный профиль**, затем отдельный A/B с автоматическим Governor. Пользовательские лимиты не повышать.

Прогрев минимум 10 минут; затем 5 A/B/A блоков по 3 минуты одной воспроизводимой сцены/маршрута. Протяжённая проверка — не менее 30 минут на вариант. Порядок вариантов чередовать между блоками, сохранять все блоки. Не смешивать прогрев/загрузку с steady-state; transitions сравнивать отдельно. Анализировать paired differences по блокам, а не считать коррелированные миллионы кадров независимыми наблюдениями.

| Свойство | Условие принятия |
|---|---|
| GPU cost | Положительная воспроизводимая экономия у целевых pass; нет p95/p99 regression всего frame и игры |
| Качество cache | Полная equality на конечных pixels и проверенных промежуточных fields |
| Качество OOB | Известный fixture исправлен; spatial/temporal error, worst frames и HUD corpus не ухудшились; отдельный visual review |
| Presentation | Нет роста repeat/late/resync, content-step jitter и пропущенных обновлений при сохранении real FPS |
| Input latency | Нет систематического роста; сохранять распределение, median/p95/p99 и uncertainty |
| Energy | Нет систематического роста мощности и энергии на одинаковый сценарий; cap сам по себе не критерий |
| Stability | Нет validation/device-lost, ошибочного lifetime, зависания compositor и поломанного fallback |

Input-to-photon измеряется внешним методом: камера высокой частоты с видимым физическим input/LED либо фотодиод/аппаратный trigger, одинаковая фаза refresh, множество событий (целиться минимум в 1000 на вариант). Игровые counters и scheduler simulation не доказывают отсутствие роста input latency. При отсутствии измерительного метода это требование остаётся **непроверенным**.

Энергия: синхронно сохранять hwmon power, cap, частоты, temp, battery/charger state. Для общей энергии использовать достоверный battery energy counter в контролируемом unplugged сценарии либо внешний измеритель на одинаковых условиях; определить границу измерения. Интегрировать мощность по времени, сравнивать одинаковую длительность и полезный игровой progress. Экономию shader GPU time не объявлять автоматически экономией APU power.

Для строгого утверждения «не увеличивается» заранее задать правило uncertainty; например односторонняя 95% граница paired delta должна быть ≤0. Если шум допускает и рост, и снижение, вывод — **не установлено**, а не «нулевая разница». Допустимую ненулевую погрешность нельзя молча объявлять разрешённой прибавкой latency/ватт.

## Plugin, ошибки и откат будущей сборки

В пользовательской сборке установка, baseline/variant selection, настройки, диагностика и экспорт доступны из GFG plugin. SSH здесь — инструмент исследования. Не требовать от пользователя вручную менять cvars/файлы для штатной проверки.

Нужны runtime status `requested_variant`, `active_variant`, shader/build hash, subgroup size, capability result и `fallback_reason`. Backend подтверждает фактический active вариант; успех UI toggle не считается подтверждением engine state. Pipeline creation failure немедленно возвращает baseline, не пустой/устаревший output. Горячая замена не освобождает in-flight pipeline/images; смену build безопаснее отложить до выхода из игры.

Постоянный rotating JSONL error log содержит timestamp/clock domain, build, variant, epoch/pair ID, stage, VkResult, code/reason и счётчик повторов ошибки. Минимальные коды: `variant_unsupported`, `pipeline_create_failed`, `shader_hash_mismatch`, `query_unavailable`, `job_deadline_missed`, `stale_completion`, `device_lost`, `fallback_activated`. Ошибка логируется сразу с rate limiting одинаковых сообщений, а не только при открытом UI. Timing samples идут в отдельный ограниченный trace, без flush/fsync на каждом кадре.

В plugin: «Состояние», «Последняя ошибка», «Вернуться к baseline», «Записать диагностический прогон», «Экспортировать отчёт». ZIP содержит runtime/build manifests, capabilities, выбранные настройки, counters, trace и error logs; private SSH material отсутствует. До завершения этого пути experimental shader не выдаётся за готовую пользовательскую функцию. Текущие proposals — файлы для стенда, а не установленная функция с готовыми controls.

Решение о внедрении: если измеримая экономия отсутствует, добавляется input/energy cost, качество регрессирует или fallback ненадёжен — остановить вариант. Улучшение synthetic benchmark не отменяет эти ограничения.
