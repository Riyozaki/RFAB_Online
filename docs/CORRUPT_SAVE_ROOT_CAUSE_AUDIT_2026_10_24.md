# Аудит и полное устранение ошибки «Файл сохранения поврежден и не может быть загружен.» (`sSaveGameCorrupt`) — 2026-10-24

## 1. Симптом со скриншота плейтеста (`image-1.png`)

В левом верхнем углу экрана Skyrim SE `1.5.97` отображается системное уведомление движка:
> **`Файл сохранения поврежден и не может быть загружен.`** (`sSaveGameCorrupt`)

Причём ошибка воспроизводится **даже в одиночной игре на 1-м уровне персонажа (`LVL 1`)** (Коллегия Винтерхолда, без активной кооп-сессии) сразу после установки `RFAB Online`, а также при попытке загрузить сохранение через `Esc → Загрузить` во время активного кооператива.

---

## 2. Доказанные первопричины (Root Causes)

### Первопричина №1 (Главная причина реального повреждения `.ess` и сбоя загрузки в соло/коопе): конфликт `SaveGameMaxSize = true` + `MemoryManager = false` в `SSE Engine Fixes` v5.x / v6.0.2

1. **Почему `RFAB Online` отключает `MemoryManager` в `EngineFixes.toml`:**
   - В `Code/client/Games/Memory.cpp` (`HookFormAllocate` на `SE ID 66859` / `514110`) клиент расширяет размер аллокации `Actor` (`0x2B0`) и `PlayerCharacter` (`0xBE0`) до `ExActor` и `ExPlayerCharacter`, а также перехватывает CRT IAT через `mimalloc`. Поэтому в `EngineFixes.toml` обязательно выставляется `MemoryManager = false` и `ScaleformAllocator = false`.
2. **Что делает `SSE Engine Fixes` v5.x / v6.0.2 (поставляемый в `drake1ch/RFAB` под Skyrim SE `1.5.97`):**
   - В оригинальной сборке `drake1ch/RFAB` (`MO2/mods/SSE Engine Fixes/skse/plugins/EngineFixes.toml` и `MO2/mods/Disable Quick Saves/skse/plugins/EngineFixes.toml`) автор RFAB штатно держит **`[Experimental] SaveGameMaxSize = false`** (`64 МБ` несжатого буфера = `~15–22 МБ` LZ4 `.ess`).
   - В предыдущей ревизии наши патчеры (`RfabModpackCompat.h`, `Setup-RfabOnline.ps1`, `patch_rfab_modpack_compat.py`) принудительно записывали `MemoryManager = false` **и одновременно `SaveGameMaxSize = true`**.
   - Проверка исходного кода `aers/EngineFixesSkyrim64` (`tag 6.0.2`, `src/patches/miscpatches.cpp` → `PatchSaveGameMaxSize()`) показала критическую особенность версий 5.x/6.x: в отличие от ветки 7.x (`save_game_max_size.h`, где `aers` специально добавил защиту `if (!bOverrideScrapHeap.GetValue()) return;`), **в `EngineFixes` 5.x/6.x нет проверки `MemoryManager`!**
   - В результате `PatchSaveGameMaxSize()` безусловно патчит `Win32FileType_Ctor` (`SE 101962 + 0x14B`), `Win32FileType_CopyToBuffer` (`SE 101985 + 0x11`) и `ScrapHeap_GetMaxSize` (`SE 35203 + 0x1`) с `0x04` (`64 МБ`) на **`0x08` (`128 МБ` = `0x08000000`)**, тогда как при `MemoryManager = false` ванильный `ScrapHeap` резервирует через `VirtualAlloc(MEM_RESERVE)` ровно **`64 МБ` (`0x04000000`)**!
   - При каждом сохранении или загрузке `Win32FileType` запрашивает `ScrapHeap::Allocate(128 МБ)` у 64-мегабайтного `ScrapHeap`, получает **`nullptr`**, записывает на диск только небуферизованный заголовок `"TESV_SAVEGAME"` + RGBA-скриншот (без сжатого тела форм!) и выдаёт в HUD **`«Файл сохранения поврежден и не может быть загружен.»`**!

### Первопричина №2 (Почему ранее созданные обрезанные `.ess` не попадали в карантин): поверхностная проверка `Detail::IsValidEssFileOnDisk`

