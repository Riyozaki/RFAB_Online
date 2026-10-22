# RFAB reference data (text only)

Производные текстовые данные из сборки RFAB SE XI Prime
(`github.com/drake1ch/RFAB`), извлечённые агентом на ПК владельца и
интегрированные сюда 2026-10-22 из `origin/main` @ `4cba32cd`
(см. `docs/review-agent-main-2026-10-22.md`, находка H3).

## Что здесь лежит

- `rfab_records.tsv` — полный дамп записей `RFAB.esp` (89 802 записи:
  тип, FormID, EditorID). Основа для матрицы совместимости Phase 5.
- `profile/` — шаблонный MO2-профиль RFAB: `loadorder.txt` (24 плагина),
  `modlist.txt` (57 модов), `plugins.txt`, `archives.txt`, `settings.ini`.
- `RFAB.ini`, `DllLoader.ini` — конфиги RFAB и Sumwunn DllLoader.
- `storage-util/` — данные RFAB из SKSE StorageUtil: `BossList.json`,
  `RaceList.json`, `Experience.ini`, `Quests.ini`, `RFAB_MCM_Settings.json`,
  `RFAB_XP_Settings.json` (базовый XP за книги и т.д.).

## Что намеренно НЕ коммитится

Бинарники и чужие исходники **не хранятся в git** (размер + права на
распространение): `RFAB.esp` (31 МБ), `skse64_1_5_97.dll`, `tbb.dll`,
`DINPUT8.dll`, `version-1-5-97-0.bin`, заголовки SKSE64
(`SKSE_GameReferences.h`, `SKSE_GameData.cpp`), листинг файлов
`RFAB_ref_files.txt` и подмодуль на весь репозиторий RFAB (3.1 ГБ).

Локально кладите их в `refdata/` в корне репозитория — каталог внесён в
`.gitignore` (как и `RFAB_extract/`).

## Как получить локально

Источник — `https://github.com/drake1ch/RFAB` (портируемая MO2-сборка):

- `MO2/mods/RFAB/` — сам мод: `RFAB.esp`, `scripts/*.pex` и — важно —
  **исходники скриптов** `scripts/source/*.psc` (в т.ч.
  `RFAB_XP_Handler.psc`, `RFAB_XP_Alias.psc`, `RFAB_LevelUpBonuses.psc`).
- `src/skse64/skse64/` — заголовки SKSE64 2.0.20 (`GameReferences.h`,
  `GameForms.h`, `GameFormComponents.h`, `GameBSExtraData.h`) — эталон
  раскладок структур 1.5.97.
- Address Library SE: файл `version-1-5-97-0.bin` (format 1) — из
  установленного Address Library for SKSE Plugins (SE-вариант) или из
  сборки RFAB; проверка ID: `tools/address_db_check.py`.

Полный клон репозитория RFAB тяжёлый (3.1 ГБ) — используйте точечную
выгрузку через GitHub API (trees/blobs) или sparse checkout нужных путей.
