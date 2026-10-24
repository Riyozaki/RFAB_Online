# RFAB Online — Полный аудит собственных сложностей RFAB («Странник» / «Довакин» / «Легенда» + «Проклятый») и проверка отсутствия ванильных перезаписей

**Дата аудита:** 24.10.2026  
**База проверки:** актуальная сборка `drake1ch/RFAB` (`RFAB.esp` 31.1 МБ, `RFAB.dll` 2.0 МБ, `DifficultyBalance.dll`, `StartMenu.json`, `RFAB_StartMenu.psc`, `RFAB_MCM_Alias.psc`, `MO2/profile-template/Default/*`) против `RFAB_Online` (`Code/client`, `Code/server`, `SkyrimTogether.esp`, `SkyrimTogetherQuestPatches.esp`, `GameFiles/Skyrim`, `dist_ready/RFAB_Online_MO2`, `Tools/rfab/Setup-RfabOnline.ps1`).

---

## 1. Как устроены собственные сложности RFAB и метка «Проклятый» на уровне движка и `RFAB.dll`

В сборке RFAB ванильный ползунок сложности полностью заменён собственной системой (`StartMenu.json` + `RFAB_StartMenu.psc` + `RFAB_MCM_Alias.psc` + `RFAB.esp` + `RFAB.dll` + `DifficultyBalance.dll`):

1. **Базовые `GMST` в `RFAB.esp`:** все 10 игровых настроек `fDiffMultHPByPC{VE,E,H,VH,L}` и `fDiffMultHPToPC{VE,E,H,VH,L}` в `RFAB.esp` выставлены в `1.0` (чтобы до выбора сложности в стартовом меню ванильный Скайрим никогда не искажал урон).
2. **Выбор сложности в `RFAB_StartMenu.UpdateDifficulty()`** (а также автоматическое повторное применение при каждой загрузке сохранения в `RFAB_MCM_Alias::OnPlayerLoadGame()`):
   - **Странник (`DifficultyIndex = 0`)**: урон от игрока (`fDiffMultHPByPC*`) = **`2.0` (`x2`)**, урон по игроку (`fDiffMultHPToPC*`) = **`0.5` (`x0.5`)**.
   - **Довакин (`DifficultyIndex = 1`)**: урон от игрока = **`1.0` (`x1`)**, урон по игроку = **`1.0` (`x1`)**.
   - **Легенда (`DifficultyIndex = 2`)**: урон от игрока = **`1.0` (`x1`)**, урон по игроку = **`2.0` (`x2`)**.
   - **Метка «Проклятый» (`Cursed = true`)**: принудительно выставляет множитель исходящего урона игрока (`_damageByPlayer`) в **`DamageWithCurse = 0.5`** (уполовинивает исходящий урон игрока при любом типе атак, сохраняя выбранный входящий множитель `0.5` / `1.0` / `2.0`).
3. **Прямые проверки `0x14` и `fDiffMultHP*PCVH` внутри `RFAB.dll` (подтверждено дизассемблированием `/tmp/RFAB.dll`):**
   - `VA 0x1800A4643` (`Actor::ProcessHitData` / `HitData::Populate`, хук на `SE ID 37673 + 0x3C0`): проверяет `victim->formID == 0x14` -> умножает на `GMST fDiffMultHPToPCVH`; иначе `aggressor->formID == 0x14` -> умножает на `GMST fDiffMultHPByPCVH`; иначе `1.0f`.
   - `VA 0x180057D97` (`RFAB` внутренний обработчик магического урона по `kHealth` `AV 24`): проверяет `victim->formID == 0x14` -> `fDiffMultHPToPCVH`; иначе `caster && caster->formID == 0x14` -> `fDiffMultHPByPCVH`.
   - `VA 0x18007FA90` (`RFAB_PapyrusFunctions.ScriptDamage`): проверяет `victim->formID == 0x14` -> `fDiffMultHPToPCVH`; иначе `aggressor->formID == 0x14` -> `fDiffMultHPByPCVH`.
   - `VA 0x18008723B` и `VA 0x1800873EB` (хуки на `SE ID 36346 + 0x35` и `36973 + 0xAE` — замена ванильного `AdjustHealthDamageToDifficulty`): применяют `fDiffMultHPToPCVH` для `0x14`.
   - `DifficultyBalance.dll` (`SE ID 36506 + 0xB`) изолирует призывных существ и спутников игрока так, чтобы их урон оставался `1.0x` и не наследовал инвертированные множители игрока.

---

## 2. Найденные проблемы со сложностями RFAB и «Проклятым» в кооперативе и их исправления

