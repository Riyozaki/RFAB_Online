# RFAB clean-core rebuild — журнал

## 2026-11-17 — решения после аудита

Реализация всего согласованного объёма разрешена. Один полный кандидат и один совместный игровой тест в конце; промежуточные двухпользовательские тесты и релизы не требуются. Порядок внутренних работ из исходного плана сохраняется, проверка каждого изменения — автоматическая. Эти решения заменяют требования к промежуточным игровым проверкам и «открытые вопросы» исторического аудита.

- Подъём: удержание 5 секунд; group wipe: отсчёт 5 секунд. Ранение не истекает при живом участнике.
- XP: равная **базовая** награда, личные множители и независимые траты RFAB.
- На двоих враги ×2 HP, без увеличения числа спавнов.
- A/reset encounter основной (включая проверенные outdoor anchors); B/checkpoint отдельно и опционально.
- Городские защищённые сундуки — общее постоянное хранилище кампании, торговцы пока личные.
- Установленная новая RFAB не находится на GitHub; её не ищем. Публичные скрипты — справочные, не доказательство установленного ABI. Для обновлений и Bizarre требуются проверяемые capabilities/adapters.

## Основание

Содержимое Code/ и Tools/ импортировано из RFABTogether 64e432ee7e397e79c3ff63a684667eced8bad331 (STR 13ff0b5a934bf3016c8c898d9a019aa04e5c1fb0). История текущего репозитория, branch, submodule pins, GameFiles и теги сохранены. Старые сервисы не перенесены обратно. Provenance — RFAB_CLEAN_BASELINE.json.

Идентификатор разработки: 5.0.0-dev.1. README и эффективный STServer.ini больше не выдают удалённые механики за работающие. Build-CloudRelease.ps1 блокирует публикацию при readiness=false и для -dev.*; диагностические архивы возможны. Не было commit/push/tag/release.

## Текущее состояние проверки

До импорта stock core и stock server тестировались в отдельном неизменённом снимке. Они **не** являются результатом проверки текущего дерева. Дополнительный probe там воспроизвёл неправильный знак server HP и двойной забор последнего предмета. Исправления должны сопровождаться постоянными тестами production-путей в текущем дереве.

Клиент Skyrim, ABI/адреса установленной игры, AI и реальные анимации здесь не запускались. Windows build, полная автоматическая матрица и упаковка ещё не подтверждены. ready_for_playtest остаётся false.


## 2026-11-17 — regression-first foundation и первая production-связка co-op

### Выполнено

- RED в корневом server harness: четыре нарушения инвариантов; команда
  `JOBS=4 Tools/rfab/linux-server/run-server-tests.sh --plain`, exit 1, 442.1s.
- Signed HP arithmetic → canonical revisioned HP, origin sequences/acks, ownership epoch;
  запрет абсолютного HP в обычном AV snapshot и управление своим avatar. Первичный direct
  harness после фикса был зелёным, но это не заменяло wrapper/crash check.
- Inventory: отрицательный остаток/overflow отклоняются без broadcast; generated enchantment
  effects входят в stack equality. **Арбитража source→recipient, receipts и persistence пока нет**.
- Core первоначально остановился на отсутствующем `<optional>` в Inventory.h. Includes
  исправлены; повторный запуск прошёл **32 cases / 15511 assertions**, 52.8s.
- Bounded fixed-width `CoopProtocol`, append-only action opcodes/factory registration;
  session/generation/operation checks в production `CoopService` сервера и клиента. Сервер
  возвращает только реально подключённые capabilities, не обещает XP/storage/checkpoint.
- `EncounterRuntime`: две персоны, indefinite downed, пять секунд непрерывного hold и пять
  секунд wipe; отмены по hit/move/loading/disconnect/heartbeat; downed revision против старого
  bleedout; живой участник в другой ячейке всё ещё предотвращает group wipe.
- Anchor требует stationary static-ground dwell; после начала боя frozen. Exterior-cell
  сама по себе не изменяет anchor; нет выдуманных смещений, overlap/arena/missing anchor
  останавливают reset. Prepare→Commit→Complete с ACK и timeout; частичный отказ блокирует группу.
- Client native glue: actor-reference essential/no-recovery с сохранением исходных флагов
  (не правит общий TESNPC template), native bleedout event, hold key из локального INI, atomic
  downed/input snapshots, отложенный post-native readiness ACK и защита от повторного teleport.
  Старый 5s upstream death-respawn отключается настройкой negotiated RFAB **до** его таймера.
- Движение и actions получили epochs. После revive/reset старый unreliable movement не может
  вернуть avatar назад. Старый replacement AI UpdateTarget не включён; узкий native SetTarget
  фильтрует downed по actor-local atomic, без доступа к registry в AI hook.
