# GSF: аудит пути Windows-игры через Proton до Gamescope

Дата: 2026-10-19. Запрос: проверить, не ошибочна ли работа Linux Vulkan-слоя с
Windows-играми через Proton. Пользователь отдельно уточнил: **60 Гц могут быть
просто выбраны в игре**. Это допустимое объяснение; автоматической замены на
90 Гц и требования изменить пользовательские настройки нет.

## Результат проверки

Сам выбор Vulkan-перехвата совместим с DXVK/VKD3D: переводчики сами отправляют
настоящие Vulkan commands и предъявляют Vulkan swapchain. Для GSF существенны
их submission/presentation semantics, а не формат Windows EXE. Однако простой
нативный Vulkan smoke не воспроизводит всю организацию очередей переводчика.
Полноценного прогона этой Windows-игры через Proton в облачной среде не было.

Подтвержденная ошибка наблюдаемости: прежнее `refresh_source=WSI-query`
проверяло лишь наличие function pointer. Теперь startup сообщает именно
`refresh_query_available`; каждая pair содержит реальный результат запроса,
сырое значение и происхождение примененного периода. Политика выбора периода,
`gfg_frame_clock_pair`, формулы admission, семафоры и способ подачи команд не изменены.
Отдельно исправлена обработка калибровки при старте GPU до возврата submit (ниже).

Причина 34–41 мс на Deck **не установлена**. Ни этот аудит, ни положительный
счетчик app-submit не доказывают, что весь интервал занят рендерингом игры.

## Проверенные исходники и предел применимости

Изучены конкретные upstream snapshots:

