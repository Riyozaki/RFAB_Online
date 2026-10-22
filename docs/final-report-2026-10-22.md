# Итоговый отчёт: сессия 2026-10-22 (RFAB Online, Skyrim SE 1.5.97.0.8)

Ветка: `arena/ada174eb-rfab-online` (16 коммитов поверх `4cba32cd`).
Репозиторий: <https://github.com/Riyozaki/RFAB_Online>.

## 1. Время

| | UTC | Комментарий |
| --- | --- | --- |
| Старт | 2026-10-22 02:41:52 | записан в `.work/session_start.txt` |
| Дедлайн (6 ч) | 2026-10-22 08:41:52 | |
| Окончание работы над кодом | ≈ 2026-10-22 09:22 | |
| Фактическая длительность | ≈ 6 ч 40 мин | контроль по системным часам, `date`, не по счётчикам в коде |

Работа продолжалась и после номинального дедлайна: последние ~40 минут ушли на
закрытие остатков (см. § 2, фазы 11–13) по прямому указанию «делать что не
закрыто». Заморозка (только проверки и документация) действовала с 08:41.

## 2. Что сделано, по фазам

| # | Фаза | Результат |
| --- | --- | --- |
| 0 | Разбор репозитория и требований | `docs/review-2026-10-22.md`, `docs/phase-0-analysis.md`, матрица требований, план |
| 1 | Порт на 1.5.97 | восстановлены pre-AE раскладки `TESObjectCELL` (`ReferenceData 0x28`, `refData 0x88`, `sizeof 0x140`) и `ActiveEffect` (`sizeof 0x80`, `fMagnitude 0x78`), удалены сторонние игровые бинарники и сабмодуль `RFAB_ref` |
| 2 | Сборка/каркас | проверки без Windows и игры: `tools/run_pure_tests.sh`, `tools/struct_layout_diff.py` |
| 3 | Синхронизация актор-велью | серверный HP-скейлинг (`N` игроков на сервере → ×(N+1)), проверка владельца, знак дельты, валидация значений, протокол v2 |
| 4 | Опыт | серверно-авторитетное начисление, воспроизведение `RFAB_XP_Handler.ModXP` нативно (RFAB-глобал + ванильная полоса) |
| 5 | Сохранения | `SaveSession` (политика загрузки, индекс `Saves/RFABOnline_saves_index.jsonl`, `SettleTracker`), `docs/multiplayer-saves.md` |
| 6 | Реестр прогресса | `ProgressionService`: `Data/progress/progress.jsonl`, восстановление при входе, протокол v3 (`AuthenticationRequest.RfabXpTotal`) |
| 7 | Анти-спам | `Gameplay::RateLimiter` (token bucket 30 / 10 в секунду), санитайз сумм, окно дедупа 5 с |
| 8 | Аудит HP | найден и исправлен разбор «актор пришёл уже отскейленным»; восстановление HP после сессии для поздно зашедших |
| 9 | Баланс новичков | `bXpBalanceNewcomers` (по умолчанию включён): новичок стартует со среднего опыта по серверу, но никогда не теряет свой |
| 10 | Диагностика | консольные команды сервера `rfabstats`, `rfabflush` |
| 11 | Аудит реестра | награда кредитуется **всем** получателям, а не только инициатору; `current_path(error_code)` в `noexcept`-конструкторе |
| 12 | Инструменты и документы | `tools/check_settings.py` (нашёл реальный баг, см. § 4.3), `tools/run_checks.sh`, готовый CI-workflow `docs/ci/checks.yml` |
| 13 | Косметика и сеть | HUD-буфер опыта (`RFAB XP +N` раз в 2 с), клиент больше не отправляет обратно только что полученный максимум |

### Коммиты

