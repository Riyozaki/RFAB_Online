# Ф1: долги по стабильности (краши, утечки, UB, disconnect)

Статус на 2026-10-21, ветка `arena/014f2836-rfab-online`.
Цель этого документа — держать в одном месте всё, что найдено статическим
разбором клиента, с указанием файла/строки, статуса и способа проверки.

**Важно:** MSVC-сборки и запуска в игре не было (нет Windows/Skyrim/SKSE/RFAB).
Всё, что помечено «исправлено», проверено только чтением кода и, где возможно,
офлайновыми скриптами из `code/tools/port1597/`. Поэтому у каждого пункта есть
пометка, как его проверить руками — см. раздел 5.

Обозначения:

* ✅ — исправлено в этой итерации, требует проверки в игре;
* 🟡 — сознательно оставлено, описано и назначено на соответствующую фазу;
* ⛔ — нельзя закрыть без стенда (Windows + Skyrim 1.5.97 + RFAB).

---

## 1. Исправлено (✅, в игре не проверялось)

| # | Место | Проблема | Что сделано |
|---|-------|----------|-------------|
| 1 | `Games/Memory.h:17` | `Memory::New<T>()` и `Memory::New<T>(args...)` выделяли память **дважды**: первый блок (`pMemory`) никуда не сохранялся и терялся. Каждая тинтованная текстура лица и каждый `Memory::New` в коде теряли `sizeof(T)` навсегда. | Лишняя аллокация убрана, оба overload'а делают одно размещение. |
| 2 | `Games/Skyrim/Projectiles/Projectile.cpp:105` | В `Projectile::Launch` (путь `LaunchData`) результат `RealLaunch` не проверялся: `result->handle.iBits` при `nullptr`. `TP_ASSERT` не спасает — `Utils::Assert` только пишет в лог. | Ранний выход с `spdlog::error`, событие запуска не отправляется. |
| 3 | `Games/Skyrim/Projectiles/Projectile.cpp:112` | `Event.Power = pProjectile->fPower` без проверки `Cast<Projectile>` (проектил мог не найтись). | При отсутствии проектила берётся `arData.fPower`, пишется `warn`. |
| 4 | `Games/Skyrim/TESObjectREFR.cpp:485` | `GetItemFromExtraData`: `pExtraEnchantment->pEnchantment` разыменовывался до проверки (мод-предмет с «пустым» enchantment extra data = краш при сериализации инвентаря). | Ветка под `if (!pEnchantment)` с логом, запись сериализуется без данных зачарования. |
| 5 | `Games/Skyrim/TESObjectREFR.cpp:592` | `GetExtraDataFromItem`: `nullptr` от `EnchantmentItem::Create`/`GetById` уходил в игровой сеттер `SetEnchantmentData` → краш при получении предмета с отсутствующим модом (типичный случай для сборки RFAB на другом клиенте). | Сеттер вызывается только при валидном указателе, иначе `spdlog::error` и предмет создаётся без зачарования. |
| 6 | `Games/Skyrim/TESObjectREFR.cpp:603` | Яд отдавался с temporary FormID (`ModId == 0xFFFFFFFF`): `GetGameId` возвращал мусор, яд вешался на случайный предмет. Раньше это был только `TP_ASSERT`. | Яд отбрасывается с `error`, предмет добавляется без яда. |
| 7 | `Games/Skyrim/TESObjectREFR.cpp:656`, `ExtraData/ExtraDataList.{h,cpp}` | Владение `ExtraDataList` было размазано: `New()` выделяет список + отдельный `bitfield`, а освобождение делалось вручную в одном месте, деструктор был **объявлен и не определён** (любой `delete` — ошибка линковки). | Добавлен `ExtraDataList::Delete()` (bitfield + список), деструктор определён; ручное освобождение заменено вызовом. |
| 8 | `Games/Skyrim/Actor.cpp:1188` | `DropOrPickUpObject`: при ненайденном `pObject` список extra data, выделенный строкой выше, терялся (утечка на каждый неудачный дроп). | Освобождается через `ExtraDataList::Delete`. |
| 9 | `Services/OverlayService.h:69` | `GetOverlayApp()->ExecuteAsync(...)` в 22 местах без проверки: оверлей создаётся лениво (CEF), при его отсутствии — разыменование `nullptr` в обработчиках событий (в т.ч. `DisconnectedEvent`). | Добавлен `OverlayService::ExecuteAsync(...)` с проверкой `m_pOverlay`; все вызовы внутри сервиса, в `PartyService` и `InputService` переведены на него. |
| 10 | `Services/Generic/DiscordService.cpp:42` | Поток `run_callbacks` создавался как `static std::thread` с `detach()` и захватом `this`: жил дольше сервиса, обращался к `m_pCore` после разрушения, `m_bRequestThreadKillHack` — обычный `bool` из двух потоков (гонка). Ядро Discord и `discord_game_sdk.dll` никогда не освобождались. | Поток стал членом класса, флаг — `std::atomic<bool>`, деструктор ставит флаг, делает `join()`, `destroy()` ядра и `FreeLibrary`. На ошибке `GetProcAddress` dll больше не течёт. |
| 11 | `Services/Generic/ActorValueService.cpp:80` | `OnDisconnected` вызывал `m_world.clear<ActorValuesComponent>()` **внутри** рассылки `DisconnectedEvent`, пока другие подписчики (`CharacterService`) удаляют актёров и чистят компоненты. Комментарий в коде прямо говорил «this crashes sometimes». Плюс `on_construct<LocalComponent>` подключался без сохранения connection — отключить нельзя. | Очистка отложена до начала следующего `OnUpdate` (флаг `m_clearActorValuesOnUpdate`), оба connection'а сохранены и освобождаются в деструкторе. |
| 12 | `Services/Generic/PlayerService.cpp:70` | `OnConnected`/`OnDisconnected`/`OnPartyJoined`/`OnPartyLeft` разыменовывали `Cast<TESGlobal>(TESForm::GetById(0x100F19 / 0xB8EC1))` без проверки. Если глобал перекрыт модом или отсутствует — краш в момент connect/disconnect. `PlayerCharacter::Get()` в disconnect тоже без проверки (отключение из главного меню). | Все четыре места под `if`, disconnect не падает без персонажа, добавлены `warn/debug` логи. |
| 13 | `Services/Generic/PartyService.cpp:124` | То же для `0xB8EC1` в `OnPartyInfo`. | Под `if`. |
| 14 | `ScriptExtender.cpp:54` | `VerQueryValueA` писал `UINT` в `sizeof`-переменную типа `DWORD` через `reinterpret_cast` (алиасинг разных типов), результат `pvi` не проверялся на `nullptr`. | Отдельная `UINT versionInfoSize`, `pvi` инициализируется и проверяется. |
| 15 | `ScriptExtender.cpp:77` | `GetSKSEStyleExeVersion` резал версию по `find_last_of("_0")` — для `1_6_640_0` получалось `1_6_6`, т.е. поиск `skse64_1_6_6*.dll` не находил ничего (тихий отказ загрузки SKSE). Для 1.5.97 маскировалось тем, что в версии нет нулей, кроме последнего. | Отрезается только суффикс `_0`. |

