# NZL Studio — обзор проекта и handoff

**Снимок контекста:** 2026-11-27. Этот README — сжатая рабочая запись проекта и договорённостей, не журнал переписки дословно. Основной язык общения — русский. Текущая сессия работает в ветке `arena/6d12f677-nngemes`; при продолжении не переключаться на другую ветку.

## Карта папки: где что лежит

| Путь | Что это |
|---|---|
| `NZL.bat` | единственная команда, которую надо помнить: `build`, `load`, `overlay`, `probe`, `report`, `clean` |
| `docs\LIVE-RUN.md` | порядок запуска по шагам, что должно быть в выводе, что делать при багчеке |
| `HANDOFF.md` | состояние работы: что сделано, что открыто, почему |
| `nzl\` | исходники: `src\` (оверлей, проба), `include\`, `client\` (транспорт), `driver\` (драйвер), `tests\` (хостовые тесты) |
| `tools\` | PowerShell-скрипты за `NZL.bat`: сборка, загрузка драйвера, отчёты, чистка |
| `tests\` | старые Python-тесты локального API (legacy) |
| `config.ini` | сохранённые настройки меню |
| `_trash-<дата>\` | сюда `NZL.bat clean -Apply` складывает мусор; удаляется целиком вручную |

Всё остальное в корне — артефакты прогонов (отчёты, пробы, старые сборки), их
убирает `.\NZL.bat clean -Apply`.

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
- [`nzl/client/`](nzl/client/), [`nzl/driver/`](nzl/driver/) — shared-memory клиент (`NaInit`, `NaBatchRead`, `NaScan`, `NaGetModule`), kernel driver и `build_driver.bat` (MSVC + WDK).
- [`nzl/include/nzl/na_wire.h`](nzl/include/nzl/na_wire.h) — единственное определение wire-протокола (`NA_SHM`, команды, лимиты) + compile-time assert раскладки. Включается и драйвером, и клиентом.
- [`nzl/include/nzl/na_validate.h`](nzl/include/nzl/na_validate.h), [`na_scan.h`](nzl/include/nzl/na_scan.h), [`na_pebwalk.h`](nzl/include/nzl/na_pebwalk.h) — переносимые проверки адресов, state machine kernel-скана и walk по PEB; компилируются и в ядро, и в host-тест.
- [`nzl/tests/test_protocol.cpp`](nzl/tests/test_protocol.cpp) — 300k+ host-проверок протокола; [`nzl/driver/check_driver.sh`](nzl/driver/check_driver.sh) и [`nzl/client/check_client.sh`](nzl/client/check_client.sh) — syntax-гейты для двух половин протокола без Windows.
- [`tools/NZL-BuildAll.ps1`](tools/NZL-BuildAll.ps1) — Windows build driver и NZL executables.
- [`tools/NZL-LoadDriver.ps1`](tools/NZL-LoadDriver.ps1) — первичная загрузка или безопасное переиспользование транспорта.
- [`tools/NZL-Package.ps1`](tools/NZL-Package.ps1) — генератор одного `NZL-Studio.bat` со встроенными `driver.sys`, `kdmapper.exe`, `nzl_overlay.exe`.
- [`NZL-START.bat`](NZL-START.bat) — multi-file Windows launcher; `NZL-Studio.bat` создаётся package builder.
- [`tools/NZL-Report.ps1`](tools/NZL-Report.ps1) — один txt со всем состоянием сборки (git, toolchain, `driver_build.log`, `LastTest.log`, `nzl_probe.txt`, BugCheck, хеши). `-Build` дополнительно прогоняет `NZL-BuildAll.ps1` с полным захватом вывода, `-CopyToDropbox` кладёт отчёт в Dropbox.
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
| Skeleton | Screen-space stick figure остаётся приблизительной; при включённом пункте `Skeleton (approx.)` считывается и диагностируется raw `SkeletonModel`/`SkeletonConfig` pose snapshot | Реальные world-space суставы ещё не рисуются: формат hierarchy и преобразование model pose → world пока не подтверждены в live-матче |
| Aim | Hold key, FOV, smoothing, выбираемая Head/Chest/Pelvis точка, `SendInput` | Точки приближены высотами; настоящий bone targeting/visibility и live-работа не подтверждены |
| Loot ESP | Снимок loot-позиций, дистанция и экранный маркер | Generic marker; название/тип предмета и offsets не подтверждены в актуальном матче |
| Cars / radar | Снимок автомобилей, radar и настройки диапазона | Live-данные/геометрия требуют проверки |
| Visible only | Экранный/frustum фильтр | **Не** raycast и не проверка видимости через стены |
| Instant pickup | Не включён | Явно исключён владельцем |

### Частоты чтения

В последнем варианте worker отделяет частые данные от стабильных: позиции игроков обновляются каждый render-cycle; roster проверяется примерно раз в секунду; pointer records/HP-armor-role chains — раз в 3 секунды; HP/armor/role/dead values — раз в 250 мс; cars — раз в 250 мс; loot — раз в секунду. Камера читается batch-командой, а позиции и cached сущности перепроецируются между редкими refresh; raw skeleton snapshot обновляется раз в секунду только при включённом `Skeleton (approx.)`.

Pacing нацелен примерно на 16 мс за цикл, но если чтения медленнее, дополнительный фиксированный `Sleep` не добавляется. Это целевой pacing, **не обещание 60 FPS**.

## Диагностика жалобы «2–3 игрока и ESP лагает»

Строка статуса сейчас имеет вид:

```text
DATA 58.0/s | PAINT 56.0/s | WORK 3ms
DICT 26/32 active 26 | ROSTER 26 POS 26 DRAW 3 | SKEL raw 0/26 m0 c0 n0 h-1 model
CULLED dead/far/proj/edge 0/0/23/0 | RD-FAIL 0 | CAM-FAIL 0/0/0 | FOV 90 VIEW 1920x1080 BASIS AXIS
```

- `data/s` — завершённые циклы чтения/подготовки ESP; `paint/s` — измеренные `WM_PAINT` overlay; `work` — длительность последнего цикла worker.
- `dict count/capacity` — `Dictionary.Count` и x64 array capacity; `active` — слоты с неотрицательным hash и plausibly valid value pointer; `roster` — active values, у которых runtime klass равен `BattleCharacterModel`.
- `pos` — roster entries с конечной ненулевой координатой; `draw` — сущности, прошедшие фильтры и добавленные в render list.
- `SKEL raw valid/attempted m... c... n... h... stage` — диагностический снимок настоящего managed SkeletonModel: прошедшие проверки записи / roster, совпавшие классы model/config, число костей и индекс Head1. Это проверка сырых pose данных, не подтверждение world-space положения и не доказательство точности рисунка.
- `dead` — исключены настройкой Show Dead; `far` — за Max Distance; `proj` — у сущности не прошла проекция хотя бы одной точки; `edge` — строгий экранный Visible Only; `rd-fail` — неудачные driver batch-команды на roster/hash/class чтении.
- Низкий `dict count/active` указывает на root/dictionary/build mismatch; большой `active`, но малый `roster` — на value class mismatch; большой `roster`, но малый `pos` — на transform/position чтение; нормальные `roster/pos`, но малый `draw` и большой `proj` — на camera basis/FOV/frustum/проекцию; высокий `data/s`, но низкий `paint/s` — на UI/paint bottleneck.

Ранее один probe показывал 26 dictionary entries, но это исторический результат другой проверки и **не подтверждает текущий overlay**. Нужна свежая строка статуса и, если возможно, скриншот из матча.

## Driver и безопасность разработки

- Shared-memory client читает `HKLM\SYSTEM\NaCoreFilter`; `NA_CMD_BATCH` обслуживает до 64 items, `NA_CMD_SCAN` — поиск root-кандидатов ограниченными kernel chunks.
- Driver валидирует user-space диапазоны перед копированием; известная ошибка `NaRegion`, писавшая в section view во время attach, была исправлена копированием результата после detach и затем проходила owner probe.
- Kernel driver исторически вызывал BSOD во время разработки. Это не обычный service driver; сохранять kernel dump перед новыми kernel-side изменениями и никогда не делать второй kdmapper map поверх активного.
- Не утверждать, что driver unload/reload или EAC-совместимость протестированы. Тесты и build checks не заменяют owner runtime.

## Протокол драйвера: один заголовок, две стороны, host-тест

Раскладка `NA_SHM` раньше была продублирована руками в `nzl/driver/sc.h` и `nzl/client/sc_client.h`. Любое расхождение (лишнее поле, другой pack, другая ширина `NTSTATUS`) сдвигает все последующие смещения, драйвер начинает читать VA клиента из чужого слота — это wild-адрес в `MmCopyVirtualMemory`, то есть ровно класс BSOD #1–#4. Теперь раскладка определена один раз в `na_wire.h`, а `NA_STATIC_ASSERT` внутри заголовка компилируется **обеими** сторонами: расхождение стало ошибкой сборки, а не синим экраном.

Закреплённые смещения (`NA_SHM_OFF_*`): `region 0x34`, `items 0x54`, `data 0x454`, `sizeof 0x10454`. Первые два адреса из `0x1E`-дампов (`...0034`) и адрес из `0x50`-дампа (`...0454`) попадают точно в `region` и `data` — эти два значения закреплены тестом.

Исправлено в этой итерации:

- `NA_CMD_SCAN`: при заполнении `maxcand` в середине 64-КБ чанка resume-VA возвращался как начало чанка, следующий вызов находил те же кандидаты и возвращал тот же resume-VA — клиентский `while (next)` не завершался никогда. Теперь resume-VA = первый ещё не просмотренный qword, прогресс строгий, дубликатов и пропусков нет. Проверено property-тестом против brute-force эталона.
- `NA_CMD_MODULE` был объявлен в обоих заголовках и реализован ни в одном: в `switch` воркера не было `case`, `NaGetModule()` всегда возвращал 0, выглядя рабочим API. Реализовано через PEB walk по `BaseDllName` (`na_pebwalk.h`) с чтением только через `MmCopyVirtualMemory`.
- `g_active` выставлялся **после** `PsCreateSystemThread`: поток мог увидеть 0, выйти и оставить драйвер загруженным, с ключом в реестре и без единого ответа на команду.
- `NaUnload` не был назначен в `DriverEntry` (мёртвый код). Назначен; при таймауте ожидания воркера view не размапливается, чтобы не освобождать память под работающим потоком.
- `NaPeb` и `NaAlloc` получили проверки диапазона/размера.

## Проверки и текущий статус

**В sandbox проверено:**

- Direct host g++ C++20 builds/runs of `test_chain`, `test_pipeline`, `test_profile`, and the new `test_skeleton` all passed. These cover the bounded raw skeleton parser with synthetic FakeMemory data; they do not validate target-game values.
- Python: `py_compile robrun.py` и `unittest discover -s tests -v`; 12 tests прошли ранее.
- `nzl_protocol_tests` (host, g++ 12.2 C++20): 300 272 проверки, 0 падений — раскладка `NA_SHM`, fuzz `na_user_range`/batch-предикатов, полнота скана против brute-force, отсутствие ливлока resume-VA, unreadable-страницы, byte-budget, PEB walk на синтетическом PEB. Прогнано также под ASan+UBSan.
- `sh nzl/driver/check_driver.sh` — `sc_drv.c` целиком компилируется (`-Wall -Wextra -Werror`, WDK shim) exit 0; `sh nzl/client/check_client.sh` — `sc_client.cpp` exit 0.

**На Windows владельца проверено:**

- Owner-reported Windows validation before the latest safety edits: `NZL-BuildAll.ps1` completed with exit 0 using MSVC 14.51.36231 / Windows SDK 10.0.26100.0; it built `driver.sys` without loading it, built the x64 apps, and passed all four CTest suites. `NZL-Package.ps1` exited 0 and generated package ID `63f9448734a579a296a5c3ce50e0456860d07c2fdd8fd8ae1a5b7d4370dfec62`. That BAT predates the current package safety guard and must not be run.
- Latest source-only driver/package safety edits have not yet been verified by Windows BuildAll or repackaging. This sandbox itself still lacks the Windows toolchain.

**Не проверено:** выполнение EXE в игре; live match и актуальная полнота player render; реальные data/paint rates в игре; live-раскладка `_bonesData`, hierarchy и model-to-world преобразование; точность aim/HP/armor/roles/loot/cars. Runtime-причина 2–3 видимых игроков не установлена.

## Статус и следующий шаг

Windows `BuildAll` и упаковка прошли до последних изменений; повторный `BuildAll` и repackage ещё нужны после синхронизации исправлений. Старый BAT (package ID `63f9448734a579a296a5c3ce50e0456860d07c2fdd8fd8ae1a5b7d4370dfec62`) не содержит новую preflight-проверку и запускать его нельзя.

Live-проверка скелета не выполнена. `crash_info-5.txt` показывает три BugCheck-события 2026-11-27: два `0x1E` (21:56 и 22:18) и один `0x50` (23:14); minidump-список включает ещё файл за 18:03. Все три предоставленных анализа `0x1E` — `STATUS_ACCESS_VIOLATION` при записи в user VA (`0x14dbca20034`, `0x2010bc60034`, `0x20718900454`) во время `ROB RUN.exe`. При packed layout `NA_SHM` первые два адреса — это начало `region` (`+0x34`), что сильно подтверждает ошибку `NaRegion` с записью в shared output при attach к другому процессу; такой write в текущем коде делается уже после detach. Третий адрес — начало `NA_SHM.data` (`+0x454`), но его triage stack обнулён. Анализ последнего `0x50` показывает чтение kernel VA `fffffede9deede7f` в `nt!memcpy <- MiCopyVirtualMemory <- MmCopyVirtualMemory`, возврат в неизвестный модуль `ffff8503`7b01378e`; это согласуется с неверным source VA в kernel-driver copy path. Это не ошибка декодирования костей; вручную отображённый модуль не идентифицирован WinDbg, а dump указал на 75% завершения.

В source workspace `NaBatch` теперь snapshot-ит запрос и проверяет все адреса/границы до `MmCopyVirtualMemory`; `NaBase` также проверяет диапазон PEB перед копированием. Генератор нового пакета отказывает в fresh mapping, если `CrashDumpEnabled != 2`. Эти kernel-driver/package изменения ещё не проверены нативной Windows-сборкой и не входят в уже созданный BAT. Host-тесты и статические проверки прошли, но runtime остаётся остановлен после нескольких BSOD: не запускать ни старый, ни новый BAT, игру или драйвер. После синхронизации исходников запустить `NZL-BuildAll.ps1`, затем `NZL-Package.ps1` и прислать выводы — пакет не запускать. Live Skeleton `SKEL raw`, hierarchy и world-space преобразование остаются неподтверждёнными.
