# RFAB Online

Мультиплеерная платформа для модпака **[Skyrim Requiem for a Balance (RFAB)](https://github.com/drake1ch/RFAB)** на базе форка [`rfortier/TiltedEvolution-rwf`](https://github.com/rfortier/TiltedEvolution-rwf), адаптируемая строго под **Skyrim Special Edition `1.5.97.0.8`** (`SKSE64 2.0.20`, `version-1-5-97-0.bin`).

---

## Ключевые проектные принципы

- **Строгий таргет `Skyrim SE 1.5.97.0.8`:** полная бинарная совместимость с `SkyrimSE.exe 1.5.97.0.8`, `skse64_1_5_97.dll` (`2.0.20`), `version-1-5-97-0.bin` (формат `1`) и стеком SKSE-плагинов `RFAB` (`EngineFixes.dll`, `SSEDisplayTweaks.dll`, `SkyrimSoulsRE.dll`, `OAR`, `AMR`, `RFAB.dll`). Версии `1.6.x` (AE) намеренно не поддерживаются.
- **Равное распределение опыта RFAB:** все игроки на сервере получают **равное количество опыта (`1×`)** за убийства врагов/боссов, завершение квестов, книги опыта и особые награды с серверной дедупликацией и полноценным повышением уровня (`PlayerCharacter::pSkills->data->xp` + `RFAB_XP_Amount`).
- **Честный баланс врагов без размножения спавнов:** количество врагов **никогда не умножается** на число игроков (`1×` врагов); вместо этого масштабируется только HP враждебных NPC (`1` игрок = `1× HP`, `2` игрока = `3× HP`, `3` игрока = `4× HP`, $N \ge 2 \implies (N + 1)\times\text{HP}$).

---

## Документация и инженерные артефакты

- [`docs/ARCHITECTURE_REVIEW_AND_ROADMAP.md`](./docs/ARCHITECTURE_REVIEW_AND_ROADMAP.md) — **Главный отчёт ревью и дорожная карта**: аудит всех веток репозитория (`main`, `014f2836`, `fcbffe0c`, `ada174eb`), разбор найденных багов и поэтапный план реализации.
- [`docs/SE_1597_PORT_SPEC.md`](./docs/SE_1597_PORT_SPEC.md) — **Низкоуровневая спецификация порта на `1.5.97.0.8`**: все `16 656`-типные проверки `sizeof`/`offsetof` C++-структур, все `11` внутрифункциональных патчей `VersionDbPtr` и правила совместимости с SKSE-плагинами `RFAB`.
- [`docs/se-1597-id-map.csv`](./docs/se-1597-id-map.csv) — **Полная бинарно верифицированная таблица `3 083 / 3 083` (`100%`) Address Library ID** (`AE 1.6.x -> SE 1.5.97.0.8` + точные `RVA` из `version-1-5-97-0.bin`).
- [`docs/RFAB_ADAPTATION_SPEC.md`](./docs/RFAB_ADAPTATION_SPEC.md) — **Спецификация интеграции с `drake1ch/RFAB`**: точные FormID из `RFAB.esp`, механика `RFAB_XP_Alias` / `RFAB_XP_Handler` / `RFAB_PapyrusFunctions`, формулы безопасного масштабирования HP, `REQ_DeathHandler`, засады, счётчики и боссы.
- [`docs/rfab/`](./docs/rfab/) — извлечённая текстовая база записей `RFAB.esp` (`rfab_records.tsv`) и эталонные таблицы баланса `StorageUtilData/` (`Experience.ini`, `BossList.json`, `RaceList.json`, `Quests.ini`).
- [`Tools/rfab/`](./Tools/rfab/) — офлайн-скрипты верификации порта (`verify_se1597_port.py`, `parse_rfab_esp.py`).
- [`BUILD.md`](./BUILD.md) — инструкция по локальной сборке клиента и сервера через `xmake` / Visual Studio 2022 + `pnpm`.
