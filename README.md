# NZL Studio — обзор проекта и handoff

**Снимок контекста:** 2026-11-27. Этот README — сжатая рабочая запись проекта и договорённостей, не журнал переписки дословно. Основной язык общения — русский. Текущая сессия работает в ветке `arena/6d12f677-nngemes`; при продолжении не переключаться на другую ветку.

## Коротко

В репозитории два связанных, но разных компонента:

1. **Legacy RobRun launcher/API** — `robrun.py`, `START.bat`, `RobRun.exe`.
2. **Новый NZL Studio для ROB RUN** — C++ overlay/ESP, клиент shared-memory транспорта, kernel driver и Windows PowerShell сборка/launcher.

Новый overlay читает **живую память запущенного процесса** через driver. Это не динамический парсер файлов игры: root/TypeInfo RVAs и поля взяты из build-matched SDK dump и могут устареть после обновления игры.

> **Текущий незакрытый вопрос:** владелец сообщает, что видит лишь 2–3 игроков и ESP запаздывает. Предыдущая оптимизация чтений внесена в исходник, но результат в актуальном матче ещё не подтверждён. Последний план — собрать Windows-версию через PowerShell, проверить её в матче и прислать строку диагностики из overlay.

## Требования владельца, которые нужно сохранять

- Один компактный deliverable, предпочтительно единый `.bat`/`.exe` со встроенными необходимыми файлами, а не папка отдельных файлов.
- Launcher может запустить ROB RUN через Steam или ждать ручного запуска, затем автоматически подключить overlay.
- Меню доступно в лобби/во время загрузки; `Insert` открывает его, меню перетаскивается мышью, имеет вкладки и настройки. Курсор освобождается, клики меню не передаются игре.
- Не перезапускать overlay при каждом переходе между лобби, загрузкой и матчем; worker сам повторяет подключение и переживает PID/смену матча.
- ESP, aim, loot ESP и запрошенные визуальные настройки должны быть проверены по факту в игре, а не объявлены готовыми только потому, что соответствующий код есть.
- **Instant loot pickup исключён** — не добавлять.
- Не требовать перезагрузку ПК для каждого запуска overlay. Уже отображённый driver нужно переиспользовать; не запускать kdmapper поверх активного экземпляра.

## Как мы работаем

- Пользователь — Windows runtime-тестер: запускает PowerShell сборку и игру, присылает полный текст ошибок/логи или строку статуса ESP.
- Coding agent — меняет исходники в текущей ветке, запускает доступные sandbox-тесты и чётко отделяет **собрано**, **проверено в sandbox** и **проверено в матче**. Не обещать runtime-результат без свежего вывода владельца.
- Для проверки Windows предпочтителен имеющийся PowerShell workflow, а не длинные ручные инструкции. После изменения overlay пользователь запускает свежую сборку, заходит в матч и присылает точные `roster / pos / draw` значения.
- Не запрашивать пароли, токены или коды. Если меняется состояние Git, работать только в текущей ветке.

## Состав репозитория

