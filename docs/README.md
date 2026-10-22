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
| [`archive/phase-0-analysis-draft-2026-10-21.md`](archive/phase-0-analysis-draft-2026-10-21.md) | Исходный черновик Фазы 0 с локальной машины (пути `G:\`, версия xmake) | архив |

Инструменты (Python, без сборки):

- `tools/address_db_check.py` — сверка всех `POINTER_SKYRIMSE`/RTTI ID из кода с
  `version-1-5-97-0.bin`;
- `tools/dump_patch_sites.py` — дамп байтов вокруг внутрифункционных патчей из
  вашего `SkyrimSE.exe` (нужен для пересчёта оффсетов).

Данные-фикстуры и сторонние бинарники — см. [`../RFAB_extract/README.md`](../RFAB_extract/README.md).
