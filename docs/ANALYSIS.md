# ROB RUN — анализ репозитория NNGemes

Дата: 2026-11-27 (обновлено после Dropbox + unknowncheats + github).
Сборка игры (S1): `justdoshiba/RobRunExternal@0ef798f`, build **26615022**,
metadata **39**, GameAssembly SHA-256 `03003754…65c18ab`.
Полевые офсеты — **VERIFIED** (5+ источников согласны), статические RVA —
**КОНФЛИКТ** (разные билды), GOM/camera RVA — **UNVERIFIED** (single-source).
См. `cpp/include/robrun/config.h` и §5.

## 1. Что в репозитории

| Файл | Назначение |
|---|---|
| `robrun.py` | Локальный API (127.0.0.1:800) + запуск `RobRun.exe` через именованный канал. Один файл, Python 3.10+. |
| `RobRun.exe` (1.9 MB, x64) | Чит: external overlay (D3D11 + DirectComposition `ROBRUN_DCOMP`, ImGui), ESP (skeleton/box/name/distance/weapon), aim assistance, radar, loot ESP, fast loot (патч `timeToCell` в памяти), kdmapper + уязвимый драйвер Intel (`iqvw64e.sys`), очистка `WdFilter` списка, античит-репорты (`/api/v1/anticheat/report`, `/status`, `hwid`, `sha256`, `parent_sha256`). |
| `config.ini` | Конфиг чита, отдаётся через `GET/PUT /api/v1/runtime/config`. |
| `START.bat` | Запуск: проверка Python 3.10+, установка через winget при отсутствии, `python -u robrun.py`. Первый запуск спрашивает про посещение страниц авторов (yougame.biz, t.me) и сохраняет выбор в `%LOCALAPPDATA%\robrun\visit.txt`. |
| `requests.jsonl`, `20261126T*.json` | Рантайм-артефакты: лог запросов локального API и DIAG-репорты клиента (`crash`). В репозитории лежат копии из `data/` — см. `.gitignore`. |
| `cpp/` | Чистое ядро ESP (C++20): `world_to_screen`, фильтр сущностей, конфиг с гейтами. Компилируется на Linux и Windows. |
| `tests/` | Изолированные тесты `robrun.py` (unittest, без Windows и игры). |
| `dropbox/` (копия, в `.gitignore`) | Примеры из Dropbox: `TrentsTuningRR_[unknowncheats.me]_.exe` (v1.4.0.0, лаунчер), `.rr_tmp/TrentsTuningOverlay.exe` (v1.04, оверлей), `kd2.exe` (kdmapper+Intel), `kernel-mode.sys` (драйвер), `WebView2Loader.dll`, `aio.log`, `masq.txt`, `Focus.exe` (84 MB, только метаданные). См. §2.5. |

## 2. Архитектура (измерено по бинарнику и логам)

### 2.1. Локальный сервер (`robrun.py`)

- `POST /api/v1/runtime/redeem` (64 hex ticket) → 64 hex token (Bearer).
- `GET/PUT /api/v1/runtime/config` (Bearer) → `config.ini` (атомарная запись через tempfile+replace, лимит 16 KB, валидация INI).
- `POST /api/v1/runtime/crash` (Bearer) → JSON в `data/crashes/<ts>-<uuid>.json` (лимит 64 KB, `allow_nan=False`).
- Всё остальное → 404. В частности `GET /api/v1/anticheat/status` → 404.
  По `requests.jsonl` клиент пробовал его 9 раз, каждый раз 404, и продолжил
  работу (36 crash-репортов, 12 PUT config). Вывод: 404 на anticheat/status
  клиент терпит; эмулировать `{"status":"ok"}` не требуется и без живой
  проверки на Windows не стал.
- `check_server` проверяет `Server: RobRunLocal/1` — защита от коллизий на порту.

### 2.2. Bootstrap-канал