```
af2b0f12 feat(client): batch shared XP into one HUD line
175c1824 ci: provide a ready-made static checks workflow
f4c471d3 fix(client): keep the cached max values in sync when the server scales
841e9562 fix(party): one bAutoPartyJoin setting, not two; add consistency checks
3eab2219 docs: progression ledger, anti-spam limits, diagnostics in the reference docs
706b6b4f fix(progression): credit shared awards to every recipient, not just the initiator
0abda130 feat(xp): newcomer balance, server diagnostics commands
d660098f fix(scaling): undo scaling for actors that arrived already scaled
44a063a7 feat(xp): rate limit experience reports per client
04e9baaf feat(progression): remember every player's RFAB XP and restore it on reconnect
b0c4cf41 docs: configuration reference, RFAB compatibility, in-game test checklist, changelog
8f0976f7 feat(gameplay): server-authoritative HP scaling, server-wide XP, multiplayer saves
b41f3f59 fix(server): correct health delta sign, validate actor values, wire protocol v2
127002fd fix(port): restore Skyrim SE 1.5.97 layouts of TESObjectCELL and ActiveEffect
329e0c7c docs: full repository and code review with requirement matrix and plan
52ddebf0 chore(repo): drop third-party game binaries and RFAB_ref submodule
```

### Ключевые файлы

* Чистая логика (компилируется и тестируется на хосте): `Code/common/Gameplay/`
  — `HealthScaling.h`, `XpAward.h/.cpp`, `XpShare.h`, `Progression.h/.cpp`,
  `RateLimiter.h`, `SaveSession.h/.cpp`, `ActorValueIds.h`.
* Клиент: `Code/client/Services/Generic/{CharacterService,ActorValueService,SaveGameService,RfabXp}.cpp`,
  `Code/client/Services/{RfabXp.h,CharacterService.h,ActorValueService.h}`.
* Сервер: `Code/server/Services/{XpShareService,ProgressionService,HealthScalingService}.{h,cpp}`,
  `Code/server/GameServer.cpp`.
* Протокол: `Code/encoding/Messages/SyncRfabExperienceRequest.*`,
  `Code/encoding/Structs/AuthenticationRequest.*`, `Code/common/ProtocolVersion.h` (v3).
* Документация: `docs/rfab-xp.md`, `docs/multiplayer-saves.md`,
  `docs/configuration.md`, `docs/RFAB-compatibility.md`,
  `docs/manual-test-checklist.md`, `docs/review-2026-10-22.md`, `CHANGELOG.md`, `BUILD.md`.

## 3. Что проверено и чем

| Проверка | Команда | Результат |
| --- | --- | --- |
| Логика игры (HP-скейлинг, дедуп, лимитер, реестр, JSON-индекс, XP, HUD-буфер, id актор-велью) | `./tools/run_pure_tests.sh` | **248 checks, 0 failures** |
| Раскладки структур против последнего pre-AE коммита upstream | `python3 tools/struct_layout_diff.py` | 127 совпадают, 0 расходятся, 4 переименованы (смещение сохранено), 51 только в заголовках |
| Настройки сервера против документации (в обе стороны) | `python3 tools/check_settings.py` | 30 в коде / 30 в документации, 0 недокументированных, 0 устаревших |
| Всё вместе | `./tools/run_checks.sh` | «all checks passed» |
| Парсер реестра под ASan | `g++ -fsanitize=address` | утечка/переполнение не обнаружены после фикса `npos` |
| Ревью диффа «придирчивым ревьюером» | вручную, по каждому коммиту | см. § 4 |

### Что НЕ проверено (и почему)

1. **Компиляция.** В среде нет MSVC, Windows SDK, CEF, DirectX и сабмодулей
   (`Libraries/TiltedCore`, `entt`), `xmake`/`cmake`/`clang` отсутствуют. Ни
   клиент, ни сервер здесь не собираются; проверен только код
   `Code/common/Gameplay/*` (он без зависимостей) и раскладки через
   `static_assert` против эталона.
2. **Поведение в игре.** Всё, что требует запущенного Skyrim, помечено
   «требует проверки в игре» — см. `docs/manual-test-checklist.md` (разделы 1–8,
   включая отдельный раздел «Провал»).
3. **Сетевой слой под нагрузкой.** Логика лимитера покрыта тестами, но реальный
   трафик 4 игроков не измерялся.

## 4. Найденные и исправленные в ходе аудита ошибки

1. **AE-раскладки в порте 1.5.97.** `TESObjectCELL` использовал AE-смещения
   (`refData 0x28`, `sizeof 0xC0`), из-за чего чтение списка ссылок ячейки шло
   по мусору. Восстановлены pre-AE значения; проверяется автоматически.
2. **Знак дельты здоровья** на сервере: урон применялся как лечение при
   отрицательных значениях. Исправлено, добавлена валидация актор-велью.