- [`robrun.py`](robrun.py), [`START.bat`](START.bat) — legacy локальный API/launcher для бинарного `RobRun.exe`; исходников самого `RobRun.exe` нет.
- [`cpp/`](cpp/) — небольшое независимое C++20 math/ESP ядро и host-тесты.
- [`nzl/src/overlay_windows.cpp`](nzl/src/overlay_windows.cpp) — persistent UI, worker подключения, player/loot/car snapshots, проекция, ESP/aim.
- [`nzl/src/chain.cpp`](nzl/src/chain.cpp), [`nzl/src/esp_pipeline.cpp`](nzl/src/esp_pipeline.cpp), [`nzl/include/nzl/`](nzl/include/nzl/) — цепочки игры, reader abstraction, проекция и общая ESP-логика.
- [`nzl/client/`](nzl/client/), [`nzl/driver/`](nzl/driver/) — shared-memory клиент (`NaInit`, `NaBatchRead`, `NaScan`), kernel driver и `build_driver.bat` (MSVC + WDK).
- [`tools/NZL-BuildAll.ps1`](tools/NZL-BuildAll.ps1) — Windows build driver и NZL executables.
- [`tools/NZL-LoadDriver.ps1`](tools/NZL-LoadDriver.ps1) — первичная загрузка или безопасное переиспользование транспорта.
- [`tools/NZL-Package.ps1`](tools/NZL-Package.ps1) — генератор одного `NZL-Studio.bat` со встроенными `driver.sys`, `kdmapper.exe`, `nzl_overlay.exe`.
- [`NZL-START.bat`](NZL-START.bat) — multi-file Windows launcher; `NZL-Studio.bat` создаётся package builder.
- [`tools/NZL-Collect.ps1`](tools/NZL-Collect.ps1), [`tools/NZL-Dump.ps1`](tools/NZL-Dump.ps1), [`docs/EXTRACT.md`](docs/EXTRACT.md) — сбор fingerprint/dump для проверки build-specific данных; [`docs/ANALYSIS.md`](docs/ANALYSIS.md) — результаты анализа и тестов.

## Build и запуск на Windows

Из PowerShell в корне проекта:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ".\tools\NZL-BuildAll.ps1"
```

Проверь в конце вывода строку `nzl_overlay.exe OK`. Затем запусти:

```powershell
.\NZL-START.bat
```

`NZL-START.bat` запросит права администратора, проверит или переиспользует транспорт, затем попросит запустить игру и подтвердить переход в матч перед запуском overlay. Сгенерированный `NZL-Studio.bat` позволяет выбрать запуск ROB RUN через Steam либо ждать ручного запуска; overlay стартует автоматически после появления процесса. Для single-file варианта после успешной сборки:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ".\tools\NZL-Package.ps1"
.\NZL-Studio.bat
```

Нужны Windows x64, Visual Studio C++ toolset, CMake и WDK. `NZL-BuildAll.ps1` компилирует `driver.sys` и executables, но сам по себе **не отображает driver в ядро** и не заменяет экземпляр, уже находящийся в памяти.

### Повторные запуски без перезагрузки

Driver вручную отображается kdmapper и остаётся загруженным до конца сессии Windows. `NZL-LoadDriver.ps1` и сгенерированный one-file launcher проверяют `HKLM\SYSTEM\NaCoreFilter\Key` + `Shm`; при полном состоянии используют текущий экземпляр, а не запускают kdmapper повторно. Неполное состояние обрабатывается fail-closed.

- Повторный запуск overlay/launcher при уже загруженном driver не требует reboot.
- Если меняется только `nzl_overlay.exe`, достаточно пересобрать overlay и запустить его заново.
- Безопасной service-manager выгрузки/горячей замены ручного kdmapper map нет (`DriverUnload` не подключён). Для замены/снятия `driver.sys` или восстановления зависшего/неполного транспорта перезагрузка всё ещё может потребоваться.
- Ранее были BSOD при разработке driver; не загружать второй экземпляр поверх первого и не экспериментировать с ручным unmap.
- `NZL-SetKernelDump.ps1` запрашивает reboot только если параметры kernel dump ещё не установлены или изменились; повторный запуск с теми же настройками не должен требовать reboot.

## Игра и известные build-matched данные

