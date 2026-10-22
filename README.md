# RFAB Online

Мультиплеер для сборки **RFAB SE XI Prime** на базе TiltedEvolution
(Skyrim Together Reborn), портированный на **Skyrim Special Edition 1.5.97**.

> Статус: **порт на 1.5.97 и мультиплеерные системы реализованы, но не
> проверены в игре** — в среде разработки нет Windows-сборки. Раскладки структур
> сверены с историей upstream (127/127), чистая логика покрыта хостовыми тестами
> (135 проверок). Что именно проверено и чем — `CHANGELOG.md` и
> **[чек-лист ручного теста](docs/manual-test-checklist.md)**; общая картина —
> **[ревью от 2026-10-22](docs/review-2026-10-22.md)**.
> Anniversary Edition (1.6.x) **не поддерживается**.

## Требования

- Skyrim Special Edition **1.5.97** (любой дистрибутив: Steam, даунгрейд,
  DRM-free — версия определяется по самому `SkyrimSE.exe`)
- SKSE64 **2.0.20** (`skse64_1_5_97.dll`)
- Address Library for SKSE Plugins, **SE-вариант** (`version-1-5-97-0.bin`)
- Сборка RFAB (Mod Organizer 2, профиль как у всех участников сессии)

Итоги последней сессии, проверки и чек-лист ручного теста:
[`docs/final-report-2026-10-22.md`](docs/final-report-2026-10-22.md).

## Документация

- **[Ревью: состояние, риски и план работ](docs/review-2026-10-22.md)** — читать первым
- [Индекс всей документации](docs/README.md)
- [Фаза 0 — анализ и план](docs/phase-0-analysis.md)
- [Фаза 1 — отчёт о порте на 1.5.97](docs/phase-1-report.md)
- [Аудит внутрифункционных патчей](docs/phase-1-midfunction-audit.md)
- [Сборка из исходников](BUILD.md)
- [Конфигурация сервера и клиента](docs/configuration.md)
- [Совместимость с RFAB](docs/RFAB-compatibility.md)
- [Чек-лист ручного теста (1–4 игрока)](docs/manual-test-checklist.md)
- [Опыт и RFAB](docs/rfab-xp.md) · [Сохранения в мультиплеере](docs/multiplayer-saves.md)
- [Оригинальный README Tilted Online](README.upstream.md)
- [Анимационные моды (форк rwf)](README-ANIMATION-MODS.md)
- [Reference-данные сборки RFAB](RFAB_extract/README.md) — что хранится в git, а что нет

## Происхождение и лицензия

Форк цепочки: TiltedPhoques/TiltedEvolution → rfortier/TiltedEvolution-rwf
(ветка `features-integration`, коммит `a7d06156`) → RFAB Online.
Лицензия — **GPLv3**, как у оригинала (см. `LICENSE`).

Сторонние файлы сборки RFAB (`RFAB.esp`, DLL, база Address Library) в
репозитории **не хранятся**: они нужны только локально для оффлайн-аудита,
см. [`RFAB_extract/README.md`](RFAB_extract/README.md).
