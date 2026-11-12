# Witcher 3: архив (8), генерация заблокирована форматом

Архив: `GSF-Game-Latest(8).zip`, 22842 байта.
SHA256: `0f66a73d4ee98eae63026fb52085feb0b32c7c7e64370c8838fa94bf81865eaa`.
Run: `game-292030-20261019T033959Z-rt_184cl`, appid 292030,
`pApplicationName=witcher3.exe`. В логе есть DXVK и vkd3d instances; их конкретные
библиотечные SHA/Proton build не зафиксированы.

Установлен прежний тестовый GSF 0.1.7, source commit
`0ee3a9bdca2b9070bcf723d853b378168d0afe00`, normal, same-device LMC tier 2,
2×, async, pacing, calibrated timestamps и latency admission включены.
Это не сборка с новой queue/refresh диагностикой.

Зафиксированы **три** `GSF_REAL_WSI_GATE`:

| Создание | Формат | Color space | Результат GSF |
| --- | --- | --- | --- |
| 1 | A2B10G10R10_UNORM_PACK32, 64 | SRGB_NONLINEAR | candidate=0, packed10 guard |
| 2 | A2B10G10R10_UNORM_PACK32, 64 | SRGB_NONLINEAR | candidate=0, packed10 guard |
| 3 | A2B10G10R10_UNORM_PACK32, 64 | HDR10_ST2084 | candidate=0, packed10 guard |

У всех: original/effective present mode=2 (FIFO), requested minImageCount=3,
create_flags=0xc0, usage=0x1b. Значение `spare=2` в gate log не доказывает, что
изображения были добавлены: ниже Gamescope создаёт swapchain с minImageCount=3.

Одинаковая причина: `gamescope-packed10-physical-dispatch-unverified`.
REPORT: GENERATION_BLOCKED, max_generated_wsi_accepted=0,
timeline_diagnostic.available=false. `frames.jsonl` отсутствует в ZIP;
нет bridge-active/generation_started. Следовательно, здесь нельзя измерять
TE cost, очередь, 2× recovery или частоту clock GSF: генератор не был запущен.
Exit -2 означает SIGINT; сам по себе не доказывает GPU crash.

**Отключение HDR не гарантирует поддерживаемый SDR8:** первые два swapchain уже
имели SDR color space, но оставались 10-битными. Формат pixels и transfer/color
space — разные свойства. Не следует объявлять этот trace просто «HDR включён»
или просить повторить тот же запуск без изменения реального формата.

Защита остаётся: раньше physical-device dispatch на packed10 под Gamescope
провоцировал fatal loader trampoline. Этот trace не обосновывает снятие guard,
небезопасную подмену игрового формата или успешность packed10 пути. Исправление
требует отдельно проверить handle provenance/dispatch и реальный 10-битный
транспорт в полном loader/Gamescope окружении. Новый ручной прогон не запрошен.