- ROB RUN, Steam AppID **4262460**, подтверждённый владельцем buildid **26866884**.
- Unity `6000.3.18.6209205`, metadata v39. Для одного прежнего owner dump записаны GameAssembly size `148323840`, SHA-256 `2dc3326915eb1e62e86dee32d9fd64f8020623f6220de747272cd7ade767b5cf`.
- `nzl/include/nzl/game_profile.h` содержит отдельный профиль владельца build 26866884 и provenance для каждого offset: TypeInfo RVAs включают также `BattleCharacterModel 0x823FFD0`.
- В активном профиле `Game + 0xD8` — единственное значение для `_gameModel`; старый `+0xD0` от build 26615022 сохранён только как историческая заметка, fallback-код удалён.
- Цепочка активного профиля: `Game + 0xD8` → `GameModel + 0x10` → `BattleModel + 0x90` → `BattleEntitiesModel + 0x60` → `WorldEntitiesModel + 0x18` (battle-characters dictionary); `BattleModel + 0x358` → camera. Каждый объект проверяется по runtime klass.
- Dictionary reader берёт entries/count/stride/hash/value и x64 array capacity из профиля; `Count <= capacity`, верхний предел, hash/value pointers и ожидаемый класс персонажа проверяются до snapshot. Capacity читается целиком как x64 `size_t`.
- Координаты активного owner-профиля — transform `+0x10/+0x14/+0x18`; отдельный dump `lm3684759` от 2026-11-22 использует `+0x10/+0x18/+0x20` и не является fallback. Неизвестные loot XYZ и все live-unverified HP/armor/role/loot/cars chains остаются закрыты.

Перед использованием Windows-код сверяет размер и SHA-256 установленного `GameAssembly.dll` с записанным owner fingerprint, проверяет PE `SizeOfImage`/TypeInfo RVA и имена классов. При несовпадении профильные offsets блокируются без попытки подставить offsets другого билда. Это точное fingerprint-гейтирование, а не автоматическое обновление профиля; источник каждого значения и build-label должны оставаться видимыми в `game_profile.h`.

## Что реализовано в исходнике и что подтверждено

| Область | Реализация в исходнике | Ограничение / статус |
|---|---|---|
| Persistent UI | Отдельное menu window, `Insert`, перетаскивание, вкладки Aim/Visuals/Loot/Radar/Misc, настройки сохраняются рядом с exe | Требует повторной проверки на Windows |
| Attach/reconnect | Worker ждёт процесс, GameAssembly, классы и активную chain; повторяет попытки между loading/lobby/match/PID | Live-переходы не проверены в каждой ситуации |
| Player ESP | Boxes, labels/distance, dead toggle, HP/armor/roles, tracers, max distance и projection FOV | Сейчас владелец сообщает, что видит 2–3 игрока; причина ещё не установлена |
| Skeleton | Есть screen-space stick figure | Это пропорциональная 2D-схема, не чтение настоящих bone transforms |
| Aim | Hold key, FOV, smoothing, выбираемая Head/Chest/Pelvis точка, `SendInput` | Точки приближены высотами; настоящий bone targeting/visibility и live-работа не подтверждены |
| Loot ESP | Снимок loot-позиций, дистанция и экранный маркер | Generic marker; название/тип предмета и offsets не подтверждены в актуальном матче |
| Cars / radar | Снимок автомобилей, radar и настройки диапазона | Live-данные/геометрия требуют проверки |
| Visible only | Экранный/frustum фильтр | **Не** raycast и не проверка видимости через стены |
| Instant pickup | Не включён | Явно исключён владельцем |

### Частоты чтения

В последнем варианте worker отделяет частые данные от стабильных: позиции игроков обновляются каждый render-cycle; roster проверяется примерно раз в секунду; pointer records/HP-armor-role chains — раз в 3 секунды; HP/armor/role/dead values — раз в 250 мс; cars — раз в 250 мс; loot — раз в секунду. Камера читается batch-командой, а позиции и cached сущности перепроецируются между редкими refresh.

Pacing нацелен примерно на 16 мс за цикл, но если чтения медленнее, дополнительный фиксированный `Sleep` не добавляется. Это целевой pacing, **не обещание 60 FPS**.

## Диагностика жалобы «2–3 игрока и ESP лагает»

Строка статуса сейчас имеет вид:

```text
ESP data 58.0/s paint 56.0/s work 3ms | dict 26/32 active 26 roster 26 pos 26 draw 3 | dead 0 far 0 proj 23 edge 0 rd-fail 0
```