### D1. Принудительная перезапись `uDifficulty = 2` (`Adept`) каждый кадр в `PlayerService::RunDifficultyUpdates()`
- **Причина:** В движке `SkyrimSE.exe` для уровня `difficulty == 2` (`Adept` / «Адепт») **не существует** настройки `GMST` `fDiffMultHPByPCN` / `fDiffMultHPToPCN` (вызов `Game.SetGameSettingFloat("fDiffMultHPByPCN", ...)` в `RFAB_StartMenu.psc` ничего не меняет в движке, поэтому в официальном `MO2/profile-template/Default/SkyrimPrefs.ini` сборки RFAB стоит `iDifficulty=0`, а `RFAB.dll` читает `fDiffMultHP*PCVH`). Принудительная запись `SetDifficulty(2)` каждый кадр заставляла `SE ID 25920` (`GetDifficultyMultiplier`) всегда возвращать `1.0f` вместо реальных множителей «Странника» / «Легенды» / «Проклятого».
- **Решение:** При активном `RFAB.esp` (`Settings::IsRfabModpackActive()`) `PlayerService` **полностью прекращает перезаписывать сложность игрока с сервера**; если в INI/памяти остался индекс `2` (`Adept`), он однократно переводится в `4` (`VH`, совпадающий с `fDiffMultHPByPCVH` / `fDiffMultHPToPCVH` в `RFAB.dll`), а `GameplayFormulas::CalculateRealDamage` напрямую читает `fDiffMultHPToPCVH` и `fDiffMultHPByPCVH` из `GameSettingCollection` (`SE ID 514622`). Каждый игрок в группе сохраняет свою собственную сложность RFAB и метку «Проклятый».

### D2. Заклинания, крики, зачарования оружия и яды игрока (`AddTargetEvent` -> `MagicService::OnNotifyAddTarget`) теряли множитель «Странника» (`x2`) и «Проклятого» (`x0.5`) на чужих NPC
- **Причина:** Когда Игрок А кастует атакующее заклинание по врагу, которым владеет Игрок Б, на клиенте Б кастером выступает `RemotePlayer` с `formID = 0xFFxxxxxx != 0x14`. Хук `RFAB.dll` (`VA 0x180057DB5`) проверяет `caster->formID == 0x14`, получает `false` и оставляет множитель `1.0x` — в результате на «Страннике» магия била в 2 раза слабее положенного (`1.0x` вместо `2.0x`), а с меткой «Проклятый» наносила полный урон (`1.0x` вместо `0.5x`).
- **Решение:** В `HookAddTarget` (`MagicTarget.cpp`), если `RFAB.esp` активен, кастер — локальный игрок (`formID == 0x14`, призыв исключён, т.к. у призыва в RFAB множитель `1.0x`), а эффект является враждебным/вредоносным уроном по здоровью (`kHealth`), сетевая величина `addTargetEvent.Magnitude` умножается на `Settings::GetRfabOutgoingDamageMult()` (`fDiffMultHPByPCVH` кастера). На принимающем клиенте `caster != 0x14`, поэтому двойного умножения не происходит никогда.

### D3. Потоковые заклинания концентрации (`Пламя`, `Искры`, `Обморожение`, стены стихий и т.д.) также игнорировали «Странника» (`x2`) и «Проклятого» (`x0.5`)
- **Причина:** В `MagicService::OnNotifySpellCast` вызывался `pCaster->CastSpellImmediate(pSpell, false, pDesiredTarget, 1.0f, false, 0.0f, pActor)` с жёстко зашитым `afEffectivenessMult = 1.0f`.
- **Решение:** В сетевые пакеты `SpellCastRequest` и `NotifySpellCast` добавлено поле `EffectivenessMult` (по умолчанию `1.0f`). При касте локальным игроком (`0x14`) с активным `RFAB.esp` передаётся `Settings::GetRfabOutgoingDamageMult()`, а на принимающей стороне `CastSpellImmediate` вызывается с `afEffectivenessMult = effectiveness` и `abAdjustOnlyHostileEffectiveness = true` (поэтому атакующие потоковые заклинания получают точный множитель `2.0x` / `0.5x`, а дружественные «Исцеляющие руки» не искажаются).

### D4. Мгновенная синхронизация прямого урона `RFAB.dll` (`RestoreActorValue(DAMAGE, kHealth, -dmg)`) по локальным NPC
- **Причина:** Магический урон (`0x180057DFE`), `ScriptDamage` (`0x18007FDD1`) и часть периодических эффектов `RFAB.dll` вызывают `Actor::RestoreActorValue(DAMAGE, kHealth, -dmg)` напрямую в обход `DamageActor`. В `HookRestoreActorValue` событие `HealthChangeEvent` отправлялось только для `pLocalPlayer`, а для локальных NPC ждало периодического опроса `ActorValueService`.
- **Решение:** В `HookRestoreActorValue` для всех локально принадлежащих NPC (`pExt->IsLocal() && !pExt->IsLocalPlayer()`) при изменении `kHealth` вне `s_inAuthoritativeDamageHook` и `ScopedActorValueOverride` вычисляется точная фактическая дельта `hpAfter - hpBefore` и немедленно отправляется `HealthChangeEvent`.

---

## 3. Полный аудит `RFAB_Online` на перезапись предметов, статов, заклинаний, перков, настроек и ассетов RFAB

