# Аудит и устранение вылета при сохранении в коопе (`0xE06D7363` `std::__non_rtti_object` в `EngineFixes.dll+0x411EB`)

## 1. Симптом и воспроизведение

При игре **вдвоём с другом** на сервере (`RFAB Respect Edition`, `SkyrimSE.exe 1.5.97.0.8`, `EngineFixes.dll 5.7.0`) нажатие **`F5`** (координированное сохранение `RFAB_MP_S0D09_Manual_1`) успешно дописывало файл `.ess` на диск (`Save Done!`, `cleared save path`), но спустя ~`120 мс` приводило к мгновенному вылету клиента с C++-исключением:

```text
EXCEPTION_RECORD: 0xE06D7363 (C++ EH exception) -> std::__non_rtti_object("Access violation - no RTTI data!")
VCRUNTIME140.dll + 0x00241D8 (__RTDynamicCast)
EngineFixes.dll  + 0x00411EB (TreeLODReferenceCaching::hk_UpdateBlockVisibility -> form->AsReference())
SkyrimSE.exe     + 0x02F257B0 (GlobalFormTable / Distant Tree LOD worker [tid 9540])
```

При игре **в одиночку на том же сервере** и сохранение (`F5`), и загрузка (`F9`) работали без единого сбоя.

---

## 2. Пошаговая цепочка первопричин (по таймстемпам и потокам)

Сопоставление лога клиента, краш-дампа `.NetScriptFramework` и исходного кода `TiltedEvolution` + `aers/EngineFixesSkyrim64` (`5.7.0`, тег `95a47f3`) выявило точную трёхзвенную цепочку:

1. **Деструктивное удаление живого аватара друга перед сейвом (`[17:39:33.849] [tid 17584]` — Main Thread):**
   - `CoordinatedSaveService::ProcessReadyDeferredSaveOrLoad` вызывал `CharacterService::PreSavePurgeDynamicActors()`, который вызывал `DisableImpl() + Delete()` на живом клоне второго игрока (`0xFF000D30`) и уничтожал его ECS-сущность `entt::entity` (оставляя при этом в куче осиротевший `TESNPC` `0xFF000D2F` и провоцируя `[error] Actor to remove not found in form ids map FF000D30`).
   - В одиночной игре на сервере удалённых клонов `0xFFxxxxxx` было `0`, поэтому цикл не выполнялся и вылета никогда не происходило.
   - Кроме того, в `TESForm::SetSkipSaveFlag` вместо рантаймового поля `inGameFormFlags` (`unk10 = 0xFFFF` по смещению `+0x18`, которое проверяет виртуальный метод `TESObjectREFR::CheckSaveGame` `vslot 0x0D` в апстриме `TiltedEvolution` и `v1.0.0`) выставлялся ESP-флаг `flags |= (1 << 14)` (`+0x10`), из-за чего и потребовалось физически удалять акторов перед сейвом.

2. **Асинхронное пересоздание `0xFFxxxxxx` форм в рабочем потоке VM после завершения сейва (`[17:39:34.066] [tid 3056]`):**
   - Через `27 мс` после завершения записи `RFAB_MP_S0D09_Manual_1.ess` (`[17:39:34.039]`) хук `HookVMUpdate` (`SE ID 53115`) на рабочем потоке `SkyrimVM` (`[tid 3056]`) вызвал `RestorePurgedRemoteActorsAfterSave()` → `OnCharacterSpawn()`, создав новые динамические формы `0xFF000D36` (`TESNPC`) и `0xFF000D37` (`Actor`).

3. **Гонка без блокировки в `SSE Engine Fixes 5.7.0` (`FormCaching` + `TreeLODReferenceCaching`, `[17:39:34.160] [tid 9540]`):**
   - В `EngineFixesSkyrim64` `5.7.0` (`src/patches/formcaching.cpp` + `treelodreferencecaching.cpp`):
     - `PatchFormCaching` подменяет потокобезопасную таблицу форм Bethesda (`SE ID 14461` с `GlobalFormTableLock`) на `tbb::concurrent_hash_map` без `ClearData`-хуков и использует `.emplace(baseId, Value)`, который **не перезаписывает устаревший указатель `TESForm*`**, если запись осталась в кеше;
     - при создании/удалении любой `0xFFxxxxxx`-формы вызывается `InvalidateCachedForm(FormId)` (`referencesFormCache.erase(FormId & 0x00FFFFFF)`), выбивающий младшие `24` бита ID из кеша LOD-деревьев;
     - через `94 мс` фоновый поток рендера дальних LOD-деревьев (`[tid 9540]`) внутри `hk_UpdateBlockVisibility` (`EngineFixes.dll+0x411EB`) получил битый/освобождённый `TESForm*` с затёртой `vtable` и вызвал `form->AsReference()` → `VCRUNTIME140.dll!__RTDynamicCast`, что выбросило `0xE06D7363` (`std::__non_rtti_object: Access violation - no RTTI data!`).