1. Skyrim SE записывает в `.ess` магическую сигнатуру `"TESV_SAVEGAME"` (`13` байт), `headerSize` (`4` байта), метаданные заголовка и несжатый RGBA-скриншот (`4 × width × height` ≈ `100–350 КБ`) **до** сброса сжатого LZ4-буфера `Win32FileType`.
2. Старая реализация `IsValidEssFileOnDisk` проверяла только `fileSize >= 256` и первые 13 байт `"TESV_SAVEGAME"`, поэтому файлы с валидным заголовком и скриншотом, но с `0` байт сжатого тела оставались в папке `Saves\`, выбирались при нажатии `F9` (`LoadMostRecentSaveGame`) и вызывали `«Файл сохранения поврежден и не может быть загружен.»`.

### Первопричина №3 (Ложное сообщение `sSaveGameCorrupt` при перехвате загрузки в коопе): возврат `false` из внутреннего `HookLoadCallSite` (`0x0058AE30 + 0x26C`)

1. В `Code/client/Games/Skyrim/SaveLoad.cpp` хук `HookLoadCallSite` стоял на внутреннем `E8`-вызове `0x0058AE30 + 0x26C` (`BGSSaveLoadGame::LoadGame` внутри `BGSSaveLoadManager::LoadEntry` `0x0058AE30`).
2. При загрузке через меню паузы `Esc → Загрузить` (`JournalMenu`, маска задачи `0xD0000400`) поток обходил `HookLoadImpl` (`34819`) и `HookQueueSaveLoadTask` (`34859`, где перехватывались только `0xD0000100`/`0xD0000200`), входил внутрь `0x0058AE30` и получал `return false` от `HookLoadCallSite` (`+0x26C`).
3. Функция `0x0058AE30` интерпретирует `false` от вызова по смещению `+0x26C` исключительно как повреждение `.ess`-файла и немедленно выводит в HUD `sSaveGameCorrupt` (`«Файл сохранения поврежден и не может быть загружен.»`).

---

## 3. Реализованное чистое архитектурное решение

1. **Автоматическое исправление `SaveGameMaxSize = false` на всех уровнях:**
   - В `Code/common/RfabModpackCompat.h` (`PatchEngineFixesTomlContent` + `EnsureEngineFixesPatchedUnderRoot`), `Tools/rfab/Setup-RfabOnline.ps1`, `Tools/rfab/Package-Release.ps1` (`Update-Quick.ps1`) и `Tools/rfab/patch_rfab_modpack_compat.py` принудительно выставляется **`SaveGameMaxSize = false`** (для EngineFixes v5/v6) и `bOverrideScrapHeap = false` / `iSaveGameMaxSize = 64` (для v7).
   - Поскольку `Launcher.cpp` и `DllBlocklist.cpp` вызывают `EnsureEngineFixesPatchedUnderRoot` **до загрузки `SkyrimSE.exe` и `EngineFixes.dll`**, любой ранее пропатченный `EngineFixes.toml` (включая `MO2\mods\SSE Engine Fixes`, `MO2\mods\Disable Quick Saves`, `MO2\overwrite` и `Data\SKSE\Plugins`) автоматически лечится при первом же запуске игры или апдейтера.
   - В `SaveLoad.cpp` дополнительно вызывается CRT `_setmaxstdio(8192)` напрямую на `ucrtbase.dll` / `api-ms-win-crt-stdio-l1-1-0.dll`.
2. **Полная бинарная валидация `.ess` по спецификации Skyrim SE v12 и авто-карантин:**
   - `RfabCompat::Detail::IsValidEssFileOnDisk` (а также `Setup-RfabOnline.ps1` и `Update-Quick.ps1`) теперь полностью разбирает структуру заголовка Skyrim `.ess` (`headerSize`, `version`, `wstring16`-поля, `ssWidth`, `ssHeight`, `compressionType`), вычисляет точное смещение `bodyOffset = 17 + headerSize + bpp * ssWidth * ssHeight` и проверяет `uncompressedLen >= 64`, `compressedLen >= 16` и `bodyOffset + 8 + compressedLen <= fileSize`.
   - Автоматический карантин (`Quarantined_Corrupt_v100`) сканирует как стандартную папку `Documents\My Games\Skyrim Special Edition\Saves` (включая подпапки профилей персонажей), так и локальные сохранения профилей `MO2\profiles\*\saves`.
3. **Чистый перехват загрузки на внешнем прологе `0x0058AE30` (`HookLoadEntry`):**
   - Добавлен внешний пролог-хук `HookLoadEntry` (`0x0058AE30`), который безопасно извлекает имя выбранного слота из `BGSSaveLoadFileEntry::fileName` (`+0x00`, с проверкой `VirtualQuery`) и вызывает `HandleEngineLoadAttempt(saveName)` **до входа в тело `0x0058AE30`**.
   - Из внутреннего `HookLoadCallSite` (`0x0058AE30 + 0x26C`) полностью убран синтетический `return false`: `+0x26C` теперь вызывается только для разрешённых загрузок, чтобы выполнить `PrepareEngineForWorldLoad` перед `kMessage_PreLoadGame`.
   - В `ExecuteDirectLoadFromProcessEvents` добавлена предварительная проверка `VerifySavedSlotOnDisk`.
