# RFAB Online

Мультиплеер для сборки **RFAB SE XI Prime** на базе TiltedEvolution
(Skyrim Together Reborn), портированный на **Skyrim Special Edition 1.5.97**.

> Статус: **в разработке, порт на 1.5.97 сделан наполовину**. Код порта есть,
> но ни одной сборки клиента и ни одного игрового теста ещё не было; 10
> внутрифункционных патчей всё ещё от 1.6.x. Полная картина «что работает, что
> нет и что делать дальше» — в **[ревью от 2026-10-22](docs/review-2026-10-22.md)**.
> Anniversary Edition (1.6.x) **не поддерживается**.

## Требования

- Skyrim Special Edition **1.5.97** (любой дистрибутив: Steam, даунгрейд,
  DRM-free — версия определяется по самому `SkyrimSE.exe`)
- SKSE64 **2.0.20** (`skse64_1_5_97.dll`)
- Address Library for SKSE Plugins, **SE-вариант** (`version-1-5-97-0.bin`)
- Сборка RFAB (Mod Organizer 2, профиль как у всех участников сессии)

## Документация

- **[Ревью: состояние, риски и план работ](docs/review-2026-10-22.md)** — читать первым
- [Индекс всей документации](docs/README.md)
- [Фаза 0 — анализ и план](docs/phase-0-analysis.md)
- [Фаза 1 — отчёт о порте на 1.5.97](docs/phase-1-report.md)
- [Аудит внутрифункционных патчей](docs/phase-1-midfunction-audit.md)
- [Сборка из исходников](BUILD.md)
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
