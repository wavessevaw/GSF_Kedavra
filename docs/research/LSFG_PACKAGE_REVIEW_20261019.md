# Lossless Scaling 3.2.2: статический разбор переданного пакета

Дата: 2026-10-19. Основание: пользовательский архив `Lossless Scaling.tar.gz`.
Это исследование конкретного экземпляра Windows-приложения, а не результат запуска
LSFG на Deck. DLL/EXE, шейдеры, модели и содержимое ресурсов в репозиторий не входят.

## Что теперь проверено

SHA256 архива: `9a7c384fc39fad9b782165fc09f005b0d3daad4589b3b4de120e3ab40b32cf84`.
В нём 482 tar-записи; большая часть состава — .NET/Windows runtime и локализации.
Исходников проекта и отдельных HLSL/GLSL/SPIR-V/ONNX-файлов не обнаружено.
.NET runtime 9.0.5 не следует путать с версией приложения.

| Файл | Размер, байт | SHA256 |
| --- | ---: | --- |
| `Lossless.dll` | 7521280 | `626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb` |
| `LosslessScaling.dll` | 166912 | `e4ea2dbb1371ea1920d73c87202f19b6f095ef6f521a8a7a139d19f34f1fe866` |
| `LosslessScaling.exe` | 157696 | `cf02e1cad47e220b6fa24d05689356d70173c86be10e3e213e8e90e859c3a542` |
| `config.ini` | 373 | `6623eb769669d3fc46ad3ffdc3f3ea417eb7b1c6b66c4b577654f3eb28491496` |

PE version information native DLL: **3.2.2.0**. Frontend assembly/PE также сообщает
3.2.2.0. Все три PE — AMD64. Надписи LSFG 1.1/2.3/3.1 в интерфейсе обозначают
варианты; они не устанавливают, какой алгоритм был активен у пользователя.

Native DLL содержит 302 ресурса: 300 RT_RCDATA, version и manifest. Каждый из 300
RT_RCDATA начинается с заголовка соответствующего контейнера:

| Формат | Ресурсов | Разных SHA256 | Статическая проверка |
| --- | ---: | ---: | --- |
| SPIR-V | 98 | 95 | Версия 1.0; 98 entry points с execution model 5 (GLCompute); границы инструкций согласованы |
| DXBC | 202 | 202 | Границы контейнеров/chunks согласованы; compute shader model 5.0 |

Это не просто поиск magic bytes. Но это также **не** `spirv-val`, проверка binding
layout, восстановление вычислительного графа или доказательство совместимости с
существующим MAKO adapter. Ресурсы могут принадлежать разным алгоритмам и вариантам
масштабирования; называть все 300 «шейдерами выбранной LSFG» оснований нет.

[Машиночитаемые метаданные](LSFG_PACKAGE_20261019.metadata.json) содержат ID, размеры,
хеши ресурсов, imports/exports и результаты структурной проверки, без их байтов.
Исторический [LSFG_BINARY_ANALYSIS.md](../archive/native-fg-20261015/research/LSFG_BINARY_ANALYSIS.md)
остается снимком прежнего состояния: его отсутствие конкретной DLL теперь закрыто
в части инвентаризации, но runtime/API trace и reference-oracle run остаются открыты.

## Частота compositor: подтвержденный небольшой участок кода

`GetDwmRefreshRate` экспортирован по RVA `0x2ba80`. В диапазоне RVA
`0x2ba80..0x2baf3` статически установлено:

1. Создается и обнуляется структура размером `0x124`, заполняется `cbSize`.
2. Вызов через IAT VA `0x180038810` разрешается именно в
   `dwmapi.dll!DwmGetCompositionTimingInfo`; HWND передается нулевой.
3. После вызова функция берет два 32-битных значения из полей структуры по
   смещениям 4 и 8, преобразует в float и возвращает их отношение.

Это соответствует `DWM_TIMING_INFO.rateRefresh.uiNumerator/uiDenominator`:
[документация Microsoft](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ns-dwmapi-dwm_timing_info).
В этом небольшом экспорте не видно ветвления по HRESULT перед делением; его нельзя
копировать как образец безопасной обработки ошибок. Managed P/Invoke объявляет
`GetDwmRefreshRate` как возвращающий float. Когда и для какого monitor/capture
сценария вызывается экспорт, полным call graph здесь не установлено.

**Вывод для GSF:** это конкретное подтверждение отдельного получения refresh от
compositor, а не основание подставить 90 Гц в наш scheduler. В Ghost of Tsushima
SDR-архиве (7) GSF сообщает 16 666 666 нс во всех 5768 pair-записях, тогда как
3242 из 5782 положительных последовательных gaps compositor actual-time близки
к 11.111 мс, и ни один — к 16.667 мс (допуск 0.2 мс). Это рассогласование источников,
не доказательство физического scanout.