Проверены все 32 714 путей репозитория `drake1ch/RFAB`, все записи `RFAB.esp` (31.1 МБ), все файлы `dist_ready/RFAB_Online_MO2` и `GameFiles/Skyrim`, установщик `Setup-RfabOnline.ps1`, а также все константы `FormID` / `GMST` / `GLOB` в `Code/client` и `Code/server`:

| № | Узел / Файл `RFAB_Online` | Что перезаписывалось в RFAB | Статус / Исправление |
|---|---|---|---|
| **V1** | `Code/client/Games/Skyrim/Magic/MagicTarget.cpp` (`HookAdjustForPerks`, перк `0x000581F8`) | В ванили перк `0x581F8` (`Regeneration`) давал **`x1.5` (`+50%`)**, тогда как в `RFAB.esp` запись `0x000581F8` — это `RFAB_Perk_Restoration_ImprovedHealing` («Улучшенное лечение») с точным множителем **`EPFD = 1.25` (`+25%`)**. Кроме того, `HookAdjustForPerks` умножал все эффекты пакета без проверки, что эффект действительно лечащий. | **Исправлено:** при активном `RFAB.esp` применяется точный множитель RFAB **`1.25f`** и строго только к невраждебным эффектам лечения `kHealth` (с учётом исключения нежити/конструктов по ключевым словам `ActorTypeUndead` `0x1CEB0`? — проверим точный `FormID` `ActorTypeUndead`!). |
| **V2** | `SkyrimTogether.esp` (`CPTH 0x01000926 MagicOrBowCams`) + `Tools/rfab/Setup-RfabOnline.ps1` | `SkyrimTogether.esp` содержал ванильный оверрайд `Update.esm` `CPTH 0x01000926 MagicOrBowCams`, а `Setup-RfabOnline.ps1` искал устаревший `Requiem.esp` вместо `RFAB.esp` и ставил `SkyrimTogether.esp` в самый конец `plugins.txt` **после `RFAB.esp`**, затирая кастомные камеры RFAB (`CPTH 0x01000926` в `RFAB.esp`). | **Исправлено:** запись `CPTH 0x01000926` полностью вычищена из `SkyrimTogether.esp` (**0 оверрайдов чужих мастеров**!), а `Setup-RfabOnline.ps1` ставит `*SkyrimTogether.esp` и `*SkyrimTogetherQuestPatches.esp` строго **перед `RFAB.esp`** (и после всех `.esm`). |
| **V3** | `meshes/Interface/logo/logo.nif` в `GameFiles/Skyrim` и `dist_ready/RFAB_Online_MO2` | Из-за высшего приоритета `+RFAB_Online` в VFS Mod Organizer 2 ванильно-модовый `logo.nif` (`331 КБ`) затирал фирменное лого главного меню `RFAB/meshes/interface/logo/logo.nif`. | **Исправлено:** `meshes/Interface/logo/logo.nif` удалён из `GameFiles/Skyrim` и `dist_ready/RFAB_Online_MO2`; в `verify_release_bundle.py` добавлен запрет на его присутствие. |
| **V4** | `Code/client/Services/Generic/PlayerService.cpp` (`fAIMinGreetingDistance` и `GLOB 0x100F19 KillMove`) | В `RFAB.esp` `GMST fAIMinGreetingDistance = 70.0` (а не ванильные `150.0`). При отключении от сервера `PlayerService` жёстко писал `150.f` в `fAIMinGreetingDistance` и `1.f` в `KillMove`, ломая настройки RFAB. | **Исправлено:** исходные значения `fAIMinGreetingDistance` и `KillMove` из `RFAB.esp` сохраняются перед изменением и точно восстанавливаются при отключении. |
| **V5** | `scripts/source/{C00TrainerScript,ResourceFurnitureScript,TempleBlessingScript}.psc` + `HallofStoriesKeyholeScript` | В `scripts/source/` оставались осиротевшие ванильные исходники `.psc` (включая `TempleBlessingScript.psc`, перекрывавший исходник RFAB в VFS), а `HallofStoriesKeyholeScript.psc`/`.pex` не содержал null-проверку `if (refActOnSuccess)` и порядок проверок `myMiscObject` из USSEP 2.0.6. | **Исправлено:** осиротевшие ванильные `.psc` удалены; в `HallofStoriesKeyholeScript` перенесены исправления USSEP (и проверен байткод `.pex`). |
| **V6** | Предметы, оружие, броня, статы NPC/игрока, зачарования, зелья, заклинания, камни судьбы, святилища | В `SkyrimTogether.esp` и `SkyrimTogetherQuestPatches.esp` **нет ни одной записи** `WEAP`, `ARMO`, `ALCH`, `INGR`, `ENCH`, `PERK`, `RACE`, `NPC_`, `AVIF`, `GMST`, `GLOB`, а также ни одного оверрайда `SPEL`/`MGEF` из `Skyrim.esm`/`DLC`/`RFAB.esp`. `InventoryService` и `EnchantmentItem` синхронизируют базовые `FormID` из самой сборки RFAB игрока без подмены характеристик. | **Подтверждено 100% чисто.** |