Лаунчер создаёт `\\.\pipe\RobRun.<32hex>` (SDDL: только текущий SID + SYSTEM),
запускает `RobRun.exe --loader-pipe <имя>` через `ShellExecuteExW`
(`SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NOASYNC`), пишет один фрейм
`ROBRUN/1\nhttp://127.0.0.1:800\n1\n<ticket>\n` (LE-u32 длина + payload,
макс. 4096), ждёт `initializing` / `waiting_game` / `ready`, сверяет
`client_pid == process.pid`.

### 2.3. URL и патч

- Сборка 1.0.81 (`ORIGINAL_SHA256 7f1b07f7…`): URL `https://95.85.230.17`
  хранился как NUL-строка, `patched_image()` менял его на
  `http://127.0.0.1:800` (равная длина 20). Функция оставлена для 1.0.81,
  лаунчером не вызывается.
- Сборка 1089 (текущий `RobRun.exe`, SHA `6ca52c0a…`): URL **не** хранится
  plain-строкой. В бинарнике только фрагменты `https://`, `http://`, `:%u`
  (смещение ~1268016) — адрес собирается в рантайме. Поэтому `patched_image()`
  к 1089 неприменим; `verify_local_image()` для 1089 — это SHA-allowlist
  (`PATCHED_SHA256_1089`, `LEGACY_PATCHED_SHA256`) + структурный fallback
  только для 1.0.81. Текущий EXE проходит allowlist.

### 2.4. RobRun.exe (строки, без исполнения)

- Классы из дампа присутствуют: `IBattleEntitiesModel`, `BattleEntitiesModel`,
  `BattleCharacterEntityModel`, `BattleCharacterModel`, `PlayerEntityModel`,
  `CharacterTransformModel`, `Models.References.Battle`,
  `Models.ReferencesProject.Descriptions.Battle`, `Models.References.Knockout`,
  `FactoriesModel`, `PlayerClientModel`, `LootNodeEntityModel`,
  `ContainerEntityModel`, `VehicleLootEntityModel`, `StaticLootEntityModel` и др.
  Это подтверждает, что модель мира в EXE совпадает с моделью из
  `RobRunExternal` (имена классов), но **не** подтверждает числовые офсеты.
- Камера: `presentation camera: source=%s`, `game hwnd = 0x%p`,
  `controls: menu=%s / %s unload=%s combatEnabled=%d hold=%s fov=%.1f smooth=%.1f`.
- Оверлей: `Hidden Overlay`, `ROBRUN_DCOMP`, `D3D11CreateDeviceAndSwapChain`,
  `DwmEnableBlurBehindWindow`, `SetLayeredWindowAttributes`,
  `CreateWindowExA`, `EnumWindows`, `GetForegroundWindow`, `GetWindowRect`.
  Импорт `GetModuleHandleA`, `IsDebuggerPresent`, `CheckRemoteDebuggerPresent`,
  `NtQueryAuxiliaryCounterFrequency`, `SE_DEBUG_PRIVILEGE`.
- Драйвер: kdmapper + Intel (`iqvw64e.sys`), `WdFilter` чистка
  (`RuntimeDriversList/Count`, `MpFreeDriverInfoEx`), `NtLoadDriver/NtUnloadDriver`,
  `SeLoadDriverPrivilege`. PDB: `C:\Users\user\Desktop\work\ROB RUN\build\beta\out\RobRun.pdb`.
- Меню (ImGui): `PLAYER OVERLAY`, `AIM ASSISTANCE`, `ESP`, `ITEM OVERLAY SETTINGS`,
  `MINIMAP OVERLAY`, `BETA EXTRAS`, `MENU & CONTROLS`, `FAST LOOT`,
  `Drag the radar box with the menu open`, ключи `MenuPrimary/MenuSecondary/Unload`,
  `Insert`, `GraveAccent`.
- DIAG-форматы: `{"version":"flicker","code":"DIAG","mod":"...","log":"..."}`,
  `{"version":"beta","code":"DIAG","mod":"fastloot",...}`,
  `{"ts":%llu,"version":"%s","code":"%s","flags":%lu,"addr":"%s","mod":"%s",...}`.