- Важная найденная проблема импортированного Runner: vendor TaskQueue::Drain исполнял callback
  под mutex. Заменён на bounded `GenerationQueue`: detached batch, callbacks вне lock, вложенный
  post на следующий кадр, invalidation при load/disconnect. Overflow явно разрывает соединение.

### Исполненные проверки текущего направления

- `JOBS=2 Tools/rfab/run-core-tests.sh`: **40 cases / 17181 assertions**, exit 0, 14.1s
  (первый запуск новых тестов сначала выявил missing factory includes; исправлено).
- После queue и ResetParticipant: `JOBS=3 Tools/rfab/run-core-tests.sh` —
  **42 cases / 25407 assertions**, exit 0, 4.4s. Последующее добавление unsafe-encounter latch
  ещё требует повторного core/sanitizer run.
- `JOBS=3 Tools/rfab/linux-server/run-server-tests.sh --plain`: **exit 0**, 65.8s, четыре
  настоящих production-wire сценария плюс crash-record self-test. Новый сценарий проверяет
  Hello, чужие session/generation, indefinite latch, запрет legacy death/regen bypass,
  5s revive/25% HP/new epoch и отказ pre-revive movement. Сеть заменена outbox spy, не сервисы.
- `Tools/rfab/run-static-checks.sh`: exit 0, но это **не полная hook/lifetime проверка**:
  импортированный aggregate пока запускает их self-tests, а не аудит production-кода.
  Прямой `check_hook_discipline.py` остаётся **RED: 84 findings**, включая устаревшие элементы
  политики старой архитектуры и реальные inherited thread/TLS риски. Это не 84 доказанных CTD.
- Wire audit: 0 новых / 0 stale / 8 известных baseline записей. SE audit: 3101 map rows,
  3078 sites / 3073 unique IDs / 0 unmigrated, 11 старых struct assertions. Это не проверка ABI
  установленного executable. Ground controller layout взят только как reference из
  CommonLibSSE-NG `b93280e832f263dbef44e44cbe2936622a02f91a`; runtime grounding не доказан.
- Lua tmpnam linker warning не фатален. Клиент Windows/PowerShell/package/Skyrim и последние
  sanitizer runs пока отсутствуют. Исправление BOM у пяти PS1 не считается их выполнением.

### Что ещё не готово

Нет semantic RFAB XP adapter, HP×2, настоящих atomic loot transfers, persistent shared storage,
QuickLoot/AutoLoot wiring или checkpoint B. Recovery/save boundaries, reconnect через restart,
HP handoff races, native AI/animation/teleport и совместимость с установленной RFAB требуют
дальнейшей реализации и проверки. Эти пункты не заменены ни протоколом, ни чистой моделью.

Один полный кандидат / один итоговый плейтест сохраняются. `ready_for_playtest=false`.
Текстовые логи в excluded build/ не являются единственным носителем доказательств: численные
результаты и ограничения записаны здесь. Текущий turn не публикует игровой релиз.


## 2026-11-17 — inventory production path, native timeline и co-save bridge

Эта запись заменяет устаревшее «текущее состояние» предыдущих снимков, не переписывая историю.

- Server inventory: paired durable reservations и exact-item fingerprints (включая присутствие
  **нулевого** charge/soul extra), epoch/revision fences для legacy delta/snapshot/equipment,
  отказ unilateral shared-container/corpse delta. Faulted ledger больше не возвращает старый
  Prepared; точный повтор envelope получает durable ответ, collision по тем же sequence/nonce
  не становится новой операцией. Закрыт live-NPC reason bypass; pickpocket adapter пока отсутствует.
- Production-wire scenario5: два taker-а/одна вещь, до native receipt endpoints не меняются,
  повтор envelope/receipt, stale owner snapshot, reconnect той же bag, restart журнала,
  отказ старой ESS bag. Сервисы/сериализаторы настоящие, socket заменён outbox recorder.
- Client LootService зарегистрирован, получает валидированные co-op actions; TESObjectREFR,
  Actor и Player RemoveItem перехвачены до переноса. Хранит только form/server IDs, повторно
  разрешает живой exact ExtraDataList, вызывает исходный paired native transfer, сверяет оба
  endpoint-а. Не использует BaseId rollback/RemoveAllItems при расхождении.
- CharacterState/ClientTimeline: write-ahead Prepared → Applied → Committed; cursor в **SKSE
  co-save** и отдельном durable per-character receipt. Старый save, missing/corrupt receipt,
  crash в промежуточном состоянии или failed write блокируют automatic execution. Это не
  checkpoint B и не обещание exactly-once при произвольном стороннем Save/Load.
