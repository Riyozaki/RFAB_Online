# RFAB Online — Система «Единство в битве (Кооп)» (защита союзников без потери лечения и баффов) и исправление кодировки GitHub Release `v1.1.0`

**Дата:** 24.10.2026  
**Целевой релиз:** `v1.1.0` (обновляется на месте; базовый релиз `v1.0.0` остаётся неприкосновенным, лишний тег `v1.2.0` удаляется).

---

## 1. Разбор оригинальной механики RFAB в ветке Красноречия

По результатам бинарного разбора `RFAB.esp` и скриптов `MO2/mods/RFAB/scripts/source/`:

1. **Перк Красноречия 50 — «Лидерство» (`RFAB_Perk_Speech_Leadership`, `0x06427139`):**
   - Выдаёт игроку способность (талант) **`RFAB_Power_NoAllyDamage` (`0x060CDFED`) — «Единство в битве»** (*«Ваши союзники перестают получать от вас урон.»*).
2. **Переключатель `RFAB_Effect_NoAllyDamage_Toggle` (`0x060CDFEA`, скрипт `RFAB_ToggleAbility.psc`):**
   - Добавляет или убирает пассивную способность `RFAB_Ability_NoAllyDamage` (`0x060CDFEE`) и выводит уведомление:
     - *«Урон по союзникам отключен»* / *«Урон по союзникам включен»*.
3. **Пассивный эффект `RFAB_Effect_NoAllyDamage` (`0x060CDFEB`):**
   - Скрипт `RFAB_AllyHitAllow.psc` выставляет `Game.SetGameSettingInt("iAllyHitNonCombatAllowed", 1000)` (чтобы случайные задевания не вызывали агрессию союзников).
   - Поле `perkToApply` (`DATA@136`) накладывает скрытый перк **`RFAB_Perk_NoAllyDamage` (`0x060CDFEC`)** с двумя точками входа:
     - **`Mod Spell Magnitude` (`0x1D`) -> `Multiply * 0.0`**, если заклинание **не** входит в белый список `FLST RFAB_List_NoAllyDamage_AllowedSpells` (`0x06005CAA`), а цель — невраждебный союзник/спутник.
     - **`Mod Attack Damage` (`0x23`) -> `Multiply * 0.0`** по невраждебным союзникам/спутникам.
4. **Белый список `FLST RFAB_List_NoAllyDamage_AllowedSpells` (`0x06005CAA`):**
   - Включает 18 специальных заклинаний поддержки RFAB, которые обязаны проходить сквозь защиту от урона по союзникам: `RFAB_Spell_Restoration1_HealingHands`, `RFAB_Spell_Conjuration1_NecrohealOnTarget` (лечение нежити/вампира/лича), `RFAB_Spell_Conjuration1_AzureReconstruction`, `RFAB_Spell_Alteration1_ArcaneMending` (починка конструктов/атронахов), `RFAB_Spell_Restoration2_ProtectionFromPoisonDiseaseOnTarget`, а также ауры-плащи (`AuraSummoningProtection`, `AuraSummoningPower`, `AuraSummoningRegeneration`, `AuraOfWarlock`, `LionheartCloakProc`, `AuraMageArmorOnTargetCloakProc`, `Ritualism_Aura_MovementSpeed`, `DarkInfusion_Aura_ResistFire`, `Werewolf_Totem_Brotherhood` и др.).

---

## 2. Архитектура «Единства в битве (Кооп)» в `RFAB_Online`

Чтобы **не обесценивать** сам перк Красноречия 50 «Лидерство» для одиночных NPC-спутников и собственных саммонов игрока, кооп-переключатель действует **строго на кооперативных союзников по группе** (других игроков в вашей группе и призывных существ / миньонов других игроков вашей группы):

1. **Переключение в игре через меню `Правый CTRL` (`F2`), Настройки и команду `/allyprotect` — строго под управлением Хоста:**
   - В верхней панели меню `Правый CTRL` (рядом с «Подсветить игроков») добавлена кнопка-переключатель **«Единство в битве: ВКЛ / ВЫКЛ»** с подсветкой активного состояния (по умолчанию на сервере **ВКЛ**).
   - **Право переключения принадлежит строго Хосту** (`SaveSnapshotService::IsPlayerAuthorizedHost`: админ-сессия -> лидер кооп-группы `PartyService::IsPlayerLeader` -> первый подключившийся игрок с минимальным `Player::GetId()`). У участников-гостей в оверлее отображается заблокированный индикатор текущего состояния Хоста (`Единство в битве: ВКЛ (Хост)` / `ВЫКЛ (Хост)`), а чекбокс в *Настройки -> Группа* и чат-команда `/allyprotect` также защищены проверкой прав Хоста.
   - При попытке подделки пакета `SyncRfabProgressionEvent::AllyProtectionSync` не-хостом сервер отклоняет запрос с предупреждением в лог и повторно отправляет клиенту авторитетное состояние сервера. При входе/выходе игроков и смене лидера группы `PartyService` автоматически обновляет статус Хоста на всех клиентах.
   - При переключении Хостом у всех участников группы выводится внутриигровое уведомление в стиле RFAB: *«Единство в битве (Кооп): урон по союзникам отключен»* / *«...включен»*.