- `data/s` — завершённые циклы чтения/подготовки ESP; `paint/s` — измеренные `WM_PAINT` overlay; `work` — длительность последнего цикла worker.
- `dict count/capacity` — `Dictionary.Count` и x64 array capacity; `active` — слоты с неотрицательным hash и plausibly valid value pointer; `roster` — active values, у которых runtime klass равен `BattleCharacterModel`.
- `pos` — roster entries с конечной ненулевой координатой; `draw` — сущности, прошедшие фильтры и добавленные в render list.
- `dead` — исключены настройкой Show Dead; `far` — за Max Distance; `proj` — у сущности не прошла проекция хотя бы одной точки; `edge` — строгий экранный Visible Only; `rd-fail` — неудачные driver batch-команды на roster/hash/class чтении.
- Низкий `dict count/active` указывает на root/dictionary/build mismatch; большой `active`, но малый `roster` — на value class mismatch; большой `roster`, но малый `pos` — на transform/position чтение; нормальные `roster/pos`, но малый `draw` и большой `proj` — на camera basis/FOV/frustum/проекцию; высокий `data/s`, но низкий `paint/s` — на UI/paint bottleneck.

Ранее один probe показывал 26 dictionary entries, но это исторический результат другой проверки и **не подтверждает текущий overlay**. Нужна свежая строка статуса и, если возможно, скриншот из матча.

## Driver и безопасность разработки

- Shared-memory client читает `HKLM\SYSTEM\NaCoreFilter`; `NA_CMD_BATCH` обслуживает до 64 items, `NA_CMD_SCAN` — поиск root-кандидатов ограниченными kernel chunks.
- Driver валидирует user-space диапазоны перед копированием; известная ошибка `NaRegion`, писавшая в section view во время attach, была исправлена копированием результата после detach и затем проходила owner probe.
- Kernel driver исторически вызывал BSOD во время разработки. Это не обычный service driver; сохранять kernel dump перед новыми kernel-side изменениями и никогда не делать второй kdmapper map поверх активного.
- Не утверждать, что driver unload/reload или EAC-совместимость протестированы. Тесты и build checks не заменяют owner runtime.

## Проверки и текущий статус

**В sandbox проверено:**

- Host `chain`, `pipeline`, `profile` tests: все три бинарника собраны g++ C++20; `ALL NZL TESTS PASSED`, `ALL PIPELINE TESTS PASSED`, `ALL PROFILE TESTS PASSED`.
- Python: `py_compile robrun.py` и `unittest discover -s tests -v`; 12 tests прошли.
- Zig 0.13.0 cross-build для `x86_64-windows-gnu`: `overlay_windows.cpp`, `main_windows.cpp`, `probe_windows.cpp`, shared client, driver-memory adapter, chain/pipeline объекты и три PE x64 exe собраны/связаны, каждый compile/link exit code 0. Это GNU cross-build, не MSVC workflow; EXE остаются в `/tmp`, не входят в ZIP.

**Не проверено здесь:** нативная MSVC+WDK сборка и запуск PowerShell workflow; сборка kernel driver; one-file `.bat`; выполнение EXE в Windows; live match и актуальная полнота player render; реальные data/paint rates в игре; точность aim/HP/armor/roles/loot/cars. Runtime-причина 2–3 видимых игроков не установлена.

## Следующий шаг

1. В `D:\NZL Rob run` заменить исходники содержимым `NZL-Studio-updated-source.zip` и из корня выполнить `powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\NZL-BuildAll.ps1`. Скрипт теперь возвращает ненулевой exit code при сбое driver/CMake/CTest и печатает exit каждого этапа.
2. Убедиться, что `nzl_overlay.exe OK`, затем запустить `NZL-START.bat` (или после успешной сборки создать single-file через `NZL-Package.ps1`).
3. Войти в матч и прислать полную строку `data/paint/dict/roster/pos/draw` и сообщение об ошибке, если оно появится.
4. По счётчикам определить, где теряются игроки и где расходуется время; только затем менять профиль или projection.