- Добавлен узкий RFAB_CoopState.dll (SKSE64 2.0.20 API, pinned ianpatt/skse64 0d6c91dc...)
  с versioned callback table экспортируемого launcher; engine/ECS не доступны сериализующим
  callbacks. Только immutable owned bytes, official ResolveFormId и atomic load fences.
  Новая персона сначала сохраняется offline и загружает co-save; capability до этого не включён.
  Outer Save/Load gates и подтверждённый SKSE PostLoadGame (включая failure) связаны с World.
- Campaign и character journal получают OS exclusive lease, без unsafe unlink lock-файла.
- Revive читает именованное **Activate** action из engine ButtonEvent (keyboard/mouse/gamepad),
  не H/E virtual key. Документированные InputEvent layout asserts добавлены. Runtime ABI не доказан.
- AV watermark codec теперь bounded, фиксированной ширины, rejects duplicate origins/nonfinite/
  truncation; enclosing factories отвергают invalid child, а не интерпретируют его как empty.
  Network build identity получил coop-wire suffix, чтобы старый dev codec не подключался молча.

Проверено до последнего mapped-input/package-only изменения:

- core plain: **63 cases / 26 675 assertions**, exit0, 5.1s.
- core ASan+UBSan: **63 / 274 427**, exit0, 210.9s; RFAB_FUZZ_SEEDS=128,
  RFAB_SOAK_SEEDS=512, mimalloc ASan tracking. Native engine не входит в target.
- server plain: **5 production-wire scenarios + crash-record self-test**, exit0;
  clean-deps build434.7s, затем lease/header rebuild79.8s.
- strict static: exit0/3.8s; hook/lifetime0, wire0new/0stale/8known; SE3086sites/
  3081unique/0AE. PowerShell отсутствует локально. Source/ID проверки не заменяют ABI/Skyrim.

Остаются в полном объёме: installed-profile storage backing resolver/generator, availability/
campaign authorization, save reconciliation edge cases, semantic XP adapter, save-bound HP×2,
полное retreat A/AI/task quiescence и optional genuine B. Новый client/bridge требует Windows
build, package/fault проверки и единственного финального игрового теста. ready_for_playtest=false.


## 2026-11-17 — installed-profile storage / first Windows diagnostic

- Запущена Windows diagnostic сборка текущей рабочей ветки, не release/playtest:
  `44135805841` / commit `3f70c8e`. **FAIL** на зависимости CommonLib → SkyrimEncoding
  (`Structs/Inventory.h` отсутствовал в include path). Явное xmake dependency исправлено;
  новый native код этим запуском ещё не подтверждён. Загрузка log-zip недоступна из этой
  среды (redirect host закрыт); compiler evidence получено из GitHub Checks annotations.
- Linux CI того же commit: core, sanitized core, sanitized server и UI jobs **success**.
- Добавлен offline `generate_storage_profile.py`: вход — только установленные активные
  plugins/masters, VMAD v1/v2 object format, winning overrides и реальные CONT-backed REFR.
  Нет поиска «текущего RFAB», hardcoded FormIDs или изменения плагинов. Один backing из разных
  городов — один ключ. Alias/null/non-container/duplicate/truncated/decompression-bomb cases
  отвергаются. Плагин с пробелом в имени не режется на ложные TSV-поля. 12 synthetic tests pass.
  Формат сверён с TES5Edit `43652b9932a9e115105fc018a87045bf1fbce083`,
  `Core/wbDefinitionsTES5.pas` / `wbScriptPropertyObject`.
- Common StorageCatalog читает TSV all-or-nothing; оба конца согласуют canonical signature
  (mismatch detector, не security hash). Network build теперь `+coop-wire-3`, Hello v3.
- Журнал кампании после restart не создаёт фиктивные plugin IDs. Decode отложен до
  согласованного списка клиента, lookup read-only, light/standard kind проверяется. Неудачный
  Hello больше не rebind-ит старого downed-участника и не занимает место в группе.
- Client StorageService wired: resolver access/backing, off-cell persistent binding, явные
  console `rfab_storage_import` (только незаполненный campaign bank) / `rfab_storage_attach`
  (только пустой/уже совпадающий local cache), lease refresh только у открытого nearby access.
  Непустое отличающееся старое личное содержимое не стирается и не сливается автоматически.
  Cache reconciliation использует собственный namespace в общем native receipt timeline;
  CharacterState v2 сохраняет campaign/form/revision adoption и читает v1. SKSE ResolveFormId
  применяется и к этим ref IDs. Известные campaign backings offline read-only.
