# Спецификация полной адаптации под сборку RFAB (`drake1ch/RFAB`)

Документ основан на прямом анализе **всех `872` Papyrus-скриптов (`.psc`)**, записей **`RFAB.esp`** ([`docs/rfab/rfab_records.tsv`](./rfab/rfab_records.tsv)), конфигураций **`StorageUtilData/*.{ini,json}`** ([`docs/rfab/configs/`](./rfab/configs/)) и нативного API `RFAB_PapyrusFunctions.psc`.

---

## 1. Ключевые FormID и файлы данных `RFAB.esp`

> **Важно:** Индекс загрузки `RFAB.esp` зависит от профиля MO2 (обычно `06xxxxxx`), поэтому в C++ **никогда не хардкодить старший байт `0x06`!** Все формы резолвятся динамически через `DataHandler::LookupForm(localFormId, "RFAB.esp")`!

| Назначение в `RFAB.esp` | Local FormID (`RFAB.esp`) | Тип | Роль в механике RFAB |
|---|---|---|---|
| `RFAB_XP_Amount` (`PlayerXP`) | `0x00AD45BB` | `GLOB` | Суммарный счётчик набранного XP для отображения в `RFAB_Menu` |
| `RFAB_XP_Multiplier` (`MultiplierXP`) | `0x00112815` | `GLOB` | Персональный множитель XP игрока (зависит от проклятия/расы/настроек) |
| `RFAB_KW_NoExpGain` (`NoXPKeyword`) | `0x0015B381` | `KYWD` | Запрет начисления XP за убийство призывных/технических существ |
| `REQ_Combat_NoDeathHandling` | `0x00AA1E9C` | `GLOB` | Отключает/переключает перехват смерти `REQ_DeathHandler.psc` |
| `RFAB_Global_CurrentUmbraBossPhase` | `0x00AD42A9` | `GLOB` | Текущая фаза многофазного босса Умбра |
| `RFAB_Counter_FalmersKilled` | `0x000CF18B` | `GLOB` | Счётчик убитых фалмеров (инкрементируется в `RFAB_XP_Handler.OnKill`) |
| `RFAB_Global_DragonsKilled` | `0x000CF1EF` | `GLOB` | Счётчик убитых драконов (прогрессия бонусов RFAB) |
| `RFAB_TFOTG_Kill_Counter` | `0x00AD488A` | `GLOB` | Счётчик убийств квестовой цепочки TFOTG |

Файлы таблиц прогрессии RFAB в runtime (`Data/SKSE/Plugins/StorageUtilData/`):
- `Experience.ini` — пороги XP по уровням `1..100` (`500`, `1000`, ..., `155000`);
- `BossList.json` — фиксированные награды XP за именных боссов RFAB по `GetDisplayName()`;
- `RaceList.json` — базовый XP за `1` уровень врага для каждой расы (`VictimLevel * RaceXP`);
- `Quests.ini` — награды XP и иконки за завершение квестов;
- `RFAB_XP_Settings.json` — `BaseBookXP` и цвет уведомлений SkyUI (`UILib`).

---

## 2. Истинная архитектура системы опыта RFAB и РАВНОЕ РАСПРЕДЕЛЕНИЕ ОПЫТА (`100%` всем игрокам)

### Почему реализации XP во ВСЕХ предыдущих ветках (`main`, `fcbffe0c`, `ada174eb`) были полностью нерабочими

