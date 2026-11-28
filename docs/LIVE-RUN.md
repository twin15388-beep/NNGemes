# NZL Studio — живой прогон (порядок, ожидаемые строки, условия остановки)

Этот файл существует потому, что три раунда подряд уходили на выяснение того,
что уже лежало в логах на диске. Здесь ровно один порядок действий и точные
строки, по которым видно, что шаг прошёл.

## 0. Исходное состояние (проверено 2026-11-28 15:31)

```
BuildAll exit=0
100% tests passed out of 5          # nzl_tests, pipeline, profile, skeleton, protocol
driver.sys 18432 bytes  sha256 D3AB9214...
sc_drv.c(280): warning C4996 ExAllocatePoolWithTag   # осознанно, не ошибка
```

Сборка и host-тесты зелёные. **В живом процессе не проверено ничего.**

## 1. Порядок

PowerShell **от администратора**. Свежее админское окно стартует в
`C:\Windows\system32`, поэтому сначала перейди в проект, а диспетчер зови
через ` .\ ` — в PowerShell кавычки в начале строки это строка, а не команда,
и полный путь требует оператора `&`:

```powershell
cd "D:\NZL Rob run"
.\NZL.bat build      # драйвер + приложения + host-тесты
.\NZL.bat load       # загрузить драйвер (до запуска игры)
.\NZL.bat probe      # транспорт + цепочка (игра в матче)
.\NZL.bat overlay    # ESP, INSERT = меню, END = выход
.\NZL.bat report     # отчёт с -Zip -CopyToDropbox
```

Из другой папки, в PowerShell: `& "D:\NZL Rob run\NZL.bat" overlay`.
В cmd.exe кавычек достаточно: `"D:\NZL Rob run\NZL.bat" overlay`.

`report` без аргументов сам добавляет `-Zip -CopyToDropbox`.
Эквивалент вручную, тоже из любой папки:

1. `powershell -NoProfile -ExecutionPolicy Bypass -File "D:\NZL Rob run\tools\NZL-SetKernelDump.ps1"`
   → если пишет `Kernel dump уже настроен` — ничего не делать; если меняет
   настройки — `Restart-Computer` и начать заново.
2. `powershell -NoProfile -ExecutionPolicy Bypass -File "D:\NZL Rob run\tools\NZL-LoadDriver.ps1"`
   Скрипт сам откажется работать, если `CrashDumpEnabled != 2` (exit 9).
3. Запустить игру через Steam, дойти до **матча** (не меню).
4. `powershell -NoProfile -ExecutionPolicy Bypass -File "D:\NZL Rob run\tools\NZL-Probe.ps1"`
5. `powershell -NoProfile -ExecutionPolicy Bypass -File "D:\NZL Rob run\tools\NZL-Overlay.ps1"`
6. `powershell -NoProfile -ExecutionPolicy Bypass -File "D:\NZL Rob run\tools\NZL-Report.ps1" -Zip -CopyToDropbox`

Оверлей: INSERT — меню, END — выход. Режим экрана в игре должен быть
**Windowed / Borderless**: layered-окно не рисуется поверх exclusive
fullscreen, там композиция рабочего стола обходится стороной.

## 2. Ожидаемые строки по шагам

`NZL-LoadDriver.ps1`:
```
Kernel dump включён (CrashDumpEnabled=2): OK
Translated... -> TRANSPORT_LOADED: sharedmem-драйвер жив, ключ опубликован.
```

`nzl_probe.txt` (пишется рядом с EXE, `build-nzl\Release\nzl_probe.txt`):
```
[probe] NaInit OK (sharedmem transport alive)
[probe] exe: MZ=0x5a4d e_lfanew=0x110 PE=0x4550 -> OK
[probe] NaGetModule (PEB walk): GameAssembly=0x... UnityPlayer=0x... (+Nms)   <- ОЖИДАЕТСЯ <100 ms
[probe] modules: GameAssembly=0x... UnityPlayer=0x... (source: PEB walk)
[probe] transport result: TRANSPORT_OK
[profile] loaded SizeOfImage=...; profile TypeInfo RVAs fit; Game.GameModel=+0x...
[root] scan calls=... / [root] ROOT_OK ...
```

