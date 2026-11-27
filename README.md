# NZL Studio — RobRun локальный лаунчер + ESP-ядро

**NZL Studio** (фиолетовый стиль: ANSI `35/95` в консоли, фиолетовый баннер в `START.bat`).

Локальный API для `RobRun.exe` (замена удалённого сервера `95.85.230.17`
на `127.0.0.1:800`) и чистое C++ ядро ESP для ROB RUN (Unity 6 IL2CPP,
build 26615022, metadata 39 — офсеты: поля VERIFIED, статики — конфликт C1, см. `docs/ANALYSIS.md`).

## Состав

- `robrun.py` — локальный сервер + запуск `RobRun.exe` через именованный канал.
  Один файл, Python 3.10+, только stdlib.
- `RobRun.exe` — клиент (overlay, ESP, aim, kdmapper-драйвер). Бинарник, исходников нет.
- `config.ini` — конфиг чита (читается через локальный API).
- `START.bat` — запуск под Windows (поиск Python, `python -u robrun.py`).
- `cpp/` — C++20 ядро: `world_to_screen`, фильтр сущностей, конфиг с гейтами.
  Компилируется на Linux и Windows.
- `tests/test_robrun.py` — тесты лаунчера (без Windows и игры).
- `docs/ANALYSIS.md` — разбор бинарника, flicker-баг, кросс-чек офсетов, рецепты.

## Запуск (Windows)

```bat
START.bat
```

или вручную:

```powershell
python -u robrun.py                 # сервер + запуск RobRun.exe
python robrun.py --server-only      # только локальный API
python robrun.py --port 801 --data .\data
python robrun.py --help
```

Рантайм-файлы (`config.ini`, `requests.jsonl`, `crashes/`) пишутся в `data/`
(или в каталог из `--data`). В репозитории лежат копии из `data/` для анализа —
они в `.gitignore`.

Требования: Windows 10/11 x64, Python 3.10+, `RobRun.exe` рядом со скриптом.
Для драйвера: администратор, отключённые HVCI / Vulnerable Driver Blocklist
(см. строки ошибок в `RobRun.exe`).

## Сборка и тесты (Linux/Windows)

Python (без Windows):

```bash
python3 -m py_compile robrun.py
python3 -m unittest discover -s tests -v
```

C++ ядро:

```bash
cmake -S cpp -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/robrun_tests
```

## Офсеты

Все значения — в `cpp/include/robrun/config.h`, помечены UNVERIFIED
(единственный источник `justdoshiba/RobRunExternal@0ef798f`). Перед
использованием в игре — живая проверка по рецепту в `docs/ANALYSIS.md` §5
(Il2CppDumper + SHA GameAssembly.dll + ReClass).

## Статус

- Лаунчер: протестирован изолированно (12 unittest + HTTP-интеграция).
- C++ ядро: компилируется, 23 теста проходят.
- В игре: не проверено в этой сессии. Flicker в `RobRun.exe` (Bounds-гейт)
  диагностирован, но без исходников бинарника не правится — см. `docs/ANALYSIS.md` §3.

## NZL Studio с нуля (новый ESP)

Чужие оверлеи не используются. Собственная сборка — `nzl/` (C++20):
reader абстракция, chain-resolver, Windows-skeleton без драйвера.
Для текущего билда игры нужны данные с живой машины — инструкция
`docs/EXTRACT.md`, сборщик `tools/collect_nzl_info.py`.

## Транспорт NZL: Sharedmem Driver

`nzl/driver/` — драйвер общей памяти (build_driver.bat, MSVC+WDK -> driver.sys,
загрузка kdmapper от администратора), `nzl/client/` — клиент (NaInit читает
`HKLM\SYSTEM\NaCoreFilter`). NZL читает игру только через него
(`DriverMemory`). kdmapper вручную отображает драйвер, поэтому его нельзя
безопасно снять как обычную службу; он остаётся в памяти до конца сессии Windows.
Повторный запуск launcher/overlay использует уже загруженный экземпляр и не
требует перезагрузки. Перезагрузка нужна только для замены/снятия `driver.sys`;
не запускай kdmapper повторно поверх работающего экземпляра.