## 2. Проверено и признано безопасным

| Место | Вывод |
|-------|-------|
| `Services/Generic/DiscordService.cpp:23-24` | `strcpy_s` в `details`/`large_image` пишет литералы 7 и 4 байта в поля `char[128]` из `discord.h` — переполнения нет. |
| `Games/Memory.cpp` | Пара `Memory::Allocate` / `Memory::Free` использует один и тот же игровой heap (`HookFormAllocate` / `RealFormFree`), `Memory::Delete` вызывает `Free`. Рассинхрона аллокаторов нет. |
| `Systems/FaceGenSystem.cpp:96-125` | `tints.data`, `tints[i]`, `texture` освобождаются тем же способом, которым выделены; деструктор `TESTexture` вызывается явно. После исправления `Memory::New` утечки нет. |
| `Games/Skyrim/TESObjectREFR.cpp:640-654` | Закомментированный блок с `*((uint64_t*)pExtraText) = 0x1416244D0` — это AE-адрес vtable, в 1.5.97 он невалиден. Блок не включать; путь `ExtraTextDisplayData` остаётся отключённым (см. 3.4). |
| `Games/Overrides.h` | `ScopedOverride` — RAII со `thread_local` счётчиком; корректен, пока создание/разрушение происходят в одном потоке (так и есть: обёртки вокруг игровых вызовов с главного потока). Ограничение отмечено в 3.5. |
| `Games/Misc/BSScript.cpp:36-38` | `new NativeFunction` передаётся в `BindNativeMethod` — владение забирает виртуальная машина; освобождать вручную нельзя. Утечки на старте нет. |

