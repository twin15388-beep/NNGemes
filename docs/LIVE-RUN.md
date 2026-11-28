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

Всё из `D:\NZL Rob run`, PowerShell **от администратора**.

1. `powershell -ExecutionPolicy Bypass -File tools\NZL-SetKernelDump.ps1`
   → если пишет `Kernel dump уже настроен` — ничего не делать; если меняет
   настройки — `Restart-Computer` и начать заново.
2. `powershell -ExecutionPolicy Bypass -File tools\NZL-LoadDriver.ps1`
   Скрипт сам откажется работать, если `CrashDumpEnabled != 2` (exit 9).
3. Запустить игру через Steam, дойти до **матча** (не меню).
4. `powershell -ExecutionPolicy Bypass -File tools\NZL-Probe.ps1`
5. `powershell -ExecutionPolicy Bypass -File tools\NZL-Report.ps1 -Zip -CopyToDropbox`

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

## 4. Что присылать после прогона

Только `nzl_report_<штамп>.zip` из Dropbox. Внутри уже есть `nzl_probe.txt`
(свежий, из `build-nzl\Release`), весь `LastTest.log`, `driver_build.log`,
BugCheck-события и хеши `driver.sys`. Отдельно ничего копировать не нужно.

## 5. Чего в этом файле нет и почему

Live-значения `SKEL raw`, никнеймы из словаря слотов, машины и loot не
подтверждены ни одним прогоном. Проба проверяет транспорт и корень цепочки;
она не доказывает, что найденные офсеты дают правильные координаты. Это
следующий этап — уже после зелёного probe.