| Компонент | SHA | Проверенные участки |
| --- | --- | --- |
| DXVK | `2cb40186c43d617b5bd349a3756721aa4bd24b6d` | [Presenter::presentImage](https://github.com/doitsujin/dxvk/blob/2cb40186c43d617b5bd349a3756721aa4bd24b6d/src/dxvk/dxvk_presenter.cpp), [D3D11SwapChain::SyncFrameLatency](https://github.com/doitsujin/dxvk/blob/2cb40186c43d617b5bd349a3756721aa4bd24b6d/src/d3d11/d3d11_swapchain.cpp), [submission queue](https://github.com/doitsujin/dxvk/blob/2cb40186c43d617b5bd349a3756721aa4bd24b6d/src/dxvk/dxvk_queue.cpp) |
| VKD3D-Proton | `0bb64c56c8f0dcc11ecc0f6c56e00e4f7bd8d506` | [swapchain.c](https://github.com/HansKristian-Work/vkd3d-proton/blob/0bb64c56c8f0dcc11ecc0f6c56e00e4f7bd8d506/libs/vkd3d/swapchain.c), [command submission](https://github.com/HansKristian-Work/vkd3d-proton/blob/0bb64c56c8f0dcc11ecc0f6c56e00e4f7bd8d506/libs/vkd3d/command.c) |
| Gamescope | `74a06ff517ed9fc1307227406bd53ae847be3a91` | [FROG WSI layer](https://github.com/ValveSoftware/gamescope/blob/74a06ff517ed9fc1307227406bd53ae847be3a91/layer/VkLayer_FROG_gamescope_wsi.cpp), [compositor](https://github.com/ValveSoftware/gamescope/blob/74a06ff517ed9fc1307227406bd53ae847be3a91/src/steamcompmgr.cpp) |

Это **не доказанные SHA установленных на Deck библиотек**. В Ghost of Tsushima
архиве (7) есть объявления `VkApplicationInfo.pEngineName=DXVK/vkd3d`, но они
не связаны структурированным pid/instance/generation контекстом с каждым bridge.
Сам `engineVersion` не устанавливает точный Proton build. Нельзя приписывать
любую upstream-ветку конкретному установленному процессу без проверки.

Новая `bridge_context` связывает pid + bridge generation с классификацией
объявленного engine (`dxvk`, `vkd3d`, `other_or_unspecified`), исходным числовым
engineVersion/API version, queue family и фактом использования TE той же present
queue. Это **hint из VkApplicationInfo**, не детектор происхождения EXE, версии
Proton или всей DirectX-цепочки. DXGI-компонент также может создавать свой instance.

## Где действительно находится готовность кадра

VKD3D `dxgi_vk_swap_chain_present_iteration` сначала получает swapchain image,
выполняет `dxgi_vk_swap_chain_submit_blit`, затем вызывает `vkQueuePresentKHR`.
В проверенном коде submit_blit отправляет **два VkSubmitInfo2 за один API call**,
с binary release semaphore и internal timelines, но с `VK_NULL_HANDLE` fence.
Present ожидает release semaphore. Сам CPU-вызов Present не означает, что эта
GPU-работа уже завершена.

VKD3D также имеет отдельный swapchain-sync thread: он ожидает Present ID через
WaitForPresent(2) либо complete semaphore, после чего освобождает frame-latency
handles. В DXVK D3D11 также есть собственное ограничение frame latency и
Presenter, передающий present ID, completion fence, mode и optional timing.
Нельзя сводить эти процессы к одному линейному CPU-вызову игры.

GSF в `layer.c` снимает `source_ns` при входе CPU в свой Vulkan Present. Output
worker сохраняет это время, а не заменяет его моментом dequeue. Он сначала
**ставит на очередь** ожидание исходных app semaphores с передачей в private gate,
прежде чем вернуть управление приложению. Это не host wait готовности GPU.
Далее `submit_real` ожидает gate, копирует источник и сигналит timeline; TE
использует этот dependency. На same-device path TE заимствует тот же VkDevice,
VkQueue и queue family через `te_device_borrow`.

Следовательно, явного «считать готовым без Vulkan синхронизации» в этом пути не
обнаружено. Но `source_ns` — CPU boundary, **не GPU-ready timestamp**. Новая
context-запись это указывает; `source_gpu_ready_ns` остается null.

## Существенный риск позиции TE в очереди

Асинхронный GSF возвращает управление после submit private gate и постановки
job в bounded CPU mailbox. Между этим и отправкой source-copy/TE output worker
другие потоки переводчика могут отправить новую game-work в ту же очередь.
Код допускает такой порядок; один общий queue host mutex сохраняет корректность
доступа, но не дает TE приоритета перед более ранними submission.

Это объясняет, почему стоит наблюдать позицию submit и зависимости. Это **не
доказательство**, что в конкретном Deck trace TE оказался за будущим кадром,
и не основание убирать semaphore waits, переносить compute в произвольную очередь
или добавлять GPU idle на каждый кадр.

Отдельное ограничение PR #38: VKD3D часто использует unfenced Submit2 с timeline
signals. Счетчик, наблюдающий существующие **fences**, не может подтвердить
завершение таких calls без поздней покрывающей fence/idle. Поэтому даже здоровая
очередь может иметь растущий `unfenced_unretired` upper bound. Поддержка Submit2
в перехвате не устраняет эту неопределенность. Высокий счетчик без completion
coverage нельзя принимать за hardware backlog и выбирать по нему архитектуру.
Наблюдение существующих timeline signals потребовало бы отдельного контроля
типа/жизни/импорта semaphore и signal provenance; оно здесь не притворяется готовым.

## Что означают 60 Гц

В проверенном Gamescope `GamescopeSwapchainData.refreshCycle` инициализируется
16 666 666 нс. `GetRefreshCycleDurationGOOGLE` прокачивает Wayland events и
возвращает это поле с VK_SUCCESS; listener `refresh_cycle` обновляет его по
сообщению compositor. Значит, даже успешный запрос может вернуть значение,
происхождение которого внутри compositor нам неизвестно. На стороне compositor
`window_refresh_cycle` также может учитывать app-target limiter; сообщаемый цикл
не следует автоматически считать физической частотой панели.

В SDR-архиве (7) GSF pair-период равен 16 666 666 нс, в reported actual-present
timing встречается сетка около 11.111 мс. Возможные объяснения включают игровую
настройку, app-target cycle, начальное/сохраненное значение WSI, версию/протокол
compositor. По старому trace они не разделены. Отсутствие строки об обновлении
refresh не доказывает, что установленная версия ведет лог так же, как upstream.

Новая диагностика **принимает успешные 60 Гц буквально**. Она не вычисляет частоту
панели из минимального gap и не корректирует clock по предполагаемой модели Deck.

| Поле pair | Значение |
| --- | --- |
| `refresh_period_ns` | Примененный период; существующее поле |
| `refresh_origin` | `configured_fallback`, `explicit_override`, `successful_wsi_query` или `unset` |
| `refresh_query_state` | Состояние именно текущей pair: accepted/error/range rejection/override/unavailable/inactive |
| `refresh_query_result` | VkResult текущего запроса; null, если его не было |
| `refresh_query_raw_ns` | Сырые байты значения из обнуленной структуры; при ошибке API они не валидируются как refresh |
| `refresh_query_cpu_ns` | CPU elapsed вокруг существующего запроса, включая WSI host mutex; не GPU time |
| `refresh_last_success_ns` | CLOCK_MONOTONIC последнего принятого запроса, сохраняется при последующих ошибках |
| `refresh_override_present` | Наличие env override; не означает его валидность или применение |

Ошибочный/недоступный запрос сохраняет последний период **и его происхождение**,
а не переименовывает успешное старое значение в новый fallback. Существующее
правило подавления запроса по самому наличию OUTPUT_HZ, включая невалидную строку,
сохранено и теперь различимо в trace. Никаких новых refresh calls, GPU commands,
GPU waits или фоновых потоков не добавлено; JSON дописывается в существующую pair
вне queue lock. Два host clock reads измеряют стоимость уже существующего запроса.

## Проверка

Локально выполнены:

- `refresh-observation`: валидные 60 Гц, fallback, сохранение origin после ошибки,
  invalid/range boundaries и override, без самовольного перехода на 90 Гц.
- Frame-clock replay, generation-budget на сохраненном Deck fixture,
  calibration-clock и queue-observation contracts.
- 7 recovery-report и 19 Decky backend тестов.
- C11 `-Wall -Wextra -Werror` syntax checks layer без TE и всех native units с TE;
  Python compile и whitespace checks.

В readiness CI добавлен реальный Vulkan case без OUTPUT_HZ, проверяющий связь
context/pair и происхождение периода. Остальные cases проверяют, что явные 90 Гц
стенда не маскируются под WSI query. Это lavapipe + Vulkan loader, **не Proton**;
результаты не выдаются за измерение latency Windows-игры или физический 2×.


## Найденный в CI дефект: GPU стартует раньше CPU submit-return

Первый прогон текущей диагностики (`3b07905`) прошел 8 из 9 workflows.
Validation-first readiness прошел, native-first остановился: все 15 GPU timing
samples в new-admission-delayed были отвергнуты как `inconsistent_timestamps`.
Артефакт [12162181007](https://github.com/wavessevaw/GSF/actions/runs/39195796393/artifacts/12162181007),
ZIP SHA256 `b378572faa6dd3ec04c87123bc122884309ef2f9d3231a6afd7a0cbf4eefc770`.
Пороги теста не ослаблялись, успешный повтор вместо разбора ошибки не использован.

В этих 15 samples device start был на **24..10678 нс раньше** host submit-return,
при maxDeviation=1 нс и совпадающих CPU/GPU calibrated clocks lavapipe. Старое
условие `start < submit_return` безусловно отвергало такой порядок. Это неверно:
асинхронная GPU-работа может начаться до возврата queue submission на CPU.
[Модель Vulkan queues](https://docs.vulkan.org/spec/latest/chapters/fundamentals.html)
не устанавливает host-return как начало исполнения на device.

Исправление сохраняет фактический signed `gpu_start_minus_submit_return_ns` и
валидный calibrated GPU-start. Для admission берется только остающаяся после
возврата задержка `max(0, signed_delta)`, без unsigned underflow. Историческое поле
`submit_to_gpu_start_ns` теперь явно означает эту неотрицательную величину;
signed поле дает исходное измерение. Это не чистое время ожидания hardware queue.
Пределы maxDeviation, stale calibration и правдоподобия ±2 с сохранены. Никакие
дополнительные GPU/host waits и изменения TE ABI не понадобились.

Все 15 исходных samples сохранены как числовой fixture
`tests/fixtures/software-submit-overlap-20261019.csv` и проходят точный C replay;
unit checks также проверяют положительный/нулевой delta и отказ вне пределов.
Старые положительные Deck задержки 34–41 мс от этого исправления не исчезают:
оно исправляет отбраковку быстрого старта и возможную потерю calibration/admission
coverage, а не доказывает устранение игровой просадки.
