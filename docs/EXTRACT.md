# NZL Studio — что вытащить с живой игры (обновлённый античит, свой билд с нуля)

Чужие оверлеи/драйверы НЕ используем. Собираем своё, поэтому для текущего
билда нужны данные ниже. Без них офсеты для нового билда — гадание.

## 1. Обязательно (5 минут, игра может быть выключена)
Запусти на Windows в папке проекта:
```
python tools\collect_nzl_info.py --game-dir "ПУТЬ_К_ROB_RUN" --out nzl_info
```
Пришли мне папку `nzl_info` (zip). Внутри `report.json`: SHA-256 и размеры
`GameAssembly.dll`, `UnityPlayer.dll`, exe игры, `global-metadata.dat`.
Это fingerprint билда — по нему пойму, совпадает ли игра со старыми дампами.

Путь игры в Steam: Библиотека → ROB RUN → ПКМ → Управление → Просмотреть
локальные файлы. Там же: Свойства → Установленные файлы (версия/дата обновления — напиши текстом).

## 2. Il2CppDumper (обязательно, 5–10 минут)
1. Скачай Il2CppDumper (Perfare/Il2CppDumper, последний релиз) с GitHub.
2. Запусти: `Il2CppDumper.exe GameAssembly.dll global-metadata.dat`
   (файлы из папки игры; metadata обычно `ROB RUN_Data\il2cpp_data\Metadata\global-metadata.dat`).
3. Пришли: `dump.cs`, `script.json`, `il2cpp.h` (можно zip, они большие — через Dropbox/Google Drive).

По ним я сам вытащу для НОВОГО билда: UnityEntry, Game, GameModel, BattleModel,
BattleEntitiesModel, WorldEntitiesModel, BattleCharacterModel, BattleCameraModel
и все поля + TypeInfo/static_fields.

## 3. Живая проверка цепочки (обязательно, в матче, 5 минут)
Нужна, чтобы подтвердить корень (старые static RVA конфликтуют: C1).
Вариант А (проще): дождись моего probe-билда NZL — он сам проверит и напишет `nzl_probe.txt`.
Вариант Б (вручную, ReClass.NET x64): запусти игру, зайди в матч, приаттачься к `ROB RUN.exe`,
проверь и запиши значения/скриншоты:
- Модули: базы `GameAssembly.dll` и `UnityPlayer.dll`.
- Камера: позиция меняется при движении? forward/right/up — единичные, ортогональны?
- Словарь персонажей: count > 0 в матче, 0 в лобби?
Что именно смотреть — напишу адресно после п.1–2, до этого адресов для нового билда нет.

## 4. Если Il2CppDumper падает (metadata зашифрована/новая версия)
Пришли: текст ошибки дословно + первые 64 байта `global-metadata.dat` hex
(PowerShell: `Format-Hex -Path ... -Count 64`) + версию Unity из
`ROB RUN_Data\globalgamemanagers` (строка `6000.x` ищется поиском по файлу).

## 5. Что НЕ надо делать
- Не запускай чужие kdmapper/драйверы/оверлеи (TrentsTuning и т.п.) — античит обновили.
- Не присылай `RobRun.exe`, логин/пароль Steam, дамп всей памяти процесса.
- Не меняй файлы игры.

## Что я делаю после получения п.1–2
1. Сверяю SHA с известными билдами, строю новый `offsets` только под твой билд.
2. Вношу в `nzl/` (свой reader + chain resolver уже есть, тесты на FakeMemory).
3. Даю probe-билд: он валидирует корень по имени класса перед чтением ESP.
4. Только после probe=OK собираем overlay (D3D11, фиолетовый NZL Studio).