### 2.5. Dropbox-примеры (TrentsTuning, unknowncheats.me)

- `TrentsTuningRR.exe` v1.4.0.0 (SHA `423e8e48…`, 2.8 MB): all-in-one лаунчер.
  Извлекает в `.rr_tmp` (по умолчанию `C:\Users\Malin\Downloads\.rr_tmp`):
  `kd2.exe` (224 256), `kernel-mode.sys` (19 456), `TrentsTuningOverlay.exe`
  (2 212 352), `WebView2Loader.dll` (165 440). Проверяет SHA, маппит драйвер
  (`kd2.exe exit code: 0`, `driver ready (Key 0xF72A702C)`), запускает оверлей.
  Настройки: `menu_delay=3 menu_wait_window=1 attach_timeout=120
  reattach_attempts=3 hide_launcher=0 masquerade=1`. Маскировка: копия в
  `%TEMP%\ctfmon.exe` (`masq.txt`). Системные проверки: Win11 build 26300,
  secure boot on, hypervisor no, VBS not set, memory integrity off.
  Цель — `ROB RUN.exe` (игра), конфиг `CoreRR.cfg`, меню WebView2.
- `TrentsTuningOverlay.exe` v1.04 (SHA `799ce0fd…`): оверлей. Selftest-строки
  подтверждают офсеты S1 (см. §5). Диагностика: `--probe/--offsets/--lootdump/
  --lootset/--itemset/--cashscan/--cashfind/--xfprobe/--aimtest/--ctxread/
  --writetest/--selftest/--chamdiag/--chamtrace/--nodescan/--classscan/
  --gomdiag/--gomlist/--objlist/--chain/--inputtest/--demo/--nohide/--no-pause/
  --exe/--pid/--aio/--menu`. Фичи: ESP (box/skeleton/name/armor/distance/
  weapon/head/self/clan/police), chams, aim assist (FOV/smooth/bone/LOS/lock),
  loot (tiers/containers/doors/dropped/vehicle/weed/cargo), recoil control,
  triggerbot, radar, cash/money/safes/alarms/grenades, roulette-анализ
  (Shannon/chi-squared/transition). Драйвер блокирует запись в 0x10
  (`write to 0x10 correctly refused`) — защита IL2CPP-заголовка.
- `kernel-mode.sys` (SHA `2563d45b…`, совпадает с aio.log): минимальный драйвер
  (ntoskrnl, ZwCreateKey/OpenKey/QueryValueKey/SetValueKey, IoCreateDriver).
- `kd2.exe` (SHA `abe8b2f3…`): kdmapper + Intel (`iqvw64e.pdb`, Nal Windows
  Driver) — тот же стек, что в `RobRun.exe`.
- `Focus.exe` (84 506 624, SHA `a0ffaa1f…`, 2026-11-19): только метаданные,
  не скачивался (лимит 25 MB). Назначение неизвестно.

## 3. Flicker-баг: количественный разбор логов

Всего 35 DIAG-репортов: 31 `flicker`, 3 `fastloot`, 1 `overlays`.
Flicker: 29 drop-блоков, суммарно **167 696 ms** дропов, **12 696** кадров.
Причины по кадрам: `Bounds=8548`, `MissingFrame=3815`, `NoEligible=304`,
`InvalidCamera=16`, `NoPose=13`. `first=Bounds` в 27/29 блоках.

Разделение:
- **Пустые дропы** (`players=0`, 2 блока, 66 512 ms): `MissingFrame` +
  `InvalidCamera` — лобби/вне рейда, не баг оверлея.
- **Реальные дропы** (`players>0`, 27 блоков, 101 184 ms, 8 636 кадров):
  почти все — `Bounds=100%` при `eligible=poses=fresh` (данные свежие:
  `frameAge` среднее 4.8 ms, `boneAge` 20.4 ms, `fg==attached` — фокус на игре).
  Примеры: `t=124.43 drop=14530ms players=27 eligible=26 poses=26 fresh=26
  reasons: Bounds=1123`; `t=386.68 drop=9759ms players=4 eligible=3 poses=3
  fresh=3 reasons: Bounds=1071`.