Прямое чтение [`RFAB_XP_Alias.psc`](https://github.com/drake1ch/RFAB) и `RFAB_XP_Handler.psc` выявило **критическую архитектурную ошибку** всех предыдущих попыток:
1. В `RFAB` глобальная переменная **`RFAB_XP_Amount` (`0x00AD45BB`) НЕ ПОВЫШАЕТ УРОВЕНЬ ИГРОКА!** Это лишь справочный счётчик для меню статистики `RFAB_Menu`.
2. **Реальный прогресс уровня персонажа** в `RFAB_XP_Handler.psc` хранится в движковом поле **`PlayerCharacter::pSkills->data->xp`** (`Game.GetPlayerExperience()` / `Game.SetPlayerExperience(...)`), причём `RFAB_XP_Alias.psc` выставляет:
   ```papyrus
   Game.SetGameSettingFloat("fXPLevelUpMult", 0.0) ; отключает прокачку от навыков!
   Game.SetGameSettingFloat("fXPLevelUpBase", XPHandler.Experience[Player.GetLevel() - 1])
   ```
   Когда во всех старых ветках на клиенте-получателе делалось только `pXpGlobal->f += xp`, цифра в `RFAB_Menu` росла, **но `pSkills->data->xp` оставался `0` — второй и третий игроки НИКОГДА НЕ МОГЛИ ПОЛУЧИТЬ НОВЫЙ УРОВЕНЬ от синхронизированного опыта!**
3. При завершении квеста (`QuestService`) `OnQuestComplete` срабатывает локально у каждого игрока в группе. Тупой поллинг дельты `RFAB_XP_Amount` приводил к тому, что каждый из `N` игроков отправлял `SyncRfabExperienceRequest` остальным `N - 1` игрокам -> **умножение квестового XP в `N` раз** (`6000 XP` вместо `2000 XP` на троих!).
4. При убийстве врага `RFAB_XP_Alias.OnActorKilled(akVictim, akKiller)` проверяет `IsPlayerOwnedKiller(akKiller)` и помечает труп `akVictim.ForceActorValue("Infamy", -1)` **только на клиенте убийцы**:
   - другие игроки получали `0 XP` (или теряли уровень);
   - если труп менял владельца / добивался повторно без синхронизации метки `Infamy = -1`, возникал эксплойт повторного получения XP с одного босса;
   - чтение книг опыта (`XPBooks` -> `XPBooksReaded`) позволяло передавать книгу по кругу между игроками и дюпать по `+600 XP` `N` раз!
5. Ветка `main` рассылала `NotifySyncRfabExperience` через `SendToParty(...)` вместо серверного широковещательного распределения всем подключённым игрокам: если игроки ещё не вступили в группу или переподключились, опыт терялся.

### Целевая архитектура `RfabXpSyncService` (Клиент + Сервер)

Требование: **Опыт распределяется между всеми игроками ПОРОВНУ (`1×` базовой награды каждому игроку на сервере, без деления `1/N` и без умножения `N×`)**:

1. **Серверный дедуплицированный реестр наград (`ServerXpLedger`)**:
   - Пакет `SyncRfabExperienceRequest` несёт:
     - `SourceType`: `Kill = 1`, `Quest = 2`, `Book = 3`, `Custom = 4`
     - `SourceKey` (`uint64_t`): для `Kill` — `ServerEntityId` (или `(CellId, FormId)`), для `Quest` — `(QuestFormId << 16) | Stage`, для `Book` — `BookFormId`, для `Custom` — `FNV1a(Label) ^ TimeBucket`
     - `BaseXp` (`uint32_t`) — **чистый базовый XP до персонального множителя `MultiplierXP`** (либо точный `GainedXp`, чтобы все игроки получили **строго равное число единиц XP**)
     - `Flags` (`IsFalmerKill`, `IsDragonKill`) + `Label` (имя врага/квеста/награды для HUD)
   - Сервер хранит множество уже выданных `SourceKey`:
     - если `SourceKey` уже выдан — дублирующий пакет от 2-го/3-го клиента **отбрасывается на сервере (`0` дублей на квестах и боссах!)**;
     - если `SourceKey` новый — сервер фиксирует его и рассылает **`NotifySyncRfabExperience` ВСЕМ подключённым игрокам на сервере поровну**.
2. **Полное нативное применение XP на каждом клиенте (`ApplyEqualRfabXp`)**:
   - Синхронно обновляет **оба** хранилища RFAB:
     1. `RFAB_XP_Amount->f += awardedXp;`
     2. `pPlayer->pSkills->data->xp += static_cast<float>(awardedXp);`
   - Помечает локально `SourceKey` как уже полученный (чтобы локальный `OnQuestComplete` / `OnBookRead` через `0.2с` не начислил его второй раз!), а для `Book` добавляет `akBook` в `XPBooksReaded`, для `Kill` выставляет `akVictim->ForceActorValue(Infamy, -1.f)` и синхронно инкрементирует `RFAB_Counter_FalmersKilled` / `RFAB_Global_DragonsKilled`.
   - Выводит штатное уведомление `HUD Menu` / `UILib` (`"<Имя>: +X XP"`) и при достижении `pSkills->data->xp >= Game.GetExperienceForLevel(level)` корректно запускает штатную анимацию нового уровня RFAB (`ShowLevelUp`).
   - Отключает ванильный `bEnableXpSync` по навыкам, когда активен `RFAB.esp` (`fXPLevelUpMult == 0`), чтобы исключить любые побочные сдвиги `pSkills->data->xp`.

---

## 3. Масштабирование врагов: СТРОГО `1×` КОЛИЧЕСТВО ВРАГОВ + МАСШТАБИРОВАНИЕ HP (`2` игрока = `×3` HP, `3` игрока = `×4` HP)

### 3.1. Запрет на размножение числа врагов (de-duplication врагов и скриптов засад)
Количество врагов при 2, 3 и более игроках **обязано оставаться строго `1×` (как в одиночной игре)**:
1. **Внешние и внутренние ячейки (`ActorService` / `AssignCharacterRequest`)**:
   - Предразмещённые враги с фиксированным `FormId` (`ModId != 0xFF`) дедуплицируются сервером по `FormId` (первый вошедший в ячейку регистрирует актёра, остальные получают `Owner = false` и привязываются к уже существующему серверному `ServerId`).
   - Динамические `0xFFxxxxxx` спавны из `RFAB_AmbushScript.psc` (`PlaceActorAtMe` в триггерах RFAB — драугры, вампиры, сфер-центурионы `DwarvenCenturionSupportsSummon.psc`, призывы боссов): триггер `OnTriggerEnter` в `RFAB_AmbushScript` срабатывает однократно через `GoToState("Done")` только при условии синхронизации факта срабатывания триггера/квестового алиаса или фильтрации спавна `PlaceActorAtMe` только на **Party Leader / Cell Host** для не-саммонов игрока! Это **полностью исключает спавн `2× / 3×` пачек засадных драугров и центурионов** при одновременном входе группы в зал подземелья RFAB!

### 3.2. Точная формула масштабирования HP врагов без багов `main` и `ada174eb`

Формула множителя максимального HP врага от числа активных игроков $N$ на сервере:

$$M(N) = \begin{cases} 1.0, & N \le 1 \\ N + 1.0, & N \ge 2 \end{cases}$$

Проверка по ТЗ:
- $N = 1 \implies M(1) = \mathbf{1\times\text{HP}}$
- $N = 2 \implies M(2) = \mathbf{3\times\text{HP}}$
- $N = 3 \implies M(3) = \mathbf{4\times\text{HP}}$
- $N = 4 \implies M(4) = \mathbf{5\times\text{HP}}$ *(с серверным CVar `Gameplay:bScaleEnemyHealthByPlayers = true` для гибкости)*

#### Кого СТРОГО масштабировать, а кого КАТЕГОРИЧЕСКИ НЕ ТРОГАТЬ:
Масштабируются **ТОЛЬКО враждебные NPC/существа**, для которых одновременно выполняются все 5 условий:
1. `pActor != PlayerCharacter::Get()` (не локальный игрок);
2. `!pActor->GetExtension()->IsRemotePlayer()` (не удалённый игрок — в `main` удалённым игрокам ломался HP!);
3. `!pActor->IsPlayerSummon() && !pActor->GetCommandingActor()` (не саммон/нежить игрока — призыв игрока НЕ должен получать `×3` HP!);
4. `!pActor->IsPlayerTeammate()` и не в `PlayerFollowerFaction` (не напарник/спутник игрока);
5. Враждебен игрокам (`IsHostileToActor(pPlayer)` или находится в бою против игрока) и `!pActor->IsDead()`.

#### Безопасная математика движка Skyrim (`PermanentMod` + `DamageMod`, 0% шанс мгновенной смерти при рескейле!)
В движке Skyrim:

$$\text{MaxHP} = \text{BaseHP} + \text{PermanentMod} + \text{TemporaryMod}, \qquad \text{CurrentHP} = \text{MaxHP} + \text{DamageMod} \quad (\text{где } \text{DamageMod} \le 0)$$

Критическая ошибка ветки `main`: вызов `ForceActorValue(DAMAGE, Health, +delta)` с **положительным** аргументом делал `DamageMod > 0`, из-за чего враг с `100` HP получал `500` HP вместо `300` HP!
Критическая ошибка ветки `ada174eb`: при выходе 2-го игрока ($M: 3 \to 1$) сначала вычитался `PermanentMod` ($300 \to 100$), а старый `DamageMod` оставался или пересчитывался с инверсией -> `CurrentHP <= 0` -> **мгновенная смерть всех NPC вокруг игрока при дисконнекте напарника!**

**Точный инвариант сохранения процента здоровья $r = \text{CurrentHP}_{\text{old}} / \text{MaxHP}_{\text{old}} \in (0, 1]$:**
Пусть $B$ — немодифицированный базовый макс. HP врага ($M_{\text{old}} \to M_{\text{new}}$):
- Новая добавка `PermanentMod` от скейлинга: $\Delta P_{\text{target}} = B \cdot (M_{\text{new}} - 1) - \Delta P_{\text{applied}}$.
- Целевой урон после рескейла: $D_{\text{new}} = -(1 - r) \cdot B \cdot M_{\text{new}} \le 0$, изменение $\Delta D = D_{\text{new}} - D_{\text{old}}$ (**при полном здоровье $r = 1 \implies \Delta D = 0$, `DamageMod` ВООБЩЕ НЕ ТРОГАЕТСЯ!**).
- **Порядок операций в движке (исключает переход `CurrentHP <= 0` на любом промежуточном шаге!):**
  1. **При росте числа игроков ($M_{\text{new}} > M_{\text{old}}$):** **СНАЧАЛА** увеличиваем `ForceActorValue(PERMANENT, Health, +\Delta P)` (`MaxHP` вырос -> `CurrentHP` гарантированно $> 0$), **ЗАТЕМ** корректируем `DamageMod` на $\Delta D \le 0$, чтобы раненый босс на `50%` HP остался ровно на `50%` нового `MaxHP` (не отхиливаясь фулл!).
  2. **При выходе игрока ($M_{\text{new}} < M_{\text{old}}$):** **СНАЧАЛА** уменьшаем накопленный урон `RestoreActorValue(DAMAGE, Health, +\Delta D)` (`DamageMod` приблизился к `0`), и **ТОЛЬКО ЗАТЕМ** снимаем `ForceActorValue(PERMANENT, Health, -\Delta P)`! Ни один живой моб или босс **математически никогда не может опуститься до `<= 0` HP** при дисконнекте игрока!
- **Авторитетность:** рескейл выполняет **ТОЛЬКО текущий `Owner` врага**, после чего рассылает актуальные `RequestActorMaxValueChanges` + `RequestActorValueChanges` (и сервер хранит верный `CurrentActorValues` с исправленным знаком `+ message.DeltaHealth`!).

---

## 4. Смерть игрока, Боссы, Кастомная магия и Кастомные анимации RFAB

1. **Система смерти (`REQ_DeathHandler.psc` vs мультиплеерный downed/respawn state)**:
   - В одиночном `RFAB` при смерти срабатывает `REQ_DeathHandler.psc` (принудительная перезагрузка сохранения) и `ReturnBagPlayerAfterDeath.psc`. В мультиплеере перезагрузка сейва одним игроком разрывает сессию и откатывает мир!
   - При подключении к серверу (`ConnectedEvent`) клиент `RFAB Online` автоматически выставляет `REQ_Combat_NoDeathHandling = 1.0` (`0x00AA1E9C` в `RFAB.esp`), передавая управление мультиплеерной системе возрождения/помощи союзнику (`Gameplay:bEnableDeathSystem = true`), и возвращает `0.0` при отключении от сервера!
2. **Многофазные боссы RFAB (`Umbra`, `Kruziikrel`, Драконьи жрецы, Эбонитовый воин)**:
   - Смена фаз `Umbra` управляется глобалом `RFAB_Global_CurrentUmbraBossPhase` (`0x00AD42A9`) и скриптовыми спеллами/телепортами; синхронизация ключевых глобалов боссов и запрет передачи ownership босса посреди каста смены фазы предотвращают застревание босса в неуязвимости (`Ghost` / `ethereal`).
3. **Кастомные заклинания и `RFAB_PapyrusFunctions.ScriptDamage`**:
   - Десятки заклинаний, рун, плащей и перков RFAB наносят урон нативной функцией `RFAB_PapyrusFunctions.ScriptDamage(akVictim, akAttacker, damage, type, applyCap)`, которая меняет `Health` напрямую через C++ `DamageActorValue`, минуя стандартный weapon hit!
   - Поскольку `TiltedOnline` перехватывает низкоуровневую функцию изменения здоровья `Actor::DamageActorValue` (`HookDamageActor`), **исправление знака `OnHealthChangeBroadcast` на сервере (`+ message.DeltaHealth` вместо `- message.DeltaHealth`)** + корректная атрибуция `apAttacker` автоматически делает `ScriptDamage` 100% синхронным для огня, холода, молнии, яда, чистого и физического скриптового урона RFAB!
4. **Кастомные анимации RFAB (`Nemesis`, `OAR`, `AMR`, `PayloadInterpreter`, `BehaviorDataInjector`)**:
   - Исправление смещений `BSAnimationGraphManager.h` (`0x98` / `0xA8` для `1.5.97`) + сохранение уже встроенной в `rfortier/TiltedEvolution-rwf` подсистемы `MODDED_BEHAVIOR_COMPATIBILITY` (`StringCache` для безлимитных переменных графа Havok и replay-кэша анимаций `#811`) обеспечивает нативную работу кастомных мувсетов `RFAB` без поломки синхронизации!