Ключевые числа: GameAssembly `2dc33269...` (совпадает с профилем), SizeOfImage
из загруженного PE. Если `NaGetModule` вернул `0x0` — probe сам уйдёт в
регион-скан, это видно по `source: region scan / snapshot` и по времени (+Nms
будет в тысячах).

## 3. Условия остановки — не продолжать

- `TRANSPORT_FAIL` — драйвер отвечает, но чтение не проходит. Не запускать overlay.
- `[profile] STOP: GameAssembly fingerprint mismatch` — профиль не от этого билда.
  Офсеты не применять, прислать `nzl_probe.txt`.
- `[root] STOP: scan cursor did not advance` — регрессия resume-VA в скане
  (та самая, что чинилась). Немедленно остановиться, это новый баг.
- BSOD — **не перезапускать драйвер**, сначала `tools\NZL-CrashInfo.ps1` и
  `C:\Windows\MEMORY.DMP` (kernel dump включён именно для этого).
  Расшифровка по коду: `0x1E` + `p3=1` + `p4=0x68` — запись в поле
  `DriverUnload` объекта драйвера, который manual map не создавал (было
  2026-11-28 15:45, закрыто). `0x1E`/`0x50` + `p1` пользовательского вида —
  чтение/запись по адресу из клиента без проверки диапазона.

## 4. Что присылать после прогона

Только `nzl_report_<штамп>.zip` из Dropbox. Внутри уже есть `nzl_probe.txt`
(свежий, из `build-nzl\Release`), весь `LastTest.log`, `driver_build.log`,
BugCheck-события и хеши `driver.sys`. Отдельно ничего копировать не нужно.

## 4a. Что подтверждено живым прогоном 2026-11-28 16:27

```
TypeInfo Game/GameModel/BattleModel/BattleEntities/WorldEntities/BattleCamera/
BattleCharacter ............ NAME_OK (все семь)
[root] ROOT_OK Game=0x1F342019F00 GameModel=0x10x... Dict=0x... Camera=0x...
[profile] roster dictionary Count=9 array_capacity=17
[camera] vectors=OK pos=(-41.10,7.42,-30.88) fwd=(0.10,-0.40,-0.91)
[camera] projection=EXACT offset=0x... m00=... m11=... fovY=...
[snapshot] roster_count=9 valid_pos=3 drawn=1 draw_commands=2
  #23844 alive pos=(-40.21,3.94,-35.77) dist=6.1m
  entry[0] id=23844 entity=0x... class=BattleCharacter
  entry[1] id=24157 entity=0x... class=BattleCharacter
[snapshot] covered=9 of roster=9, with_position=9
[sweep] WorldEntitiesModel=0x... fields 0x0..0x200
  +0x018 -> 0x... System.Collections.Generic.Dictionary`2
    dict count=41 capacity=61
    slot[3] 0x... LootModel
[sweep] named_fields=6
```

`[camera] projection=EXACT` — самый важный признак: угол обзора и пропорции
берутся из матрицы самой игры, а не из предположения про 90°. Если вместо этого
`projection=NONE`, боксы будут смещены к краям экрана, и это надо лечить
отдельно (значит, матрицы в модели камеры нет и нужен другой источник).

`entry[...]` печатается по одной строке на каждого игрока из ростера: адрес
объекта и его имя класса. Если `covered` меньше `roster_count` или рядом стоит
`NOT-A-POINTER` / `class=<unreadable>` — это ответ на «не всех видит», и он
уходит в отчёт как есть.

`[sweep]` перебирает поля `WorldEntitiesModel` с шагом 8 и печатает класс
за каждым читаемым указателем. Словари печатаются с `count/capacity` и
несколькими значениями — по их именам видно, какое поле держит лут, и это
единственный способ получить офсет добычи из живой игры, а не из чужого EXE.

Этого не хватало для ESP: транспорт, PEB-walk, профиль, имена классов, корень
цепочки, словарь и камера читаются из живой игры за 55 вызовов скана.
Не подтверждено визуально: рисуется ли текст/линии там, где ожидается (FOV
считается как 90.0, потому что поле матрицы проекции неизвестно).

## 5. Чего в этом файле нет и почему

Live-значения `SKEL raw`, никнеймы из словаря слотов, машины и loot не
подтверждены ни одним прогоном. Проба проверяет транспорт и корень цепочки;
она не доказывает, что найденные офсеты дают правильные координаты. Это
следующий этап — уже после зелёного probe.