- Фокус/окно: `noFocus>0` только в 1 дропе, `otherWin>0`/`ghost>0` в 4,
  `iconic=0`, `hwndLost=0` везде. То есть окно не теряется, иконка не сворачивается.
- Сэмплер: `boundsRej` ≈ числу ригов (127/133) — все риги отклоняются на bounds-гейте.

**Вывод:** данные (позы, камера, фокус) свежие, но bounds-гейт отклоняет всё.
Это не «игроки за камерой» (иначе не было бы 14-секундных дропов с 26 свежими
позами) и не потеря фокуса. Кандидаты на первопричину в `RobRun.exe`:
1. Неверный viewport (DPI/scaling, размер окна игры vs размер оверлея) —
   проекция падает вне bounds при любом FOV.
2. Устаревшая/неверная матрица камеры (presentation camera source) —
   `frameAge` свежий, но матрица не от текущего кадра.
3. Слишком строгий bounds (например NDC в (-1,1) без запаса + неверный aspect).
   В референсе `RobRunExternal` запас 20% (`-1.2..1.2`); если в EXE запаса нет
   и aspect неверный, все маркеры у краёв будут отбрасываться.

**Что нельзя сделать без исходников EXE:** починить bounds внутри бинарника.
**Что сделано:** `cpp/` реализует w2s с явными инвариантами (Unity LH, Y-up,
+Z forward, запас 20%, fallback базиса, отказ при NaN/Inf/за камерой) и тестами.
При переносе в оверлей сверять viewport с `GetWindowRect` игры в физических
пикселях (учесть DPI) и брать камеру из `BattleCameraModel` (+0x10/+0x1C/+0x3C/
+0x48/+0x54/+0x60, VERIFIED S1+S6). У `TrentsTuningOverlay` DPI permonitorv2 —
тот же подход.

## 4. Fast loot: работает

- `[415.79] fast loot on, speed x5, memoryWrite=1`
- `[415.79] view descriptions 0x29586362AF0, timeToCell=0.1450`
- `[415.79] patched 7 floats so far, timeToCell now 0.0290` (×5)
- `[471.19] view descriptions gone (left the raid?)` — корректный детект выхода.
- `[494.18] view descriptions 0x29586362AF0, timeToCell=0.0290` — тот же адрес,
  патч жив.

## 5. Офсеты: кросс-чек и рецепты

Источники (все проверены в этой сессии):
1. **S1** `justdoshiba/RobRunExternal@0ef798f` (2026-11-12), build 26615022,
   metadata 39, GameAssembly `03003754…65c18ab`. Полный набор: offsets.json,
   rig.json (94 кости), sdk_offsets.json, robrun_offsets.h, robrun_sdk_offsets.h.
2. **S2** `TrentsTuningOverlay.exe` v1.04 (Dropbox `/rb run/.rr_tmp`, SHA
   `799ce0fd…`, unknowncheats.me). Selftest-строки подтверждают 0x38/0x18/0x58,
   xfprobe — 0x58/0x60, static_fields 0xB8, world static `0x81B7148`
   (КОНФЛИКТ C1 с S1). Лаунчер `TrentsTuningRR.exe` v1.4.0.0 (SHA `423e8e48…`)
   содержит тот же оверлей + kdmapper/Intel. `aio.log`: Win11 build 26300,
   secure boot on, memory integrity off, `driver ready (Key 0xF72A702C)`,
   `masquerade=1` → `ctfmon.exe` в Temp. `kernel-mode.sys` SHA `2563d45b…`
   (совпадает с логом), `kd2.exe` SHA `abe8b2f3…` (kdmapper+iqvw64e).
