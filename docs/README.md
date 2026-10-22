# Документация RFAB Online

Порядок чтения: сначала `review-2026-10-22.md` (честное состояние проекта на
сегодня), потом фазы.

| Документ | О чём | Статус |
|---|---|---|
| [`review-2026-10-22.md`](review-2026-10-22.md) | Полное ревью репозитория и кода: что хорошо, что сломано, план работ, матрица требований | **актуально** |
| [`phase-0-analysis.md`](phase-0-analysis.md) | Разведка: устройство TiltedEvolution-rwf, что завязано на версию игры, состав сборки RFAB, риски | актуально (с errata в `phase-1-report.md`) |
| [`phase-0-evidence/address-id-audit.txt`](phase-0-evidence/address-id-audit.txt) | Лог сверки Address Library ID с базой 1.5.97 | актуально, воспроизводимо |
| [`phase-1-report.md`](phase-1-report.md) | Что сделано в порте на 1.5.97, что проверено, что нет | актуально, но **отстаёт от кода** (см. ревью, §4.1) |
| [`phase-1-midfunction-audit.md`](phase-1-midfunction-audit.md) | Список 10 внутрифункционных патчей, которые надо пересчитать под 1.5.97 | актуально, работа не сделана |
| [`multiplayer-saves.md`](multiplayer-saves.md) | Как устроены сохранения в мультиплеере: правила, индекс сейвов, что сознательно не делается | **актуально** |
| [`rfab-xp.md`](rfab-xp.md) | Опыт: пайплайн `RFAB_XP_Handler.ModXP`, что делает сервер, дедупликация, ограничения | **актуально** |
| [`configuration.md`](configuration.md) | Все серверные настройки и клиентский `RFABOnline.ini` с значениями по умолчанию | **актуально** |
| [`RFAB-compatibility.md`](RFAB-compatibility.md) | Матрица совместимости: версии, какие системы RFAB затронуты, риски | **актуально** |
| [`manual-test-checklist.md`](manual-test-checklist.md) | Чек-лист ручного теста в игре для 1/2/3/4 игроков, плохой сети и сохранений | **обязателен перед релизом** |
| [`archive/phase-0-analysis-draft-2026-10-21.md`](archive/phase-0-analysis-draft-2026-10-21.md) | Исходный черновик Фазы 0 с локальной машины (пути `G:\`, версия xmake) | архив |

Инструменты (Python, без сборки):

- `tools/address_db_check.py` — сверка всех `POINTER_SKYRIMSE`/RTTI ID из кода с
  `version-1-5-97-0.bin`;
- `tools/dump_patch_sites.py` — дамп байтов вокруг внутрифункционных патчей из
  вашего `SkyrimSE.exe` (нужен для пересчёта оффсетов);
- `tools/struct_layout_diff.py` — сверка всех `static_assert(offsetof/sizeof)`
  с последним pre-AE коммитом upstream (`5c5837f6`), ловит AE-раскладки,
  просочившиеся в порт;
- `tools/run_pure_tests.sh` — сборка и запуск хостовых тестов чистой логики
  (`Code/tests/pure/`), без Windows и без игры;
- `tools/check_settings.py` — сверка настроек сервера в коде с
  `docs/configuration.md` в обе стороны (ловит и недокументированные ключи, и
  устаревшую документацию);
- `tools/run_checks.sh` — все проверки, возможные без Windows и игры (тесты +
  настройки + раскладки структур); то же самое выполняет
  тот же набор готов как workflow в `docs/ci/checks.yml` (его нужно один раз
  скопировать в `.github/workflows/` — см. инструкцию в самом файле).

Данные-фикстуры и сторонние бинарники — см. [`../RFAB_extract/README.md`](../RFAB_extract/README.md).