3. **`Gameplay:bAutoPartyJoin` был объявлен дважды** (`GameServer.cpp` и
   `PartyService.cpp`). Два объекта `Console::Setting` с одинаковым именем — две
   независимые переменные: `set` и ini-файл меняли только первую, сервис партий
   продолжал жить со значением по умолчанию. Теперь одно определение, второе —
   `extern`. Ошибку нашёл новый `tools/check_settings.py`.
4. **Награда доставалась только инициатору.** `ProgressionService::OnAward`
   кредитовал одного игрока, из-за чего реестр расходился с фактическим опытом
   остальных. Теперь кредитуются все подключённые, причём **после** дедупа
   (подавленный дубль не считается дважды).
5. **`ComputeRestore` ломал неотскейленных акторов.** При `base == currentMax`
   функция всегда требовала восстановления, то есть каждый дисконнект
   «откатывал» здоровье существа, которое никогда не масштабировалось
   (единственный FAIL в тестах на тот момент).
6. **Мутация реестра во время итерации** в `ActorValueService::OnDisconnected`:
   удаление компонента внутри `view.each` — неопределённое поведение. Теперь
   сбор сущностей → восстановление → `clear`, локальный игрок пропускается.
7. **HP поздно зашедших игроков.** Игрок, вошедший после масштабирования, при
   выходе оставлял врагов с ×N HP: база не была запомнена. Добавлены
   `ActorValueService::RememberScaledMaxHealth` (при спавне) и обратный расчёт
   `scaledMax / HealthMultiplier` при дисконнекте.
8. **UB при отрицательном счётчике** в `Player::AddRfabXp` (clamp в `uint32_t`
   через промежуточный `int64_t`).
9. **`Find()->` при `npos`** в разборе JSON-реестра — падение на повреждённой
   строке; найдено прогоном под AddressSanitizer.
10. **Бросающее исключение в `noexcept`-конструкторе** `ProgressionService`
    (создание каталога): заменено на `current_path(error_code)`.
11. **Клиент «спорил» с сервером о максимумах.** `OnActorMaxValueChanges`
    применял значение, но не обновлял кэш, поэтому следующий скан отправлял то
    же значение назад. Лишний round trip на каждое изменение скейлинга.
12. **Документация расходилась с кодом**: не были описаны `ModPolicy:bAllowSKSE`
    и `bAllowMO2`, а `GameServer:uQuestHistoryExpiration` в документации уже не
    существовала. Теперь расхождение ловится автоматически.

## 5. Оставшиеся проблемы, риски, ограничения

| Что | Статус / риск |
| --- | --- |
| Сборка не проверялась | **Высокий риск**: возможны ошибки компиляции в клиентском/серверном коде. Первый шаг — собрать и прогнать чек-лист. |
| `ShowLevelUp` RFAB не воспроизводится | Косметика: уровень растёт штатно (ванильный UI сам показывает), но фирменной анимации RFAB на чужом опыте нет. |
| Счётчик фалмеров RFAB (`RFAB_Counter_FalmersKilled`) не синхронизируется | **Не сделано сознательно**: FormID глобала не известен, а придумывать его нельзя. Нужен реальный FormID из сборки — тогда по образцу `SyncRfabExperienceRequest` добавляется за один блок. |
| Дедуп по сумме | Два разных события с одинаковым XP внутри 5 с схлопнутся; отключается `Gameplay:uXpShareDedupWindowMs = 0`. |
| Потолок тотала 10 000 000 | `float`-глобал RFAB теряет точность выше 2^24. |
| Реестр — единственный источник истины | Порча файла приведёт к потере прогресса (пишется через tmp+rename, есть резервный разбор, но бэкапов нет). |
| Сохранения | Сессия при загрузке закрывается по политике; «общий мир» не синхронизируется — см. `docs/multiplayer-saves.md`. |
| CI | Workflow лежит в `docs/ci/checks.yml` — включить копированием в `.github/workflows/` (токен этой среды не имеет права создавать workflow-файлы). |

## 6. Документация