2. **Серверно-авторизованная синхронизация состояния (`AllyProtectionSync`):**
   - `RfabProgressionService` хранит авторитетный флаг `m_allyProtectionEnabled{true}` и рассылает `NotifyRfabProgressionEvent::AllyProtectionSync` (`GlobalValue = 1.0f/0.0f`, `XpDelta = hostPlayerId`, `EditorId = "toggle"` при явном переключении или `""` при тихой синхронизации). Клиентский `localStorage` больше никогда не перезаписывает серверное состояние при подключении.
3. **100% блокировка урона и негативных эффектов при сохранении лечения и баффов:**
   - **Физический урон (`HookDamageActor` в `Actor.cpp`)**: мечи, двуручное оружие, стрелы, болты, удары щитом и силовые атаки от игрока (и его саммонов) по союзникам в группе и их саммонам блокируются до вызова `RealDamageActor` и до генерации `HitEvent` (чужие саммоны никогда не агрятся от случайного задевания), а `iAllyHitNonCombatAllowed` выставляется в `1000`.
   - **Магический урон и негативные эффекты (`HookAddTarget` в `MagicTarget.cpp`)**: все атакующие заклинания, плащи стихий, зачарования оружия, яды, болезни, замедления, паралич, страх, «Смертный приговор» и крики (включая отбрасывание `Безжалостной силы` / `Fus Ro Dah`) блокируются до `RealAddTarget`.
   - **Лечение и баффы (`MagicItem::IsHealingSpell()` + `MagicItem::IsBuffSpell()` + прямой учёт `FLST RFAB_List_NoAllyDamage_AllowedSpells`)**: проходят на 100% как по игрокам группы, так и по их саммонам (включая «Исцеляющие руки», некрохил по вампиру/личу, «Магическое восстановление» по конструкту союзника и все ауры RFAB).
   - **Режим спарринга / проверки урона**: если игрок в группе явно выключает «Единство в битве: ВЫКЛ», его удары и заклинания по согруппникам разрешаются без необходимости редактировать `STServer.ini` и перезапускать сервер (при этом игроки вне группы по-прежнему подчиняются серверному `bEnablePvp`).

---

## 3. Исправление кодировки описания релиза на GitHub и политика версий (`v1.0.0` + `v1.1.0`)

- **Причина кракозябр на скриншоте релиза (`RFAB Online ... â€” Ð§Ð¸ÑÑ‚Ñ‹Ð¹`):** файл `Tools/rfab/Build-CloudRelease.ps1` не имел маркера UTF-8 BOM (`EF BB BF`). В облачном раннере `windows-latest` Windows PowerShell 5.1 читал `.ps1` без BOM как `Windows-1252`, превращая кириллицу в строке `$ReleaseNotes` в двойную перекодировку UTF-8 -> CP1252 -> UTF-8.
- **Исправление:**
  1. Текст описания релиза вынесен в отдельный чистый UTF-8 шаблон `Tools/rfab/RELEASE_NOTES_TEMPLATE.md` и читается явно через `[System.IO.File]::ReadAllText(..., [System.Text.Encoding]::UTF8)`, а на все `.ps1`-скрипты (`Build-CloudRelease.ps1`, `Package-Release.ps1`, `Setup-RfabOnline.ps1`) установлен обязательный UTF-8 BOM (`EF BB BF`) с проверкой в `verify_release_bundle.py`.
  2. Лишний релиз `v1.2.0` удалён (`gh release delete v1.2.0 --yes --cleanup-tag`); поддерживаются ровно два релиза: неприкосновенный бэкап **`v1.0.0`** и обновляемый актуальный релиз **`v1.1.0` (`Latest`)**.

---

## 4. Финальный аудит безопасности, стабильности C++ и чистых обновлений установщика

В рамках полного ревью кодовой базы (`Code/server`, `Code/client`, `Code/common`, `Code/tp_process`, `Code/skyrim_ui`, `Tools/rfab`) выявлены и устранены дополнительные краевые случаи:

1. **Защита от крашей и обходов в `MagicTarget::HookAddTarget` и `MagicService::OnNotifyAddTarget`:**
   - Проверка `EvaluateCoopAction` перенесена **выше** `ScopedSpellCastOverride::IsOverriden()`, чтобы синхронные эффекты (`CastSpellImmediate`, стихийные плащи, скриптовые ауры) не пробивали «Единство в битве».
   - Добавлена зеркальная фильтрация `EvaluateCoopAction` во входящем сетевом обработчике `MagicService::OnNotifyAddTarget` (плюс ранний возврат `if (!pActor) return;` вместо `continue` в цикле с последующим разыменованием `pActor`).
   - Добавлены полные `nullptr`-проверки для `arTargetData.pEffectItem`, `pEffectSetting`, `pSpell` и `RTTI_Cast<Actor>(this)` в `HookAddTarget`, а также для `PlayerCharacter::Get()` и `Settings::GetDifficulty()` в `PlayerService`.
2. **Поддержка скриптовых призывов духов RFAB и защита от саммонов вне группы:**
   - В `RfabAdapterService::EvaluateCoopAction` добавлен учёт локальных саммонов через `IsPlayerTeammate()` (а не только ванильный `GetCommandingActor()`), что покрывает всех духов и тоггл-миньонов RFAB.
   - В `Actor::HookDamageActor` защита `!PvpEnabled` на локальном игроке `0x14` расширена на призывных существ удалённых игроков **вне** группы.
3. **Защита пароля в логах (`OverlayClient.cpp`):**
   - Удалено логирование `eventArgs->GetString(0..2)` в `connect`-обработчике CEF, которое ранее могло записывать пароль сервера в текстовый клиентский лог и читать аргументы вне границ списка.
4. **Чистое обновление с версии на версию (`Setup-RfabOnline.ps1`, `Package-Release.ps1`):**
   - При запуске `INSTALL_RFAB_ONLINE.bat` установщик автоматически останавливает `SkyrimTogetherServer` и осиротевший `TPProcess` (`Wait-Process`), через `Remove-DirectoryClean` полностью сносит старые каталоги `MO2\mods\RFAB_Online` и `RFAB_Online_Server` (сохраняя и мигрируя `STServer.ini`), очищает устаревшие файлы в `MO2\overwrite` и устанавливает свежую версию с нуля.
   - В `Quick_Update.zip` (`UPDATE_RFAB_ONLINE.bat`) добавлены `TPProcess.exe`, `SkyrimTogetherQuestPatches.esp`, полная замена папки `UI/` и автоматическое удаление устаревших `.pex`/`.psc`/`logo*.nif` из старых версий.

---

## 5. Изменённые компоненты проекта

- `Code/common/RfabModpackCompat.h` — чистая логика `EvaluateCoopAllyActionPolicy` с юнит-тестами в `Code/tests/rfab_se1597_hardening_audit.cpp`.
- `Code/encoding/Messages/{SyncRfabProgressionEvent,NotifyRfabProgressionEvent}.h`, `Code/server/Services/{RfabProgressionService,PartyService}.{h,cpp}` — серверно-авторизованная синхронизация `EventKind::AllyProtectionSync` строго от Хоста с автообновлением прав при смене лидера группы.
- `Code/client/Services/RfabAdapterService.{h,cpp}` и `Code/client/Services/Generic/PartyService.cpp` — `EvaluateCoopAction` (включая `IsPlayerTeammate`), проверка прав Хоста `IsLocalPlayerAllyProtectionHost`, синхронизация с CEF `allyProtectionState(enabled, isHost)`, `iAllyHitNonCombatAllowed = 1000` / `iAllyHitCombatAllowed = 1000`, кэш `FLST RFAB_List_NoAllyDamage_AllowedSpells` и HUD-уведомления.
- `Code/client/Games/Skyrim/{Actor.cpp,Forms/MagicItem.cpp,Magic/MagicTarget.cpp}` и `Code/client/Services/Generic/{MagicService,PlayerService,OverlayClient}.cpp` — 100% блокировка физического/магического урона и негативных эффектов по союзникам в группе и их саммонам при сохранении 100% лечения и баффов, защита саммонов вне группы, null-guards и удаление логирования пароля.
- `Code/tp_process/ProcessHandler.cpp` + `Code/skyrim_ui/` (`angular.json` с `images`/`fonts`) + `dist_ready/RFAB_Online_MO2/SkyrimTogetherReborn/UI/` — мост CEF V8 `window.skyrimtogether.setAllyProtection`, кнопка Хоста **«Единство в битве: ВКЛ / ВЫКЛ»** (и индикатор для гостей) в меню Правого CTRL, чекбокс в *Настройки -> Группа* и чат-команда `/allyprotect`.
- `CHANGELOG.md`, `README.md`, `PROCHTI_MENYA.txt`, `Tools/rfab/{RELEASE_NOTES_TEMPLATE.md,Setup-RfabOnline.ps1,Build-CloudRelease.ps1,Package-Release.ps1,verify_release_bundle.py}` — чистая установка/обновление между версиями, журнал изменений, шаблон релиз-ноутов в чистом UTF-8, UTF-8 BOM на всех `.ps1` и обновление релиза `v1.1.0` на месте.