## 3. Известные долги, оставленные сознательно

### 3.1 ⛔ 94 ID из 3083 не сопоставлены

Подробности и процедура — `docs/phase1/IDS_RU.md`. Остаток: 79 pointer-ID, 13 versiondb, 2 rtti.
Наиболее критичные для играбельного минимума: `ExtraDataList` (5), `Actor` (15),
`TESObjectREFR` (9), `CombatController` (4), `MagicTarget` (4), `PlayerCharacter` (5).
Без exe 1.5.97 закрыть нельзя: нужны перекрёстные ссылки в IDA.

### 3.2 ⛔ 7 ID байтовых патчей

`docs/phase1/BYTEPATCHES_RU.md`: 68781, 52510, 52518, 82082, 36548, 51538, 34452.
Патчи под `BytePatchGuard` (на 1.5.97 пропускаются с `warn`), но их смещения не подтверждены.

### 3.3 🟡 Синхронизация здоровья игроков отключена

`Services/Generic/ActorValueService.cpp:319` — `SetPlayerHealthPercentage` закомментирован
(«find fix for player health sync»). Это влияет на HP-бар в оверлее, а не на геймплей.
Ф2.

### 3.4 🟡 Custom name предметов (`ExtraTextDisplayData`) отключён

`Games/Skyrim/TESObjectREFR.cpp:640-654`. Причина — тот самый AE-vtable. В 1.5.97
нужен корректный конструктор `ExtraTextDisplayData` (функция не найдена в базе ID).
Ф2/Ф5.

### 3.5 🟡 `ScopedOverride` не виден другим потокам

`Games/Overrides.h:3-14`: счётчик `thread_local`. Если игра вызывает перехваченную
функцию с рабочего потока (например, загрузка ячейки), override не сработает и
побочный эффект продублируется. Сейчас это теория: нужно подтвердить стеком вызовов
на стенде. Ф2.

### 3.6 🟡 XP: только в пати и без сброса кэша

`Services/Generic/CharacterService.cpp:1752` — `RunExperienceUpdates` выходит, если
игрок не в пати, **не очищая** `m_cachedExperience`. Вне пати опыт копится и потом
уходит одним пакетом при вступлении в пати (расхождение с ожидаемым поведением и
потенциально огромное значение в пакете). Правильное решение — серверные
авторитетные события, это задача Ф4; менять сейчас поведение без требований Ф4
не стал.

### 3.7 🟡 `ModSystem` сравнивает моды по имени файла

Ф5 (build identity hash). Для 1.5.97+RFAB это станет источником ложных
«мод отсутствует» при переименованных архивах.

### 3.8 🟡 `GetByServerId` — линейный поиск по view

`Services/Generic/CharacterService.cpp` и соседние сервисы: O(N) на каждый пакет.
При 4 игроках не проблема; оптимизация — Ф2 (не трогаем, чтобы не менять структуру
до стабилизации).

### 3.9 🟡 Статические `new` в `ModCompat/BehaviorVar.cpp`

`new Replacer()`, `new AnimationGraphDescriptor()` живут до конца процесса. Это
осознанное решение (реестр модов), не утечка в цикле. Оставлено.

### 3.10 🟡 Overlay: `Initialize()` может вернуть `false`, но сервис продолжает работать

Теперь это безопасно (все вызовы через guard), но UI просто не покажется.
Добавить явный `m_bAvailable` и сообщение в лог — Ф2.

