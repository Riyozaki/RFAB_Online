# Сборка и установка RFAB Online

Документ описывает сборку клиента (Windows), сервера (Windows/Linux) и то, как
поставить сборку в игру. Все команды взяты из `xmake.lua` репозитория и из
CI-воркфлоу `.github/workflows/windows.yml`.

> Статус: **инструкция не проверена запуском в этой среде.** В песочнице нет
> Windows, Visual Studio, xmake и самого Skyrim, поэтому клиент здесь не
> собирался. Серверная часть собирается на Linux. Что именно проверено, а что
> нет — см. `docs/phase1/README_RU.md`.

## 1. Требования

### Клиент (Windows)

| Компонент | Версия | Примечание |
|---|---|---|
| Windows | 10/11 x64 | только 64-битная сборка |
| Visual Studio 2022 | 17.x | workload «Desktop development with C++», MSVC v143 |
| Windows SDK | 10.0.22621 или новее | нужен `dinput.h`, `Commctrl.h` |
| xmake | 2.9.8 (CI) / ≥ 2.8.5 | `xmake --version` |
| Node.js | 20 LTS | только для сборки UI |
| pnpm | 9.x | `npm i -g pnpm@9` |
| Python | 3.10+ | только для генераторов в `tools/port1597` |
| Git | любой | обязательны submodules |

### Сервер (Linux)

| Компонент | Версия |
|---|---|
| g++ / clang++ | C++20 (проверено на g++ 12) |
| xmake | ≥ 2.8.5 |
| CMake | ≥ 3.30 (xmake скачает сам, если нужно) |

## 2. Клонирование

```bash
git clone --recurse-submodules https://github.com/Riyozaki/RFAB_Online.git
cd RFAB_Online
git submodule update --init --recursive
```

Подмодули: `Libraries/TiltedReverse`, `Libraries/TiltedUI`,
`Libraries/TiltedConnect`, `Libraries/TiltedHooks`. Без них сборка не начнётся.

## 3. Клиент (Windows)

> **Целевая версия игры — 1.5.97.** Профиль раскладок выбирается при
> конфигурации и по умолчанию равен `1597`; при необходимости его можно задать
> явно:
>
> ```powershell
> xmake config --skyrim_profile=1597
> ```
>
> Клиент, собранный под один профиль, отказывается работать на другой версии
> игры (диалог «Wrong game version for this build» и `exit(6)`): смещения
> структур вычисляются на этапе компиляции, поэтому «мягкая» деградация здесь
> невозможна. Профиль `skyrim_profile=1600` (1.6.629+) оставлен только для
> сравнения с апстримом — он не поддерживается и не проверялся.

```powershell
xmake config --arch=x64 --mode=releasedbg --yes -vD
xmake -y
xmake install -o distrib

# UI
pnpm --prefix Code/skyrim_ui/ install
pnpm --prefix Code/skyrim_ui/ deploy:production
copy-Item -Recurse Code/skyrim_ui/dist/UI build/windows/x64/releasedbg/
```

Результат появляется в `build/windows/x64/releasedbg/`:

| Файл | Назначение |
|---|---|
| `SkyrimTogether.exe` | лаунчер (`SkyrimImmersiveLauncher`, basename `SkyrimTogether`) |
| `SkyrimTogetherClient.dll` | код клиента, вкомпонован в лаунчер |
| `STServer.exe` / `STServer` | сервер (в Windows-сборке тоже собирается) |
| `UI/` | интерфейс (обязателен рядом с exe) |

Режимы: `--mode=debug|releasedbg|release`. Для отладки используется
`releasedbg` (в `debug` часть кода не рассчитана на работу, см. комментарий в
`Code/client/Utils.h`).

## 4. Сервер (Linux)

```bash
xmake config -p linux --arch=x64 --mode=release --yes -vD
xmake -y SkyrimTogetherServer
```

Бинарник: `build/linux/x64/release/STServer` (basename задан в
`Code/server/xmake.lua`). Запуск:

```bash
./STServer --config-path /path/to/config
```

Docker-образ собирается из `.github/workflows/docker-server-image.yml`.

## 4.1. Логи

Клиент пишет `logs/tp_client.log` (ротация: 5 МБ × 3 файла) и одновременно в
консоль. Уровень задаётся переменной окружения **до** запуска игры:

```powershell
$env:TP_LOG_LEVEL = "debug"   # trace | debug | info | warn | error | critical | off
```

По умолчанию `info`. Записи уровня `warn` и выше сбрасываются на диск сразу,
остальные — раз в секунду; ошибки sink'ов уходят в `OutputDebugString` и не
роняют игру.

## 5. Тесты

```bash
xmake -y TPTests
xmake run TPTests
```

Юнит-тесты лежат в `Code/tests`. Для слоя адресов дополнительно есть
независимый от xmake прогон:

```bash
g++ -std=c++20 -I Code/common tools/port1597/selftest.cpp \
    Code/common/address_library/AddressLibrary.cpp \
    Code/common/address_library/SkyrimIdMap.cpp -o /tmp/selftest
/tmp/selftest --database "/path/to/version-1-5-97-0.bin"
```

Генераторы данных проверяются так:

```bash
python tools/port1597/gen_inl.py --check
python tools/port1597/extract_clibng_pairs.py /path/to/CommonLibSSE-NG --check   # нужен чекаут NG
python tools/port1597/verify_pairs.py --database /path/to/version-1-5-97-0.bin \
        --legacy-tree /path/to/2021-client-tree
```

## 6. Установка в игру (Skyrim SE 1.5.97)

1. **Skyrim SE 1.5.97.0.8** (Steam, откат через depot) + **SKSE64 2.0.20**
   (`skse64_1_5_97.dll`, `skse64_loader.exe`).
2. **Address Library for SKSE Plugins**: файл `version-1-5-97-0.bin` (или
   `versionlib-1-5-97-0.bin`) в `Data\SKSE\Plugins\`. Клиент теперь понимает оба
   имени и все форматы баз (1, 2, 5).
3. Файлы клиента — в корень игры (рядом с `SkyrimSE.exe`), папка `UI` — туда же.
4. RFAB — через MO2, профиль `RFAB`. Клиент проверяет версию игры и версию
   Address Library и отказывается стартовать при расхождении.

### Диагностика

| Сообщение | Причина |
|---|---|
| `Failed to load the Skyrim Address Library` | нет `.bin` в `Data\SKSE\Plugins`, имя не совпало с версией игры или база от другой версии |
| `The 1.5.97 port is incomplete` | не все Address Library ID переведены на 1.5.97; полный список — в `docs/phase1/address_id_inventory.csv` и в логе |
| `Script Extender ... is too old` | SKSE64 ниже 2.0.20 для 1.5.97 (или ниже 2.1.0 для 1.6.x) |

Переменная окружения `TP_ALLOW_UNRESOLVED_ADDRESS_IDS=1` позволяет запуститься
с незакрытыми ID — **только для разработки**, обращение к такому указателю
уронит игру.

## 7. Конфигурация

Сервер читает настройки из файла, путь передаётся ключом `--config-path`
(см. `Code/server/`). Клиент — из конфигурации лаунчера. Отдельный профиль
«RFAB» и правила масштабирования HP/XP появятся в фазах 3–5; до этого менять
дефолты не следует.