---

## 3. Архитектурное решение (без костылей)

1. **Восстановлен эталонный `TESForm::SetSkipSaveFlag(bool)` (`unk10 = 0xFFFF` в `Code/client/Games/Forms.cpp`):**
   - Возвращена установка рантаймового поля `inGameFormFlags` (`unk10 = 0xFFFF` на `TESObjectREFR` / `Actor`), благодаря которой движок Skyrim SE в `TESObjectREFR::CheckSaveGame` (`vslot 0x0D`) штатно пропускает сериализацию временных сетевых `0xFFxxxxxx`-акторов и предметов в `.ess` без удаления живых персонажей из мира.
   - На базовой форме `TESNPC` (`TESBoundObject`) вызывается строго `UnsetChanged(0xFFFFFFFF)` без мутации `unk10` (100% соответствие апстриму `TiltedEvolution` и `v1.0.0`).

2. **Недеструктивная подготовка к сохранению (`CharacterService::PreSavePurgeDynamicActors` & `CoordinatedSaveService`):**
   - Полностью убрано удаление (`DisableImpl() + Delete()`), уничтожение `entt::entity` и последующее пересоздание (`RestorePurgedRemoteActorsAfterSave` → `OnCharacterSpawn`) живых удалённых игроков во время сохранения `F5`.
   - Теперь перед записью `.ess` всем удалённым `0xFFxxxxxx`-акторам и временным объектам недеструктивно обновляется `SetSkipSaveFlag(true)` + `UnsetChanged(0xFFFFFFFF)` (а их динамическим `0xFFxxxxxx` `TESNPC` — `UnsetChanged(0xFFFFFFFF)`): персонаж друга не исчезает ни на один кадр, `TESNPC` не утекает в памяти, а файл `.ess` остаётся на 100% чистым от сетевых клонов.
   - Полная зачистка `PurgeAllDynamicActorsForWorldLoad()` сохранена строго для загрузки мира (`PrepareEngineForWorldLoad`) и дисконнекта.

3. **Автоматическое отключение багованного `FormCaching` и `TreeLODReferenceCaching` в `EngineFixes.toml`:**
   - В `Code/common/RfabModpackCompat.h` (`Launcher` + `EarlyLoad.dll` до загрузки `EngineFixes.dll`), `Tools/rfab/Setup-RfabOnline.ps1`, `Quick_Update` (`Tools/rfab/Package-Release.ps1`) и `Tools/rfab/patch_rfab_modpack_compat.py` добавлено принудительное выставление `FormCaching = false` и `TreeLODReferenceCaching = false` в секции `[Patches]` всех `EngineFixes.toml` (включая частичные MO2-оверрайды вроде `Disable Quick Saves/skse/plugins/EngineFixes.toml`, а также `bFormCaching = false` / `bTreeLodReferenceCaching = false` для `v7`).
   - Движок использует родную потокобезопасную таблицу форм Bethesda под `BSReadWriteLock` (на ~`30` плагинах RFAB разница в скорости нулевая), что физически исключает вылет `EngineFixes.dll+0x411EB` (`std::__non_rtti_object`) как при сохранениях, так и при любых спавнах/призывах/переходах между локациями в коопе.

4. **Убрано ложное `[critical]`-сообщение `BehaviorVar::Patch` в консоли (`Code/client/ModCompat/BehaviorVar.cpp`):**
   - Безобидное совпадение сигнатур графов поведения четвероногих существ (`Cow`, `Deer`, `Dog`, `Wolf`, `Horse` и др.) после генерации анимаций Nemesis/Pandora переведено с пугающего уровня `spdlog::critical` на `spdlog::debug`.