- Native storage wiring пока **не проверен Windows/Skyrim**, production-wire storage scenarios
  ещё дополняются. Второй старый личный stash с отличающимся содержимым требует recovery policy,
  а не «первый opener победил». Это не отметка закрытия всех требований к storage.

На момент записи: core **65 / 26 817**, server прежние **5 scenarios + crash PASS** (368.4s),
strict static exit0, Python storage profile **12 PASS**, pwsh локально отсутствует.
Внешние research .cache-файлы убраны из Git и добавлен ignore; runtime/generated profile data
тоже не включаются в исходники. Artwork пользователя не менялся. ready_for_playtest=false.


Дополнение Windows diagnostic `44135805841`: после получения всех Checks annotations видна
ещё независимая ошибка companion: имя переменной `s_host` раскрывалось в legacy макрос
winsock.h (`S_un_b/s_b2`). Переменная переименована, target RFABCoopState добавлен отдельной
ранней группой в Build-CloudRelease, чтобы эта маленькая проверка не ждала сборки CEF.
Windows run для `8218a8a` отменён как заведомо содержащий тот же макрос; Linux CI не отменялся.


## 2026-11-17 — hidden-bank production-wire regression / native cache race / safe update

- Шестой production-wire сценарий: два города/две группы → один hidden backing, explicit и
  idempotent import, concurrent take, paired deposit, cross-cohort cache update **без** snapshot
  чужой personal bag, expiry/поддельный access/lock/foreign-extra-plugin refusal, restart сначала
  с отсутствующим plugin (отказ без выдуманных ID), затем с правильным profile (точное восстановление).
  Hello проверяет каждый catalog access/backing plugin и standard/light kind. Это не полная
  защита от злонамеренного клиента: координаты самого access пока не подтверждены серверным
  installed-profile position catalog; native ≤256 отдельно проверяет клиент.
- Найдена и закрыта duplication-гонка: native paired move уже Applied, а bank snapshot всё ещё
  старой revision. Same-revision + different contents теперь **ожидает новую revision**, а не
  клонирует предмет назад. Storage stamp разрешено продвинуть только из Applied transfer receipt
  до его Commit; immutable committed receipt содержит исходные post-transfer revisions, даже
  если последующие операции изменили bank. Добавлен чистый core regression и wire retry после deposit.
- Полный installer больше не удаляет кампанию вместе с binary directory: stage + SHA256 verify,
  exclusive runtime lease checks, перенос предыдущего дерева в RFAB_Online_Backups, byte-exact
  сохранение character receipts, campaign journals и generated profile. Ни full, ни quick update
  не force-kill-ят сервер. ESS/SKSE save pairs больше не классифицируются/перемещаются автоматическим
  header-эвристическим «ремонтом». Companion обязателен в full/install/deploy и PE import gate.
  Новый PowerShell self-test wired до Windows C++ build и в Linux static gate, но локального
  pwsh нет: **native PowerShell pass пока не заявляется**.

Результаты на рабочем дереве (не game validation): core plain **66 / 26 823**, core
ASan+UBSan **66 / 274 595** (128 fuzz / 512 soak); server plain **6 scenarios + crash-record
PASS**. Full static PASS, pwsh явно skipped. Новый server sanitizer build выполняется; первый
параллельный запуск остановлен после OOM compiler (6 simultaneous sanitized compilers > RAM),
это инфраструктурный fail, не успешный прогон. Linux CI8701c61/run44140814737 **success**.
Windows44140814679 пока in_progress; он не включает последние правки этого раздела.

Никакого release/tag/второго игрового теста: VERSION5.0.0-dev.1, readiness=false. Всё ещё нужны
semantic XP, save-bound enemy HP×2, genuine retreat A/AI-task/hazard quiescence, save/load edge
failures, полный Windows package и один финальный gameplay test; optional B не выдаётся за A.


### Windows follow-up: не выдавать PCH за header dependency

Windows8701c61/run44140814679 завершился **FAIL**: CommonLib не имеет PCH, а новые ledger/catalog
тянули старые upstream GameId/Inventory headers, полагавшиеся на EncodingPch.h. Добавлены direct
cstdint/functional/Buffer/Stl includes; Linux common TUs теперь тоже собираются **без** PCH.
Новый first-include test отдельно разбирает все17 co-op/wire boundary headers — локально PASS.
Исправлен lazy REPO_ROOT Makefile: included .d не должен менять значение корня в поздних recipes.
Windowsa183409/run44144088071 остановился ещё в новой PowerShell preflight до native build;
в текущем запуске причина не попала в Checks annotation (сырой log redirect недоступен). Preflight
перенесён в существующий Invoke-LoggedCmd, чтобы следующий запуск показал конкретную ошибку.
Никаких выводов о native/installer success из portable pass не делается.