3. **S3** UnknownCheats 779959 (catalog 2026.11.12.2) + 781224: цепочка
   UnityEntry+0x28→Game+0xD0→GameModel+0x10→BattleModel+0x90→
   BattleEntitiesModel+0x60→WorldEntitiesModel+0x18; BattleModel
   0x40/0x90/0x198/0x218/0x358/0x4D0/0x518/0x630; GOM 0x2230768;
   camera RVA 0x62801A0/0x61D23F0/0x61D1890/0x61D04B0/0x61D2AE0/0x61D2940;
   RR_CAMERA (упакованная, не BattleCameraModel) и RR_PLAYER структуры.
4. **S4** platinmods FULL DUMP 17.11.26: BattleModel/Game/GameModel/
   GlobalModel/UnityEntry/AimTargetModel/CameraState — совпадает с S3.
5. **S5** `lm3684759-lang/robrun-offsets` (dump-2026-11-22) offsets.json:
   BattleModel 0x90, BattleEntitiesModel 0x60/0xD0/0x80, WorldEntitiesModel
   0x18, BattleCharacterModel 0x10/0x38/0x40/0x48/0x58/0xB8/0xF0/0x120/
   0x138/0x150, Data 0x20/0x28, Role 0x20, IsDead 0xE0, Health 0x38,
   MaxHealth 0x28, Transform 0x10/0x18/0x20.
6. **S6** `uxxx00/Robrun-offsets-newest` (Il2CppDumper headers, 11 файлов):
   BattleEntitiesModel 0x10..0xE0 полный, BattleCameraModel
   0x10/0x1C/0x2C/0x3C/0x48/0x54/0x60 (TypeInfo 0x823C210),
   BattleModel 0x10..0x90, GameModel 0x10, Data/Common/Private/Public.
7. **S7** `satanware25-beep/robrunnn` (Il2CppDumper grep-dump):
   BattleEntitiesModel 0x10..0xE0 — совпадает с S6.
8. `aminahmcl/rob-run-orvella` — 404; `jubileeuk27-ship-it/rob-run-orvella` —
   Svelte-заглушка; `Icetuobliterate/robrun-external`, `rob-run-external`,
   `rob-run-esp` — README-спам/заглушки без офсетов. `luannavrs46-arch/robrun`
   — только README DMA. `Herkkk/Catalyst` — бинарники без исходников.
   `Focus.exe` (84 MB, SHA `a0ffaa1f…`) — только метаданные, не скачивался
   (лимит 25 MB).

Правило: значение принимается при 2+ независимых источниках или 1 источник +
живое измерение. Полевые офсеты — **VERIFIED** (S1+S2+S3+S5+S6 согласны).
Статические RVA — **КОНФЛИКТ C1** (S1 0x81D36C8/0x81D0710 vs S2 0x81B7148,
разные билды) — оба гейтятся. GOM/camera RVA — **UNVERIFIED** (S3 only).

### Рецепт верификации (для каждого офсета)

1. Скачать актуальный `GameAssembly.dll` + `global-metadata.dat` из Steam
   (ROB RUN, Unity 6000.3.18f1, IL2CPP).
2. Сверка SHA: `GameAssembly.dll` должен совпасть с
   `03003754353ff08380dc8e91920f2c77f7977b370d413083acfe9b4bc65c18ab`,
   `global-metadata.dat` — с `23bed54126a2b89e9c78cfdbd59dc3be2bc14a5817823cf057cde33984618cc9`.
   Не совпал → билд другой, офсеты из репозитория не применять.
3. Прогнать Il2CppDumper (metadata 39): сверить поле, например
   `BattleCharacterModel.PlayerBattleModel` → ожидается `0x258`;
   `WorldModel.WorldEntitiesModel` → `0x38`;
   `BattleCameraModel.Forward/Right/Up` → `0x3C/0x48/0x54`.
4. Статические глобалы: КОНФЛИКТ C1. S1: WorldModel 0x81D36C8 (1/20),
   WorldEntitiesModel 0x81D0710 (56/347), BattleCameraModel 0x81CB420 (2/17),
   BattleCharacterModel 0x8196748 (1/106). S2: world 0x81B7148.
   Не доверять вслепую: искать xref на конструктор класса в GameAssembly.dll
   (строка имени класса → метод → чтение статического поля), подтвердить в ReClass
   живым указателем. TypeInfo (S6): BattleEntitiesModel 0x8245928,
   BattleCameraModel 0x823C210 — это слоты TypeInfo, не статики, не смешивать.
