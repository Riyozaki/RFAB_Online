# Фаза 1: аудит внутрифункционных патчей под 1.5.97

Статус на 2026-10-21: **оффсеты в коде пока от 1.6.x и для 1.5.97 НЕ проверены.**
Ни один оффсет не будет «угадан»: каждый подтверждается дизассемблированием
функции в exe 1.5.97 (старт функции известен из базы Address Library RFAB).

## Живые места патчей (полный список по поиску в коде)

| # | Файл | ID | Функция RVA (1.5.97) | Патч (оффсет от 1.6.x) | Назначение |
|---|---|---|---|---|---|
| 1 | `Games/Skyrim/BSGraphics/BSGraphicsRenderer.cpp` | 77226 | `0xDEE380` | Put `WS_OVERLAPPEDWINDOW` в операнд по `+0x174+1` | Стиль окна игры |
| 2 | `Games/Skyrim/BSGraphics/BSGraphicsRenderer.cpp` | 68781 | `0xC50FA0` | Put `3` по `+0x55+2` | DirectInput не крадёт медиа-клавиши |
| 3 | `Games/Skyrim/BSGraphics/BSGraphicsRenderer.cpp` | 77246 | `0xDEFB40` | SwapCall `+9` | Хук StopTimer (таймер/пауза) |
| 4 | `Games/Skyrim/BSSystem/BSTaskletManager.cpp` | 69554 | `0xC71D40` | SwapCall `+0x63` | Именование tasklet-потоков |
| 5 | `Games/Skyrim/Interface/UI.cpp` | 82082 | `0xF35EC0` | SwapCall `+0x682` | UI AddToActiveQueue |
| 6 | `Games/Skyrim/Interface/UI.cpp` | 36548 | `0x5EA350` | Put `0xEB` по `+0xFE` | Пропуск стартового ролика |
| 7 | `Games/Skyrim/Interface/UI.cpp` | 51538 | `0x8BA1C0` | Put `0x9090` по `+0x15` | Избранное во время коннекта (заимствовано из SkyrimSoulsRE!) |
| 8 | `Games/Skyrim/Interface/Menus/SkillsMenu.cpp` | 52510 | `0x8F95C0` | Nop `+0x84E`(6), `+0xA10`(4), `+0x1040`(2) | StatsMenu без паузы (заимствовано из SkyrimSoulsRE!) |
| 9 | `Games/Skyrim/Interface/Menus/SkillsMenu.cpp` | 52518 | `0x8F9720` | Nop `+0x46`(4), `+0x4A`(2) | Управление в StatsMenu (SkyrimSoulsRE) |
| 10 | `Games/Skyrim/Projectiles/Projectile.cpp` | 34452 | `0x56EF30` | Jump `+0x374`, возврат `+0x379`, кадр стека `rsp+0x50`/`0x138` | Защита от nullptr у проджектайлов |

Не живые (действий не требуют, не «оживлять»): `MapMenu.cpp` (всё под `#if 0`),
`BSThread.cpp` havok-патчи и абсолютный адрес `0x1411F0FD4` (под `#if 0`),
vtbl `0x1416244D0` в `TESObjectREFR.cpp` (закомментирован).

## Конфликт с RFAB, замеченный при аудите

Патчи №7–9 заимствованы из **SkyrimSoulsRE**, а в RFAB стоит сам
`SkyrimSoulsRE.dll` (мод «Skyrim Souls»). Оба будут патчить одни инструкции.
В Фазе 5 решить: отключить наши №7–9 при наличии SkyrimSoulsRE **или**
отключить соответствующие опции в ini SkyrimSouls. До решения — не трогаем.

## Как снять данные для проверки (на машине с игрой)

1. `python3 tools/dump_patch_sites.py "<путь>\SkyrimSE.exe" > patch-sites.txt`
2. Если инструмент ругнулся на шифрованный `.text` (Steam CEG): нужен дамп из
   памяти запущенного процесса (x64dbg: Attach к SkyrimSE.exe после загрузки
   главного меню, для каждого RVA из таблицы — `dump` окрестности), либо
   распакованая копия собственного exe.
3. Прислать `patch-sites.txt` в чат. Дальше я дизассемблирую байты (capstone),
   нахожу инструкции-аналоги 1.6.x и вношу оффсеты 1.5.97 отдельным коммитом
   с таблицей «старый оффсет → новый, инструкция до/после».

Критерий готовности пункта: для каждого патча в этой таблице стоит отметка
«подтверждено на 1.5.97: инструкция … по оффсету …» либо патч осознанно
отключён с обоснованием. До этого **первый запуск клиента — только в отладочной
среде с краш-логом NetScriptFramework под рукой**, эти 10 мест — первые
кандидаты при краше на старте.
