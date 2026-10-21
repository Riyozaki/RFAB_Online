# Байтовые патчи внутри функций: что не переносится на 1.5.97

Часть хуков в клиенте ставится не на функцию целиком (по Address Library ID,
который одинаков для всех версий), а на **фиксированное смещение внутри**
функции:

```cpp
VersionDbPtr<uint8_t> ProcessMessage(52510);
TiltedPhoques::Nop(ProcessMessage.Get() + 0x84E, 6);
```

Смещение `0x84E` снято с SkyrimSE 1.6.x. В 1.5.97 тело функции другое, и та же
запись либо не делает ничего полезного, либо затирает чужую инструкцию. Ожидаемые
исходные байты для 1.5.97 неизвестны (их неоткуда взять без exe), поэтому
проверить «то ли это место» в рантайме нельзя — единственный безопасный вариант
на 1.5.97 **не применять патч** и сообщить об этом в лог.

Реализация: `Code/client/Games/BytePatchGuard.h`,
`CanApplyInsideFunctionPatch(site)`. На 1.5.97 функция возвращает `false` и
пишет `warn` с именем площадки; на 1.6.x поведение не изменилось.

Хуки, которые подменяют функцию целиком (`TiltedPhoques::Jump` по адресу
функции, `TP_HOOK_IMMEDIATE`, подмена vtable), от версии не зависят и этим
guard'ом не затрагиваются.

## 1. Площадки под guard'ом

| Площадка | Файл:строка | ID | Смещение | Что делает | Критичность |
|---|---|---|---:|---|---|
| `BSGraphicsRenderer/InitWindows window style` | `BSGraphicsRenderer.cpp:84` | 77226 | `+0x174+1` | ставит `WS_OVERLAPPEDWINDOW` | косметика (окно без рамки) |
| `BSGraphicsRenderer/SetWindowLong DISCL flags` | `BSGraphicsRenderer.cpp:89` | 68781 | `+0x55+2` | не отдаёт медиа-клавиши | косметика |
| `BSGraphicsRenderer/StopTimer render hook` | `BSGraphicsRenderer.cpp:96` | 77246 | `+9` | `SwapCall` на `OnRender` | важно для рендера/UI |
| `BSTaskletManager/tasklet naming` | `BSTaskletManager.cpp:35` | 69554 | `+0x63` | имена потоков | косметика (отладка) |
| `SkillsMenu/ProcessMessage freeze frame` | `SkillsMenu.cpp:13` | 52510 | `+0x84E`, `+0xA10`, `+0x1040` | меню не «замерзает» | функционально для синхронизации меню |
| `SkillsMenu/controls` | `SkillsMenu.cpp:24` | 52518 | `+0x46`, `+0x4A` | управление в меню | функционально |
| `UI/active queue hook` | `UI.cpp:127` | 82082 | `+0x682` | `SwapCall` на очередь UI | важно для UI-событий |
| `UI/skip startup movie` | `UI.cpp:133` | 36548 | `+0xFE` | `jmp` мимо вступительного ролика | косметика |
| `UI/favorites menu numbering` | `UI.cpp:139` | 51538 | `+0x15` | `nop nop` для нумерации избранного | косметика |
| `Projectile/launched hook` | `Projectile.cpp:154` | 34452 | `+0x374` | JIT-переход на обработчик выстрела | нужно для синхронизации снарядов |

Всего 10 площадок в 5 файлах (11 записей `Nop`, считая три `Nop` в
`SkillsMenu/ProcessMessage`).

## 2. Мёртвые площадки

Эти уже отключены в исходниках (`#if 0`) и на порт не влияют; при возврате к
ним понадобится тот же разбор:

| Файл | Что |
|---|---|
| `Interface/Menus/MapMenu.cpp:8` | три `Nop` из SkyrimSoulsRE (`+0x53`, `+0x9D`, `+0x9F`, ID 53112) |
| `BSSystem/BSThread.cpp:86` | `Nop +0x81` в Havok-потоке (ID 57704) |
| `AI/Movement/PlayerControls.cpp:42` | патч ввода |
| `TESObjectREFR.cpp:618` | эксперимент с `Nop` |
| `Interface/UI.cpp:144` | эксперименты с `Put`/`Nop` по «сырым» адресам |
| `TiltedOnlineApp.cpp:103` | `Put` по «сырому» адресу |

## 3. Отдельная проблема: половина ID с этих площадок не закрыта

Guard защищает от неверного смещения, но не от неверного ID. Проверка по
`docs/phase1/address_id_inventory.csv`:

| ID | Площадка | Состояние |
|---:|---|---|
| 77226 | InitWindows / Renderer_Init | закрыт (NG) |
| 77246 | StopTimer | закрыт (NG) |
| 69554 | tasklet naming | закрыт (NG) |
| 68781 | DISCL flags | **не закрыт** |
| 52510 | SkillsMenu ProcessMessage | **не закрыт** |
| 52518 | SkillsMenu controls | **не закрыт** |
| 82082 | UI active queue | **не закрыт** |
| 36548 | UI startup movie | **не закрыт** |
| 51538 | UI favorites | **не закрыт** |
| 34452 | Projectile | **не закрыт** |

То есть 7 из 10 площадок нельзя включить даже на 1.6.x, пока не закрыты ID:
`GetPtr()` вернёт null, и запись уйдёт по мусорному адресу. На 1.5.97 они
дополнительно закрыты guard'ом, так что до старта игры это не мешает.

Порядок: сначала закрыть эти 7 ID (они уже есть в `id_worklist.csv`), потом
пере-развернуть смещения для 1.5.97.

## 4. Как пере-развернуть площадку для 1.5.97

1. Взять ID из таблицы выше, разрешить его в 1.5.97 через
   `id_pairs_curated.csv` (если ещё не закрыт — `id_worklist.csv`).
2. Найти функцию в `SkyrimSE.exe` 1.5.97 по её адресу и открыть место,
   соответствующее описанию из столбца «Что делает»:
   * стиль окна — `mov` с `WS_OVERLAPPEDWINDOW` (0x00CF0000) внутри
     `BSGraphics::InitWindows`;
   * `DISCL` — `mov` третьего аргумента `SetWindowLong`/`CreateWindowEx`;
   * `StopTimer` — вызов таймера в конце кадра;
   * `SkillsMenu` — три `Nop`/`jcc` в `ProcessMessage` и два в обработчике
     управления;
   * `UI` — вызов `UI::AddToActiveQueue`, `mov byte` перед роликом, `mov word`
     в проверке избранного;
   * `Projectile` — вызов в `Projectile::Launch`, после которого JIT
     возвращается на `+0x379`.
3. Записать новое смещение и **тот же** `CanApplyInsideFunctionPatch` заменить на
   проверку профиля: смещение действует только для той версии, с которой оно
   снято. В идеале — две константы и выбор по `VersionDb::Get().IsPreAnniversary()`.
4. Проверить в игре (см. чеклист в `README_RU.md`, § 4).

## 5. Что не проверено

* Ни одна площадка не проверена в игре ни на 1.5.97, ни на 1.6.x.
* Для 7 площадок неизвестен даже ID, поэтому их смещения нельзя проверить
  статически.
* Guard проверен только чтением кода: он не компилировался под MSVC (в среде
  разработки нет Windows).