5. Камера: проверить `BattleCameraModel.Position/Orientation/Forward/Right/Up`
   в ReClass на живой игре (позиция меняется при движении, forward — единичный
   вектор, right/up ортогональны).
6. Кости: риг `shared-characters_assets_all.bundle` SHA
   `68048c1c9005a267508c3820be20c33bd702df926dd9121dc4a1265a9a38ded9`,
   94 кости. Индексы в `cpp/include/robrun/esp.h` — из порядка в `robrun_rig.json`.
   Сэмплер мировых позиций костей в источнике есть, в `cpp/` — нет (UNKNOWN).

### UNKNOWN (нет в источнике)

- ViewMatrix (`Camera.main.worldToCameraMatrix`) — искать xref `worldToCameraMatrix`
  в GameAssembly.dll, читать 4×4 float в ReClass.
- Il2CppClass* синглтонов — только RVA выше; класс-указатель валидировать
  `Il2CppClass.name` в живом процессе.
- LOS/visibility для `visible_only` — в `RobRunExternal` есть `rr_los*.c`,
  в `cpp/` не перенесён.

## 6. Что исправлено в `robrun.py` (эта сессия)

- Убран второй `def main()` (первый, server-only, был затенён и недостижим).
  Единый `main()` с `--port/--data/--server-only`; `python robrun.py --help` работает.
- Все импорты подняты наверх (были 4 блока, `configure_output` использовал
  `os/k32/w` до их определения).
- Убран `Handler.__getattr__` (маскировал опечатки `do_*`); оставлены явные
  `do_GET=...=dispatch`.
- `verify_local_image()`: честное сообщение при несовпадении SHA (раньше fallback
  для 1089 всегда падал с вводящим в заблуждение текстом про LOCAL_URL).
- `patched_image()`: помечен как legacy-only (1.0.81), с объяснением про
  runtime-конструкцию URL в 1089.
- `run_bundle()`/`wait_until_ready()`: ловят `ConnectionResetError`/`TimeoutError`
  наравне с `ConnectionRefusedError` (сервер ещё стартует).
- `ShellProcess` docstring: убран stale-упоминание `--launch`.
- Поведение сервера не менялось (404 на anticheat/status сохранён — проверено логами).

## 7. Сборка overlay под Windows (рецепт, не выполнен в этой сессии)

В песочнице нет MSVC/WDK, поэтому overlay+драйвер не собирались здесь.
Рецепт для Windows-машины:
1. Visual Studio 2022 + Desktop C++ + Windows Driver Kit + WebView2 Runtime.
2. Клонировать `justdoshiba/RobRunExternal`, `build.cmd -Target All`
   → `build/overlay/RobRun.exe`, `build/driver/RobRun.sys`, `build/mapper/RobRunMapper.exe`.
3. `map-driver.cmd` от администратора, запустить ROB RUN, `run.cmd`, `F1` — меню.
4. Перед этим — верификация офсетов по §5 (иначе ESP читает мусор).
5. `cpp/` собирается тем же CMake (`cmake -S cpp -B build && cmake --build build`),
   тесты — `./build/robrun_tests`.

## 8. Статус

- Python-лаунчер: **собран и протестирован** (`py_compile` exit 0,
  `unittest` 12/12 OK, HTTP-интеграция 11/11 OK).
- C++ ядро: **скомпилировано** (`cmake` configure exit 0, build exit 0,
  `robrun_tests` 27/27 OK, g++ 12.2, C++20).
- В игре: **не проверено** (нет Windows/игры в песочнице). Поля VERIFIED,
  статики в конфликте C1, GOM/camera RVA UNVERIFIED.
- Flicker в `RobRun.exe`: **не исправлен** (нет исходников бинарника);
  диагноз и обход — в §3, корректный w2s — в `cpp/`.