* `CHANGELOG.md` — раздел «RFAB Online (2026-10-22)».
* `BUILD.md` — сборка клиента/сервера и блок «Проверки без Windows и без игры».
* `docs/configuration.md` — все 30 настроек с типами, дефолтами, описанием.
* `docs/RFAB-compatibility.md` — матрица совместимости с RFAB.
* `docs/multiplayer-saves.md` — как устроены сохранения в мультиплеере.
* `docs/rfab-xp.md` — пайплайн опыта, реестр, анти-спам, HUD, ограничения.

### Матрица совместимости с RFAB (кратко)

| Элемент RFAB | Поддержка |
| --- | --- |
| Версия игры | только SE 1.5.97.0.8 (AE не поддерживается) |
| SKSE | 2.0.20 |
| `RFAB_XP_Handler.ModXP` | воспроизведён нативно; пайплайн тот же (RFAB-глобал + `VanilaXPGain`) |
| `RFAB_XP_Settings.json` | множитель должен быть одинаковым у всех (вычисляется на клиенте-инициаторе) |
| `PlayerXP` (FormID `0xAD45BB`) | синхронизируется через реестр |
| Уровневая анимация RFAB | не воспроизводится (HUD-строка) |
| Счётчик фалмеров | не синхронизируется (см. § 5) |
| Порядок загрузки | RFAB → RFAB Online, SKSE-плагин грузится после сборки |

## 7. Ручное тестирование: 1, 2, 3 и 4 игрока

Полный пошаговый чек-лист — `docs/manual-test-checklist.md`. Ниже сжатая
версия; каждый пункт — «да/нет» с ожидаемой строкой в логе.

**Общая подготовка.** Один и тот же коммит у всех, одинаковый мод-лист,
`RFABOnline.ini` в `Data/SKSE/Plugins/`, включённые логи SKSE и сервера.
Сервер запускается первым; `rfabstats` должен отвечать.

**1 игрок (регресс одиночной игры).**
1. Бандит имеет такой же HP, как в одиночной игре; лог `1 player(s) -> x1.00`.
2. Убийство даёт XP, `PlayerXP` совпадает с одиночной игрой.
3. Выход и загрузка сохранения не рвут соединение аварийно; в
   `RFABOnline_saves_index.jsonl` появляется запись `inSession:false`.

**2 игрока.**
4. Бандит с базой 100 HP у обоих показывает максимум 300 (`x3.00`).
5. A добивает — опыт получают оба, у B тоже растёт `PlayerXP`.
6. A выходит из игры в бою — B не замирает, владение врагом переходит к B,
   HP не скачет.
7. A возвращается — максимум остаётся ×3, не ×9.
8. A уходит в другую ячейку и возвращается — HP врагов ×3.
9. Оба завершают один квест одновременно — начислено по одному разу, в логе
   максимум одно `suppressed duplicate amount`.
10. B перезаходит с новым персонажем при включённом `bXpBalanceNewcomers` —
    стартует со среднего опыта сервера (в логе `newcomer`), A не теряет опыт.

**3 игрока.**
11. Множитель ×4 на всех NPC; проверка — `rfabstats` печатает
    `HealthMultiplier 4`.
12. Один игрок в подземелье, двое в другом месте: убийство даёт опыт всем трём.
13. Дисконнект одного во время боя: двое оставшихся видят ×4 (а не ×9 и не ×3).
14. `rfabflush` во время игры не рвёт сессию, реестр на диске обновлён.

**4 игрока.**
15. Множитель ×5; HP врага ×5 от базы (проверить на трёх разных типах NPC).
16. Массовый бой (≥10 NPC): HUD показывает одну строку `RFAB XP +N`, а не
    поток; счётчик при этом растёт сразу.
17. Все четверо выходят и заходят по очереди — HP и опыт восстанавливаются
    ровно в одно и то же значение у всех.
18. Один клиент намеренно шлёт «много» опыта (консоль/скрипт) — сервер
    ограничивает (`rate limit`/`sanitised`), остальные не страдают.
19. Отключение сети у одного на 30 с: после реконнекта реестр досылает
    недостающее, дубли не начисляются.

**Провал (обязательные негативные проверки).**
20. Убить сервер при подключённых игроках — клиенты не падают, показывают
    дисконнект; после рестарта реестр читается (`progress.jsonl`).
21. Испортить строку в `progress.jsonl` — сервер стартует, пишет warning,
    остальные записи читаются.
22. Загрузить сохранение в момент, когда соединение активно — сессия
    закрывается по политике, игра не падает.