## 4. Классы ошибок, которые остались в коде (по grep)

Это не «место», а шаблон, который надо вычищать по мере касания файлов:

1. `cast`→`->` без проверки: `Cast<X>(TESForm::GetById(...))->...`. Проверка:
   `grep -rn "Cast<.*>(TESForm::GetById(.*))->" Code/client`.
2. `TP_ASSERT(...)` перед разыменованием: макрос **только логирует**
   (`Utils.h:11`), гарантий не даёт. Проверка: `grep -rn "TP_ASSERT" Code/client`.
3. `m_world.get<T>(...)` / `view.get<T>(...)` без `try_get` — падает при
   отсутствии компонента. Проверка: `grep -rn "\.get<" Code/client`.
4. `strcpy_s`/`strncpy_s` — проверять `sizeof` приёмника, а не длину источника.

## 5. Ручной чек-лист (когда появится стенд)

Порядок: сначала сборка, потом каждый пункт в игре.

**Сборка**

1. `xmake f --skyrim_profile=1597` (дефолт), `xmake build` — MSVC, без ошибок и
   предупреждений уровня `/W4` в изменённых файлах.
2. Запуск: в логе `SkyrimSE 1.5.97.0.8 detected` и нет `exit(6)`.
3. Подключение к серверу, `TP_LOG_LEVEL=debug` — в логе есть `Connected`.

**Пункт 1 (Memory::New)**

4. Зайти в игру, открыть расу/лицо с тинтами (создание персонажа или
   `player.setrace`), 3–4 раза перегенерировать лицо; в диспетчере задач память
   не растёт монотонно на десятки МБ.

**Пункты 2–6 (предметы/проектилы)**

5. Выстрел из лука/заклинанием: в логе нет `No projectile handle returned`;
   у других игроков виден проектл.
6. Взять со стола предмет с зачарованием из мода, которого нет у второго
   клиента: второй клиент **не падает**, в его логе `enchantment ... was not found`.
7. То же с отравленным предметом и с temporary FormID — в логе `poison ... dropped`,
   падения нет.
8. Дропнуть предмет, которого нет в загрузке (`player.drop` с битым FormID):
   в логе `Object to drop not found`, игра жива.

**Пункты 9–10 (оверлей, Discord)**

9. Запустить без `discord_game_sdk.dll`: присутствие не работает, игра не падает,
   в логе нет ошибок Discord.
10. Запустить с Discord и **выключенным** оверлеем Discord: статус «Playing»
    обновляется (раньше `!m_pOverlayMgr` отключал presence).
11. Выйти из игры (`WM_QUIT`): процесс завершается без зависания и без
    «pure virtual call»/«access violation» в логе.

**Пункт 11 (disconnect)**

12. Подключиться, зайти в мир, убить пару NPC, выйти из игры штатно; затем
    подключиться снова — падений нет.
13. Оборвать сервер (`docker stop`) при живом клиенте: клиент пишет
    `Disconnected from server`, возвращается в меню, не падает; повторный заход
    на сервер работает.

**Пункты 12–13 (глобалы)**

14. Подключиться/отключиться из главного меню (без загрузки сейва) — нет падения.
15. С модом, переопределяющим `0x100F19`/`0xB8EC1` (или с удалённым плагином):
    в логе `warn ... not found`, игра жива.

**Пункты 14–15 (SKSE)**

16. С `skse64_1_5_97.dll` и `skse64_loader.exe` в каталоге: в логе
    `Starting SKSE v2.0.20.0`, затем `SKSE is active`.
17. Подменить dll на `skse64_1_6_640.dll` — в логе «too old/не найден», без падения.

## 6. Что нужно от стенда, чтобы закрыть остаток Ф1

* `SkyrimSE.exe` 1.5.97.0.8 (для IDA: 94 ID + 7 смещений патчей);
* SKSE64 2.0.20 (проверить реальные поля version-ресурса — см. п. 14 выше);
* сборка RFAB целиком (для проверки FormID/ESL и `ModSystem`);
* 2 клиента (для disconnect/crash-сценариев).
