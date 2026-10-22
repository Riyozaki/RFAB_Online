# Сборка RFAB Online

## Что где собирается

| Компонент | Платформа | Инструмент |
|---|---|---|
| Клиент (`SkyrimTogether.dll`, лаунчер, CEF UI) | **только Windows** | Visual Studio 2022 (MSVC), xmake 2.9.8, Node LTS + pnpm (UI) |
| Сервер | Windows или Linux | xmake 2.9.8; есть `Dockerfile`/`docker-compose.yml` |
| Тесты сериализации (`Code/tests`, Catch2) | Windows или Linux | xmake |

Таргет игры фиксирован: **Skyrim SE 1.5.97 + SKSE64 2.0.20**. Клиент при
старте проверяет версию и на 1.6.x завершается с понятной ошибкой.

## Windows (клиент + сервер)

1. Поставить: VS2022 с workload «Desktop development with C++», Git,
   [xmake 2.9.8](https://xmake.io), Node.js LTS, pnpm (`corepack enable`).
2. Клонировать с сабмодулями:
   ```
   git clone --recursive <repo-url> RFAB_Online
   ```
3. Собрать (эквивалент `Build.bat`):
   ```
   xmake project -k vsxmake
   xmake config -m releasedbg
   xmake -y
   xmake install -o distrib
   ```
4. UI:
   ```
   cd Code\skyrim_ui
   pnpm install
   pnpm deploy:production
   ```
   результат из `dist\` копируется рядом с бинарями клиента
   (`build\windows\x64\releasedbg`) — см. `Build.bat`.
5. Первый запуск — по чек-листу из `docs/phase-1-report.md`
   (краш-лог NetScriptFramework держать под рукой: RFAB его пишет по адресу
   `Documents\My Games\Skyrim Special Edition\NetScriptFramework\Crash\`).

Возможная первая ошибка компиляции — `static_assert` на размерах структур в
`Code/client/Games/Skyrim/Actor.h` и соседях. Это **ожидаемый сторожевой
механизм** порта на 1.5.97: пришлите текст ассерта в чат, поправлю раскладку.

## Проверки без Windows и без игры

```bash
git fetch origin --depth=300   # нужен для сверки раскладок с pre-AE эталоном
./tools/run_checks.sh
```

Три проверки: хостовые тесты чистой логики (`tools/run_pure_tests.sh`),
сверка настроек с документацией (`tools/check_settings.py`) и сверка всех
`static_assert` раскладок с последним pre-AE коммитом upstream
(`tools/struct_layout_diff.py`). Это единственное, что можно проверить без
MSVC и без запуска игры — всё остальное помечено в
[чек-листе ручного теста](docs/manual-test-checklist.md).

## Linux (только сервер и тесты)

```
xmake config --arch=x64 --mode=debug --yes
xmake -y
```

Или Docker: `docker compose build` (образ сервера из `Dockerfile`).

> Примечание среды разработки агента: в песочнице сеть ограничена, поэтому
> полная Linux-сборка зависимостей xmake там не завершена (детали — в
> `docs/phase-1-report.md`). На обычной машине сеть доступна полностью, но
> **CI сейчас отключён**: воркфлоу лежат в `.github/workflows-upstream-disabled/`.
> Включение Linux-сборки сервера и тестов — первый пункт плана в
> `docs/review-2026-10-22.md` (фаза A).

## Проверка базы адресов (оффлайн, без игры)

```
python3 tools/address_db_check.py "<SkyrimSE>\Data\SKSE\Plugins\version-1-5-97-0.bin" .
```

Ожидается: `missing: 0` по функциональным и RTTI ID.

## Снятие байтов для аудита внутрифункционных патчей

```
python3 tools/dump_patch_sites.py "<SkyrimSE>\SkyrimSE.exe" > patch-sites.txt
```

Подробности: `docs/phase-1-midfunction-audit.md`.
