# PROGRESS

Сессия: 2026-10-22 02:41:52 UTC → 08:41:52 UTC (6 ч). Ветка
`arena/ada174eb-rfab-online`. Отсчёт — `.work/time_log.md`.

## Block 1 (02:42–03:02) — эталон 1.5.97 из истории + фикс раскладок

**План:** использовать последний pre-AE коммит upstream (`5c5837f6`) как
эталон настоящих раскладок 1.5.97, автоматизировать сравнение, исправить
найденные регрессии.

**Сделано:**
- `tools/struct_layout_diff.py` — сравнение всех `static_assert(offsetof/sizeof)`
  порта с pre-AE эталоном; поддерживает рабочее дерево, `--fail-on-regression`.
- Найдено 2 регрессии, обе исправлены:
  - `TESObjectCELL.refData` 0x80 → **0x88**, `ReferenceData` 0x30 → **0x28**
    (был взят AE-вариант из merge; ломало `refData.refArray` в
    `GetRefsByFormTypes`), `cellFlags` приведён к `uint8[5]` как в 1.5.97;
  - `ActiveEffect` sizeof 0x90 → **0x80**, убраны AE-поля
    (`eConditionStatus/usUniqueID/eCastingSource`), `fMagnitude` возвращён на 0x78.
- После фикса: 127 совпадающих ассертов, 0 расхождений, 2 переименования
  (`Actor::unk9C` → `fVoiceTimer`, `TESLeveledList` поля).

**Проверено:** скриптом (127 asserts), вручную по исходникам 5c5837f6.
**Не проверено:** сборка (нет MSVC), игра.
**Риски:** поля `ActiveEffect` 0x70/0x74 (`fElapsedSeconds/fDuration`) выведены
по порядку AE-полей, используются только в debug-виде — помечено в коде.

## Block 2 (02:42–02:45) — P1: знак HP, владелец, протокол

**План:** закрыть три ошибки, найденные в ревью: инвертированная дельта HP на
сервере, запись актор-велью любым клиентом, неучтённое поле в `operator==`.

**Сделано:**
- `ActorValueService::OnHealthChangeBroadcast` (server): `current - delta` →
  `current + delta` + clamp в `[0, maxHealth]`; добавлен общий
  `Gameplay::ActorValueIds.h` (`kHealth = 24` и т.д., сверено с
  `ActorValueInfo.h`).
- Все четыре обработчика (`ActorValueChanges`, `ActorMaxValueChanges`,
  `HealthChangeBroadcast`, `DeathStateChange`) требуют, чтобы отправитель был
  владельцем сущности (`OwnerComponent`).
- `ServerSettings::operator==` учитывает `SyncPlayerCalendar` (иначе изменение
  настройки не доезжало до клиентов).
- `Code/common/ProtocolVersion.h` (v1), `HealthScaling.h`, `XpShare.h`,
  `Code/tests/pure/*` + `tools/run_pure_tests.sh` (66 проверок на хосте).

**Проверено:** pure-тесты 66/66; чтение кода сервера.
**Не проверено:** сборка/игра.

## Block 3 (02:47–02:48) — HP-скейлинг N+1 (серверная истина)

**Сделано:** `HealthScalingService` (сервер): `N+1`, clamp 32, база HP
захватывается один раз, `Recompute` идемпотентен, мёртвые/игроки/маунты
исключены, `BaseMaxHealth` проброшен в компонент, множитель публикуется в
`ServerSettings.HealthMultiplier`; клиентский статичный пересчёт удалён,
клиент применяет серверные максимумы и восстанавливает базу+долю HP на
дисконнекте. Тесты: 66 → 86.

## Block 4 (02:48–02:52) — опыт на весь сервер

`ProtocolVersion = 2`, `SyncRfabExperienceRequest.Sequence`,
`XpShareService`: сентизация, sequence-dedup, окно одинаковых сумм 5 с,
рассылка `SendToPlayers(..., sender)`; флаги `bEnableRfabXpSync`,
`bXpShareWholeServer`, `uXpShareDedupWindowMs`. Тесты 86 → 102.

## Block 5 (02:50–02:52) — сохранения в мультиплеере

`Gameplay::SaveSession` (ini-политика, JSON-индекс, `SettleTracker`),
клиентский `SaveGameService` (загрузка сейва в сессии → дисконнект, индекс
сейвов, HUD), `docs/multiplayer-saves.md`, `.work/ASSUMPTIONS.md`,
`GameFiles/.../RFABOnline.ini`. Тесты 102 → 127.

## Block 6 (02:53–02:56) — опыт через пайплайн RFAB

Найдены .psc исходники RFAB в репозитории сборки; `ModXP` = `PlayerXP.Mod` +
`VanilaXPGain`. Клиентский `RfabXp` воспроизводит обе записи + проверку порога
уровня; `Gameplay::XpAward` (тестируемая арифметика, clamp 10M, отказ от
отрицательных/абсурдных). `docs/rfab-xp.md`.

## Block 7 (02:56–02:59) — аудит №1

Найдено и исправлено:
- двойной XP у клиента, который сам выполнил то же событие → `EchoFilter`
  (окно 1.5 с, тесты);
- сервер принимал NaN/inf актор-велью и max-значения → отбрасываются;
  уменье max health подрезает текущее HP;
- сервер разрешал «убить» игрока → отказ;
- маунты скейлились (лошадь ×4) → исключены; драконы явно скейлятся;
  мёртвые `IsEssential/ScaleEssential` заменены на реальные `IsMount/IsDragon`;
- рантайм-переключение `bEnableHealthScaling` не применялось до смены состава;
- удалён осиротевший `SaveSessionService.h`.
- **HP-база запоминается по форме**, а не по сущности: иначе после выгрузки
  ячейки клиент отдавал уже масштабированный максимум и множитель
  применялся повторно (×3 → ×9).

Тесты: 135 проверок, структуры 127/127.

## Дальше (порядок)

1. Документация-поставка: CHANGELOG, сборка/установка, конфиги, матрица
   совместимости с RFAB, чек-лист ручного теста для 1–4 игроков.
2. Аудит №2: повторное чтение каждого диффа (дисконнект владельца в бою,
   смена ячейки, реконнект, деление на ноль, переполнения, null).
3. Улучшения (низкий риск, за флагом): телеметрия XP/скейлинга в лог,
   покрытие тестами краевых случаев, диагностика (команда статуса).
4. Финальный отчёт по 7 пунктам задания.
