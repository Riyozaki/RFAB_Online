# RFAB Online

Мультиплеер для сборки **RFAB SE XI Prime** на базе TiltedEvolution
(Skyrim Together Reborn), портированный на **Skyrim Special Edition 1.5.97**.

> Статус: **в разработке (Фаза 1 — порт на 1.5.97)**. Игровых тестов ещё не
> было; что проверено, а что нет — см. отчёты по фазам в `docs/`.
> Anniversary Edition (1.6.x) **не поддерживается**.

## Требования

- Skyrim Special Edition **1.5.97** (любой дистрибутив: Steam, даунгрейд,
  DRM-free — версия определяется по самому `SkyrimSE.exe`)
- SKSE64 **2.0.20** (`skse64_1_5_97.dll`)
- Address Library for SKSE Plugins, **SE-вариант** (`version-1-5-97-0.bin`)
- Сборка RFAB (Mod Organizer 2, профиль как у всех участников сессии)

## Документация

- [Фаза 0 — анализ и план](docs/phase-0-analysis.md)
- [Фаза 1 — отчёт о порте на 1.5.97](docs/phase-1-report.md)
- [Аудит внутрифункционных патчей](docs/phase-1-midfunction-audit.md)
- [Сборка из исходников](BUILD.md)
- [Оригинальный README Tilted Online](README.upstream.md)
- [Анимационные моды (форк rwf)](README-ANIMATION-MODS.md)

## Происхождение и лицензия

Форк цепочки: TiltedPhoques/TiltedEvolution → rfortier/TiltedEvolution-rwf
(ветка `features-integration`, коммит `a7d06156`) → RFAB Online.
Лицензия — **GPLv3**, как у оригинала (см. `LICENSE`).
