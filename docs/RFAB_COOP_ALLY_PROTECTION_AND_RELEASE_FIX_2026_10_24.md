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

1. **Переключение прямо в игре через меню `Правый CTRL` (`F2`), Настройки и команду `/allyprotect`:**
   - В верхней панели меню `Правый CTRL` (рядом с «Подсветить игроков») добавлена кнопка-переключатель **«Единство в битве: ВКЛ / ВЫКЛ»** с подсветкой активного состояния (по умолчанию **ВКЛ** и сохраняется между сессиями).
   - Дублирующий чекбокс добавлен в **Настройки -> Группа**, а также чат-команда `/allyprotect`.
   - При переключении выводится внутриигровое уведомление в стиле RFAB: *«Единство в битве (Кооп): урон по союзникам отключен»* / *«...включен»*.
2. **Сетевая синхронизация состояния между клиентами (`AllyProtectionSync`):**
   - В Скайриме физические удары и стрелы по Игроку Б обсчитываются на клиенте Б (`HookDamageActor` на `0x14`). Поэтому состояние переключателя каждого игрока синхронизируется через `SyncRfabProgressionEvent::EventKind::AllyProtectionSync` / `NotifyRfabProgressionEvent::EventKind::AllyProtectionSync` при подключении, входе новых игроков и каждом переключении кнопки.
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

## 4. Изменённые компоненты проекта

- `Code/common/RfabModpackCompat.h` — чистая логика `EvaluateCoopAllyActionPolicy` с юнит-тестами в `Code/tests/rfab_se1597_hardening_audit.cpp`.
- `Code/encoding/Messages/{SyncRfabProgressionEvent,NotifyRfabProgressionEvent}.h` и `Code/server/Services/RfabProgressionService.{h,cpp}` — сетевая синхронизация `EventKind::AllyProtectionSync` между игроками (с тестами сериализации в `Code/tests/encoding.cpp`).
- `Code/client/Services/RfabAdapterService.{h,cpp}` — `EvaluateCoopAction`, синхронизация `m_localAllyProtectionEnabled` / `m_remoteAllyProtection`, `iAllyHitNonCombatAllowed = 1000` / `iAllyHitCombatAllowed = 1000`, кэш `FLST RFAB_List_NoAllyDamage_AllowedSpells` и HUD-уведомления.
- `Code/client/Games/Skyrim/{Actor.cpp,Forms/MagicItem.cpp,Magic/MagicTarget.cpp}` — 100% блокировка физического/магического урона и негативных эффектов по союзникам в группе и их саммонам при сохранении 100% лечения и баффов (включая все 18 заклинаний из `RFAB_List_NoAllyDamage_AllowedSpells`).
- `Code/tp_process/ProcessHandler.cpp` + `Code/client/Services/Generic/OverlayClient.cpp` + `Code/skyrim_ui/` + `dist_ready/RFAB_Online_MO2/SkyrimTogetherReborn/UI/` — мост CEF V8 `window.skyrimtogether.setAllyProtection`, кнопка **«Единство в битве: ВКЛ / ВЫКЛ»** в меню Правого CTRL, чекбокс в *Настройки -> Группа* и чат-команда `/allyprotect`.
- `Tools/rfab/{RELEASE_NOTES_TEMPLATE.md,Build-CloudRelease.ps1,Package-Release.ps1,verify_release_bundle.py}` — шаблон релиз-ноутов в чистом UTF-8, UTF-8 BOM на всех `.ps1` и обновление релиза `v1.1.0` на месте.

