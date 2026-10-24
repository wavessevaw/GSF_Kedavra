# Сравнение Hardware Runtime с Kedavra 1.4.1 вне Steam Deck

Baseline: `5f22938b55f9853f764e927c847d9c1a8d9a8c04`. Candidate: `f965e9fe91209232789668e4beb86f6fb1cfc1a3`.

Ubuntu GitHub Actions, software Vulkan lavapipe/llvmpipe, два worker threads; один host, последовательные прогоны. Это измеренные времена этой CPU Vulkan-среды, не AMD GPU и не прогноз Steam Deck.

5 блоков A1/B0/B/A2 на размер; 9 warm-up и 30 измерений на case/block. A1/A2 — исходная 1.4.1, B0 — новый engine с legacy path, B — новый engine с оптимизированным path. Baseline блока = среднее медиан A1/A2; итог — медиана блоков. В CSV сохранены все samples.

Vulkan elapsed — timestamp от начала первого dispatch до конца последнего, включая промежуточные barriers. Wall — CPU begin/record/submit/wait; uploads/readbacks, allocations и pixel hashing исключены из окна. CPU record и submit/wait сохранены раздельно. Никакой параллельной нагрузки из нашего job нет; host общий для CI.

## Измеренные времена

Положительная экономия означает ускорение. 95% bootstrap interval строится по пяти блокам (разведочная оценка, не аппаратная гарантия). Дрейф A2−A1 и negative controls помогают видеть шум; изменившийся контроль не объявляется оптимизацией.

| Размер | Case | 1.4.1, ms | Новый, ms | Экономия, ms | % | 95% interval экономии, ms | B0−B, ms | A2−A1, ms |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 320x200 | analysis | 4.103 | 4.133 | -0.015 | -0.4% | [-0.633; +0.099] | +0.117 | -0.167 |
| 320x200 | warp_x2 | 0.821 | 0.806 | +0.008 | +1.0% | [-0.007; +0.020] | +0.019 | -0.004 |
| 320x200 | warp_x3 | 1.036 | 1.042 | -0.007 | -0.7% | [-0.012; +0.023] | +0.004 | +0.001 |
| 320x200 | sharp | 2.239 | 1.979 | +0.267 | +11.9% | [-0.578; +0.286] | +0.271 | -0.019 |
| 320x200 | vivid | 2.657 | 2.361 | +0.283 | +10.6% | [+0.264; +0.660] | +0.257 | -0.001 |
| 320x200 | smooth | 5.545 | 5.085 | +0.421 | +7.6% | [-2.001; +1.375] | +0.467 | -0.086 |
| 320x200 | display_copy_proxy | 1.324 | 1.040 | +0.275 | +20.7% | [+0.272; +0.407] | +0.265 | +0.003 |
| 1280x800 | analysis | 35.746 | 35.713 | +0.210 | +0.6% | [-0.947; +4.236] | -0.043 | +0.003 |
| 1280x800 | warp_x2 | 12.185 | 12.323 | -0.225 | -1.9% | [-0.869; +0.152] | +0.013 | -0.131 |
| 1280x800 | warp_x3 | 15.950 | 16.078 | -0.227 | -1.4% | [-1.011; +0.173] | -0.013 | -0.027 |
| 1280x800 | sharp | 33.415 | 30.119 | +3.247 | +9.7% | [+2.391; +3.987] | +3.791 | -0.016 |
| 1280x800 | vivid | 38.781 | 35.387 | +3.250 | +8.4% | [+2.674; +4.330] | +3.539 | +0.120 |
| 1280x800 | smooth | 62.896 | 58.483 | +4.084 | +6.5% | [+3.189; +4.562] | +4.293 | +0.241 |
| 1280x800 | display_copy_proxy | 19.376 | 15.953 | +3.388 | +17.5% | [+2.333; +3.564] | +3.491 | -0.175 |

## Проверка результата и границы интерпретации