На `main` e14f169 лог `refresh_source=WSI-query` основан на наличии указателя,
а не на успешном результате запроса. В `gpu_bridge.c` период меняется лишь при
`VK_SUCCESS` и допустимом значении; ошибка оставляет прежний период. Поэтому для
разделения причин нужны результат `vkGetRefreshCycleDurationGOOGLE`, сырое
`refreshDuration`, примененное значение и происхождение текущего периода
(override, успешный запрос, сохраненное значение или fallback). Это отдельная
диагностическая задача без изменения `gfg_frame_clock_pair` или формулы admission.

## Буфер захвата, очередь GPU и вывод — разные вещи

В переданном `config.ini` найдены:

| Группа | Ключи и переданные значения | Что это действительно устанавливает |
| --- | --- | --- |
| capture | `frametime_buffer_size=15`, `frametime_buffer_reset_multiplier=6`, `queue_draining_momentum=0.01` | Имена параметров истории времени/очереди присутствуют и в native DLL; единицы и формулы не восстановлены |
| lsfg | `real_timestamp_tolerance=0.05`, `base_framerate_threshold=10` | Значения конкретного файла, не доказанные ограничения или рекомендуемые FPS |
| rendering | `flush=1` | Конфигурационный флаг; конкретный API call и его расположение не установлены |
| fps_counter | `show_captured=1` | Интерфейс различает captured и output frames/s |

В frontend metadata есть `QueueTarget`, `MaxFrameLatency`, `CaptureApi`, `SyncMode`.
Встроенные описания UI различают buffered capture с target queue 1 и 2; второй
вариант описан как более терпимый к нестабильному входу/нагрузке, с ценой в latency.
Это заявленное поведение интерфейса, а не измерение и не восстановленный алгоритм.
Native строки описывают удаление старого кадра при полной очереди и пропуски при
заполненных буферах. RTTI/строки содержат DDCapture, WGC, ThreadsafeQueue и
Direct3D11CaptureFramePool. Imports подтверждают DXGI, DWM, WinRT, Win32 window API,
QPC, waitable timers, thread/event primitives. Наличие этих примитивов не доказывает
ни per-frame блокировки, ни полностью асинхронный конвейер.

**Практический вывод:** продукт явно предусматривает регулирование очереди
захвата и устаревших входов. Это не счетчик незавершенных game-submit на GPU и
не правило «добавить GSF буфер на два кадра». Положительный API submit upper bound
из [PR #38](https://github.com/wavessevaw/GSF/pull/38) сам по себе также не является
доказательством hardware queue depth или времени ожидания TE.

Понижение частоты TE dispatch в 0.1.7 не устранило задержку на Deck, а данный архив
не содержит Deck trace. Нельзя приписывать наш after-submit delay одному конкретному
LSFG параметру, объявлять отдельную compute queue готовым решением или переносить
`base_framerate_threshold=10` в admission. Раздельные counters для captured/native,
computed и WSI-accepted полезны; ни один из них отдельно не доказывает показ 2×.

## Что использовать дальше

Приоритет — установить происхождение 60/90 Гц и совместить его с уже подготовленной
наблюдаемостью app-submit/admission, а не менять одновременно очередь и шейдеры.
Затем возможен контролируемый опыт с bounded capture/history и сбросом устаревших
входов, если наши замеры покажут накопление именно там. Windows API не переносится
на SteamOS буквально. Семантика LSFG capture и Vulkan-injection GSF различается.

Конкретный resource inventory теперь позволяет заранее проверить совместимость
reference adapter с этой DLL и сравнивать одинаковые input pairs. Сам oracle здесь
не запускался; качество, GPU cost и реальный variable-rate 2× этой DLL на Deck не
измерены. Lossless Scaling не добавляется как runtime dependency GSF.

## Воспроизведение

В отдельном окружении анализа установить `pefile`; в GSF runtime он не требуется.
Скрипт читает четыре выбранных tar member в память без распаковки путей,
не загружает PE в процесс и не записывает байты шейдеров:

```bash
python tools/research/inspect_lossless_package.py '/absolute/path/Lossless Scaling.tar.gz' > metadata.json
```

Для проверки небольшого refresh export по той же DLL:

```bash
objdump -d --start-address=0x18002ba80 --stop-address=0x18002bb00 /absolute/path/Lossless.dll
```

Локально: повторное получение metadata полностью совпало; все 300 контейнеров
прошли указанные проверки границ, отдельно сверены counts, unique hashes и stages.
Windows-программа, DLL и GPU shaders не исполнялись. Runtime/release GSF не изменены.