- 540 сравнений SHA256 readback-изображений: исходная 1.4.1 = новый legacy = новый optimized = повтор исходной. Проверяются RGBA целиком и три фазы каждого case/block/size, не только счётчики.
- Original quality suites normal/lowbase: обе версии PASS, JSON quality metrics идентичны; post suites обеих версий PASS.
- SPIR-V всех shaders, kfg_vk.c/.h, kfg_plan.h и kfg_sched.h идентичны. Analysis и warp без filters — negative controls, их изменение времени является шумом.
- C1 cases sharp/vivid/smooth: реальные production post plans + тот же compute kfg_copy, что RecordCopy, либо production kfg_plan_post_into. Снято ровно одно dispatch; resource formats одинаковы. Это не полный Gamescope frame benchmark.
- display_copy_proxy: warp + identity compute copy против warp. Этот проход является proxy переноса полного SDR изображения; он НЕ выполняет Gamescope BLIT/color management, импорт modifiers или DRM commit. Его экономию нельзя выдавать за измеренный выигрыш DirectScan.
- На 1280×800 RGBA8 удалённый copy имеет логический payload 4 096 000 B read + 4 096 000 B write = 8 192 000 B/frame. Это расчёт обращений изображения, не измерение DRAM bandwidth: cache/compression/tiling не учтены. Дополнительная память pool DirectScan здесь не моделируется.
- Старый scheduler matrix (5 scenarios × 3 seeds × 180 s) идентичен: он не моделирует новый completion adapter. Нет заявленного повышения FPS из изменения scheduler kernel.

## Контролируемые задержки completion

Production scheduler обеих версий получает одинаковые latch/finish traces. Legacy bridge сообщает latch+2; новый bridge сообщает наблюдаемое completion. Это воспроизводимый model counterexample корректности готовности, не GPU timing, не физически показанные кадры и не обещание более высокого FPS.

| × | Completion, vblanks | Premature GEN: 1.4.1 → новый | GEN decisions: 1.4.1 → новый | Repeat: 1.4.1 → новый |
|---|---:|---:|---:|---:|
| 2 | 1 | 0 → 0 | 5998 → 5999 | 3 → 2 |
| 2 | 2 | 0 → 0 | 5998 → 5998 | 3 → 3 |
| 2 | 3 | 5998 → 0 | 5998 → 372 | 3 → 5690 |
| 2 | 4 | 5998 → 0 | 5998 → 372 | 3 → 5752 |
| 2 | 5 | 5998 → 0 | 5998 → 311 | 3 → 5813 |
| 2 | 6 | 5998 → 0 | 5998 → 311 | 3 → 5875 |
| 2 | 7 | 5998 → 0 | 5998 → 250 | 3 → 5936 |
| 3 | 1 | 0 → 0 | 7997 → 7998 | 4 → 3 |
| 3 | 2 | 0 → 0 | 7997 → 7997 | 4 → 4 |
| 3 | 3 | 3999 → 0 | 7997 → 824 | 4 → 7235 |
| 3 | 4 | 7997 → 0 | 7997 → 708 | 4 → 7351 |
| 3 | 5 | 7997 → 0 | 7997 → 708 | 4 → 7351 |
| 3 | 6 | 7997 → 0 | 7997 → 708 | 4 → 7410 |
| 3 | 7 | 7997 → 0 | 7997 → 592 | 4 → 7526 |

## Воспроизведение

Workflow `.github/workflows/kedavra-compare.yml` закрепляет исходную 1.4.1 по SHA, собирает один harness против обеих версий с одинаковыми `cc -O2`, запускает `compare_versions.py`. Среда, hashes, CSV каждого блока, correctness JSON, completion CSV и исходный отчёт находятся рядом с этим файлом.

GPU submission-to-start delay, KMS pacing, физический scanout, энергопотребление и эффект deadline admission на реальную очередь этой средой не измеряются. Нулевой результат и регрессии сохраняются в таблице.


## Инженерная интерпретация

**CONFIRMED в указанной software Vulkan-среде:** C1 сокращает полную цепочку warp + filters + history при 1280×800. Экономия по парным блокам: Sharp 3,247 ms (9,7%), Vivid 3,250 ms (8,4%), Smooth 4,084 ms (6,5%). Все пять блоков каждого из этих трёх cases дают положительный эффект; bootstrap intervals также положительны. Проверка B0−B на одном новом engine подтверждает направление эффекта. Это сокращение времени цепочки, а не рост игрового FPS на те же проценты.

CPU wall подтверждает результат, отдельно от timestamp elapsed:

| Case, 1280×800 | 1.4.1 wall median, ms | Новый wall median, ms | Экономия по парным блокам, ms | Экономия времени |
|---|---:|---:|---:|---:|
| sharp | 33.583 | 30.219 | 3.276 | 9.8% |
| vivid | 38.964 | 35.569 | 3.285 | 8.4% |
| smooth | 63.097 | 58.672 | 4.125 | 6.5% |

Медиана парных разностей не обязана равняться разности двух отдельно вычисленных медиан; проценты рассчитаны из парного эффекта. В [машиночитаемом summary](evidence/non-deck-1.4.1-summary.json) сохранены пять исходных block effects, drift, интервалы и все метрики.

**DirectScan:** измерено только исключение identity-copy proxy (3,388 ms, 17,5% цепочки на этом CPU ICD). Это **не** результат запуска DirectScan: KMS, modifiers, Gamescope BLIT и display не входят в стенд. Использовать 17,5% как обещанный эффект DirectScan запрещает сама область измерения. Аппаратная реализуемость остаётся отдельным capability gate.

**Completion/deadline:** новая completion-привязка устраняет преждевременные GEN decisions в контролируемой трассе. Например, ×3 при completion=latch+5: 7 997 → 0 premature decisions на 12 000 vblanks. При этом GEN decisions уменьшаются 7 997 → 708, repeats растут 4 → 7 351: scheduler перестаёт обещать готовность неготовой пары и деградирует. Legacy GPU-зависимости могли превращать такие решения в ожидание; эта модель не доказывает показ некорректных пикселей старым runtime и не измеряет выгоду deadline admission по времени. Выигрыш здесь — корректность readiness-контракта, не увеличение FPS.

У analysis и warp без filters нет подтверждённого изменения вычислительной стоимости: исполняемый код и SPIR-V идентичны, intervals включают ноль. Отрицательные изменения сохранены в полной таблице; они не выданы за доказанную регрессию нового алгоритма. При 320×200 эффект Sharp/Smooth неотделим от шума этих пяти блоков, несмотря на положительные медианы.

## Происхождение и сохранённые данные

- [Успешный сравнительный CI, run 40084819888](https://github.com/wavessevaw/GSF_Kedavra/actions/runs/40084819888).
- [Архив исходных CSV, quality JSON, pixel hashes, source hashes, среды и полного отчёта](https://github.com/wavessevaw/GSF_Kedavra/actions/runs/40084819888/artifacts/12556967890) — 178 файлов; SHA256 внешнего ZIP: `c9255151fe974217d45d84ed0052633594bf62908d225fe60a0b3bd1ccb72f15`. Артефакт имеет срок хранения GitHub; постоянный summary сохранён коммитом рядом с этим документом.
- CPU host: AMD EPYC 9V45 VM, 4 vCPU; Mesa 25.2.8, LLVM 20.1.2, `LP_NUM_THREADS=2`; GCC 13.3.0 `-O2`. Полный manifest — в summary JSON. Это AMD CPU, не RDNA GPU.
- [Код benchmark harness](https://github.com/wavessevaw/GSF_Kedavra/blob/f965e9fe91209232789668e4beb86f6fb1cfc1a3/kedavra/tools/kfg_compare.c), [orchestrator/statistics](https://github.com/wavessevaw/GSF_Kedavra/blob/f965e9fe91209232789668e4beb86f6fb1cfc1a3/kedavra/tools/compare_versions.py), [completion model](https://github.com/wavessevaw/GSF_Kedavra/blob/f965e9fe91209232789668e4beb86f6fb1cfc1a3/kedavra/tools/kfg_compare_sched.c), [workflow](https://github.com/wavessevaw/GSF_Kedavra/blob/f965e9fe91209232789668e4beb86f6fb1cfc1a3/.github/workflows/kedavra-compare.yml).
- [Runtime Draft PR #79](https://github.com/wavessevaw/GSF_Kedavra/pull/79). Между runtime commit `6a449cdb96577de2b4e1ddb5ca7af18cbc804e92` и измеренным `f965e9fe91209232789668e4beb86f6fb1cfc1a3` изменены только стенд и workflow; runtime/shaders не менялись.

Ни игровой FPS, ни энергопотребление, ни аппаратная скорость DirectScan этими данными не установлены. Тесты вне Deck показывают полезный эффект C1 и readiness-контракт completion; следующие аппаратные проверки остаются в исходном [протоколе](08-EXPERIMENTS.md).
