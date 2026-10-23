# RFAB Online — отчёт о сборке, тестировании и упаковке

Дата: 2026-10-22  
Рабочая папка: `G:\RFAB Together\repo`  
Ветка: `arena/4f97534a-rfab-online`  
Коммит: `e2df7e1fde6b578e1410585324ba9f3026b0b341`  
Режим сборки: `releasedbg` (Windows x64, MSVC)

## 1. Итог

| Пункт | Результат |
|---|---|
| C++ сборка (xmake) | Успешно, `build ok, spent 70.015s` (инкрементальная финальная сборка), лог `04_xmake_build.log` |
| Установка в `distrib` | Успешно, `install ok!`, лог `04b_xmake_install.log` |
| TPTests | 17/17 кейсов, 2259 assertions, exit 0 |
| `verify_se1597_port.py --strict-migrated` | exit 0, 3096/3096 call-sites, 0 unmigrated, 9/9 struct OK |
| Console_Tests | 6/6 passed, exit 0 |
| Resources_Tests | 13/13 passed, exit 0 |
| UI (Angular) | Production-сборка успешна, `Code/skyrim_ui/dist/UI` |
| Упаковка | `dist_ready/`: MO2-архив, сервер, патчер, PDB; smoke-test сервера пройден |
| SHA256 MO2-архива | `DC68C130D2EE25D7C4D454B08BAAE35038D8F4A33895BDF37FC5BC2EE733FC35` |

Все изменения, необходимые для сборки, перечислены в разделе 4. Изменения в исходниках репозитория — 5 файлов, `14 insertions(+), 4 deletions(-)`, см. `local_changes.patch`.

## 2. Исходные данные

### 2.1. Репозиторий и сабмодули

Сабмодули зафиксированы на требуемых коммитах (проверено `git -C Libraries/<name> rev-parse HEAD`):

| Сабмодуль | Коммит |
|---|---|
| `Libraries/TiltedConnect` | `293fab99288b7414bd129ace25ae018114cefb2a` |
| `Libraries/TiltedHooks` | `d7c175b86975679eb05883c097d7eb1c2165226a` |
| `Libraries/TiltedReverse` | `55ee3f29c62fea247148e57b94ec4941055139ed` |
| `Libraries/TiltedUI` | `a2b9a671626d6489e6e2d9ee02adf61d7d9673dd` |

### 2.2. Инструменты

| Инструмент | Версия |
|---|---|
| ОС | Windows 10 Pro 10.0.19045 x64 |
| CPU / RAM | AMD Ryzen 7 1700 / 31.9 GB |
| git | 2.53.0.windows.1 |
| node | v24.13.0 |
| npm | 11.6.2 |
| pnpm (системный) | 11.22.0 |
| corepack | 0.34.5 |
| Python | 3.14.3 |
| xmake | v3.1.1+master.a97d1638a |
| Visual Studio | Build Tools 2022 17.14.36915.13, MSVC 19.44.35222, linker 14.44.35222.0 |
| Windows SDK | `C:\Program Files (x86)\Windows Kits\10` |
| vcvars64 | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat` |

Полный аудит окружения: `01_env.log`.

## 3. Сборка

### 3.1. C++ (xmake)

Команды выполнялись из `G:\RFAB Together\repo` после `vcvars64.bat`:

```bat
xmake config -p windows -a x64 -m releasedbg -y
xmake -y -v
xmake install -y -o distrib
```

Результат:

- `03_xmake_config.log` — конфигурация и установка пакетов, включая CEF `141.0.11` (пакет `8daea7b1a9b7420994d1aba2609fc469`);
- `04_xmake_build.log` — успешная финальная сборка, все цели слинкованы, в конце `build ok`;
- `04b_xmake_install.log` — `install ok!`, артефакты скопированы в `distrib/bin` и `distrib/Data`;
- неудачные попытки сохранены отдельно: `04_xmake_build_attempt1_failed.log` … `04_xmake_build_attempt4_failed.log`.

Собранные цели: `SkyrimTogether.exe` (immersive launcher), `SkyrimTogetherClient.lib`, `TPProcess.exe`, `SkyrimTogetherServer.exe`, `STServer.dll`, `EarlyLoad.dll`, `TPTests.exe`, `Console_Tests.exe`, `Resources_Tests.exe`, `ESLoader_Tests.exe`, `TiltedReverse_Tests.exe` и вспомогательные библиотеки.

### 3.2. UI

Системный pnpm 11.22.0 несовместим с `lockfileVersion: 6.0` из репозитория (`ERR_PNPM_LOCKFILE_BREAKING_CHANGE`). Использован pnpm 8.15.9 через corepack:

```powershell
$env:NODE_OPTIONS = "--openssl-legacy-provider"
corepack pnpm@8.15.9 install --frozen-lockfile
corepack pnpm@8.15.9 run deploy:production
```

Результат: `Code/skyrim_ui/dist/UI` содержит `index.html`, `main.js`, `polyfills.js`, `runtime.js`, `styles.css`, `assets/`, `favicon.ico`, `3rdpartylicenses.txt`. Лог: `06_ui_build.log` (в нём же зафиксирована первоначальная ошибка pnpm 11).

UI упакован в `RFAB_Online_MO2/SkyrimTogetherReborn/UI`, поскольку клиент загружает `UI/index.html` и курсоры из `assets/images/cursor.*` относительно каталога исполняемого файла.

## 4. Проблемы сборки и внесённые исправления

| № | Симптом | Причина | Исправление | Файл |
|---|---|---|---|---|
| 1 | `rfab_stage3_4.cpp`: не найдены типы TiltedCore | Тест использовал `TiltedCore` без явных include | Добавлены `TiltedCore/Stl.hpp`, `TiltedCore/Buffer.hpp`, `TiltedCore/Serialization.hpp` | `Code/tests/rfab_stage3_4.cpp` |
| 2 | `Vector3_NetQuantize.h`, `Rotator2_NetQuantize.h`: неизвестен `glm::vec2/vec3` | Заголовки наследуют glm-типы без include | Добавлен `#include <glm/glm.hpp>` | `Code/encoding/Structs/Vector3_NetQuantize.h`, `Code/encoding/Structs/Rotator2_NetQuantize.h` |
| 3 | `LNK2038: mismatch detected for 'RuntimeLibrary': value 'MT_StaticRelease' doesn't match 'MD_DynamicRelease'` при линковке `TPProcess` | Пакет CEF в xmake-repo по умолчанию собирался с `/MT`, остальной проект — с `/MD` | В `packages/c/cef/xmake.lua` изменён default `vs_runtime` с `MT` на `MD`; пакет переустановлен, новый хеш `8daea7b1a9b7420994d1aba2609fc469` (старый `89899e7225a7462bb6e7fc9925331578` больше не используется) | вне репозитория: `C:\Users\Arbuz\AppData\Local\.xmake\repositories\xmake-repo\packages\c\cef\xmake.lua` |
| 4 | `TPTests`: `Server.hpp` → `steam/steamnetworkingsockets.h` не найден | В `TPTests` не были подключены зависимости серверного кода | В `add_packages` добавлены `gamenetworkingsockets`, `libuv` | `Code/tests/xmake.lua` |
| 5 | `error C2146: missing '>' before identifier __VA_OPT__`, warning C5109 | MSVC с традиционным препроцессором не раскрывает `__VA_OPT__` (проект не использует `/Zc:preprocessor`) | В трёх макросах `scope __VA_OPT__(,) __VA_ARGS__` / `returnType __VA_OPT__(,) __VA_ARGS__` заменены на `scope, ##__VA_ARGS__` / `returnType, ##__VA_ARGS__` | `Code/client/Services/PapyrusService.h` |
| 6 | `ERR_PNPM_LOCKFILE_BREAKING_CHANGE` при `pnpm install` | pnpm 11 не читает lockfile v6 | Сборка UI через `corepack pnpm@8.15.9` + `NODE_OPTIONS=--openssl-legacy-provider` | `Code/skyrim_ui` |

Примечание: после правки пакета CEF xmake всё ещё печатает предупреждение `configs.runtimes is readonly in package(cef), it's always MT`; это предупреждение относится к пользовательской конфигурации пакета и не отменяет default `vs_runtime = MD`. Сборка и линковка проходят, что подтверждается логом и использованием пути пакета `8daea7b1...`.

## 5. Тесты

### 5.1. Обязательные проверки

| Проверка | Команда | Результат |
|---|---|---|
| TPTests (Catch2) | `build\windows\x64\releasedbg\TPTests.exe -r compact` | `Passed all 17 test cases with 2259 assertions`, exit 0 |
| SE 1.5.97 port verifier | `python Tools\rfab\verify_se1597_port.py --strict-migrated` | 3084 AE→SE записи; 3096 активных call-sites (3085 уникальных ID); 3096 валидных SE target ID; 0 unmigrated; 9/9 struct checks OK; exit 0 |
| Console_Tests | `Console_Tests.exe` | 6/6 passed, exit 0 |
| Resources_Tests | `Resources_Tests.exe` | 13/13 passed, exit 0 |

Список 17 кейсов TPTests: `05_tests_list.log`; полный вывод обязательных проверок: `05_tests.log`.

Замечание по количеству assertions: в коммите `e2df7e1` TPTests содержит 17 кейсов и 2259 проверок — на одну меньше ориентира ≥2260 из постановки. Тестовые файлы не изменялись (добавлены только include в `rfab_stage3_4.cpp`), поэтому 2259 — фактическое состояние тестового набора в этом коммите. README стадии 4 с 15 кейсами/2208 проверками устарел относительно текущего кода.

### 5.2. Тесты, зависящие от игрового окружения

| Тест | Результат | Причина |
|---|---|---|
| `ESLoader_Tests.exe` | 8 failed, exit 1 | Тесту нужны реальный `Skyrim Data\` и `loadorder.txt` рядом с бинарником; без них `BuildRecordCollection` возвращает false, остальные кейсы падают с SEH 0xC0000005 |
| `TiltedReverse_Tests.exe` | 1 failed (SIGSEGV), exit `-1073741819` | Тест загружает `DLL.dll` и инжектит его в фиктивный процесс; без подготовленного `DLL.dll` и окружения инъекции падает |

Эти тесты не входят в проверку переноса на SE 1.5.97 и не блокируют выпуск: падения вызваны отсутствием локальной установки Skyrim/подготовленных бинарников, а не изменениями RFAB. Полный вывод: `05_tests_env_dependent.log`.

## 6. Артефакты `dist_ready`

```
dist_ready/
├── RFAB_Online_MO2.zip          171 283 321 байт  (SHA256 DC68C130...733FC35)
├── RFAB_Online_MO2/             387 702 744 байт, 498 файлов
├── RFAB_Online_Server/            7 688 064 байт, 7 файлов
├── RFAB_Patcher/                      9 260 байт, 2 файла
└── DebugSymbols_PDB/            136 269 824 байт, 5 файлов
```

### 6.1. `RFAB_Online_MO2`

Корень мода (то, что распаковывается в MO2):

```
SkyrimTogether.esp
SKSE/Plugins/RFAB_Online_Bindings.json
SkyrimTogetherReborn/
    SkyrimTogether.exe
    TPProcess.exe
    EarlyLoad.dll
    CEF runtime: libcef.dll, chrome_elf.dll, chrome_100_percent.pak,
                 chrome_200_percent.pak, resources.pak, icudtl.dat,
                 v8_context_snapshot.bin, d3dcompiler_47.dll, dxcompiler.dll,
                 dxil.dll, libEGL.dll, libGLESv2.dll, vk_swiftshader.dll,
                 vulkan-1.dll, locales/, swiftshader/
    discord_game_sdk.dll
    UI/ (index.html, main.js, polyfills.js, runtime.js, styles.css, assets/)
meshes/
scripts/
SkyrimTogetherRebornBehaviors/
```

Проверки упаковки:

- `*.lib`, `*.exp`, `*Tests.exe` в MO2-папке отсутствуют (проверено рекурсивно);
- PDB в MO2-папку не входят;
- UI лежит рядом с `SkyrimTogether.exe`, как ожидает `OverlayApp`;
- серверные бинарники (`SkyrimTogetherServer.exe`, `STServer.dll`) в MO2-папку не входят: они поставляются только в `RFAB_Online_Server`, чтобы клиентский архив оставался чистым.

Размер в распакованном виде — 387.7 МБ, в архиве — 171.3 МБ; основной объём даёт CEF (`libcef.dll` и ресурсы).

### 6.2. `RFAB_Online_Server`

```
SkyrimTogetherServer.exe   594 432 байт   SHA256 BCCA66A989C9B6F7338A90448D3C58BB088D5832D71B05F3806F37E469170B9B
STServer.dll             7 091 712 байт   SHA256 ECD8E3677FA7F51BA948BE350F11772532CA1AF11D19CCDA12C3E2C46D4A3FD6
config/STServer.ini
Data/README.txt
logs/smoke_stdout.log
logs/smoke_stderr.log
logs/STServerOut.log
```

Smoke-test: сервер запущен с `TILTED_ACCEPT_EULA=true`, проработал 5 секунд, не завершился аварийно и был остановлен принудительно. Вывод:

```
[info] Server none started on port 10578
[warning] bAnnounceServer is set to false...
[warning] Failed to open loadorder.txt
[info] Loaded 0 resources
> Server started, type /help for a list of commands.
```

`Failed to open loadorder.txt` ожидаемо для чистого пакета: файл загрузки должен положить пользователь. `stderr` пуст. Конфиг `STServer.ini` создан автоматически; при первом запуске он содержит только настройки, зарегистрированные до первого сохранения (41 строка), остальные значения используются по умолчанию и будут дописаны при последующих запусках/сохранении.

### 6.3. `RFAB_Patcher`

- `patch_rfab_modpack_compat.py` — проверка/патч MO2-модпака или `Data` под RFAB Online;
- `RFAB_Online_Bindings.json` — манифест привязок, используемый патчером.

Проверен только запуск `--help`; применение к реальному модпаку в этом окружении не выполнялось (нет каталога модпака). Использование:

```powershell
python patch_rfab_modpack_compat.py --root "путь\к\MO2\mods" --bindings RFAB_Online_Bindings.json
python patch_rfab_modpack_compat.py --root "путь\к\MO2\mods" --bindings RFAB_Online_Bindings.json --apply
```

Первый запуск — dry-run, второй — запись изменений.

### 6.4. `DebugSymbols_PDB`

`SkyrimTogether.pdb`, `TPProcess.pdb`, `STServer.pdb`, `SkyrimTogetherServer.pdb`, `EarlyLoad.pdb` — для анализа крашей и отладки релизной сборки.

## 7. Изменения в репозитории

`git diff --stat`:

```
 Code/client/Services/PapyrusService.h        | 6 +++---
 Code/encoding/Structs/Rotator2_NetQuantize.h | 2 ++
 Code/encoding/Structs/Vector3_NetQuantize.h  | 2 ++
 Code/tests/rfab_stage3_4.cpp                 | 4 ++++
 Code/tests/xmake.lua                         | 4 +++-
 5 files changed, 14 insertions(+), 4 deletions(-)
```

Полный патч: `local_changes.patch`.

Изменения вне репозитория (обязательны для повторяемой сборки):

- `C:\Users\Arbuz\AppData\Local\.xmake\repositories\xmake-repo\packages\c\cef\xmake.lua`, строка 41: `add_configs("vs_runtime", {..., default = "MD", ...})`.
- После изменения пакет CEF нужно переустановить (`xmake require --force cef` или удалить каталог `packages/c/cef/141.0.11/89899e...` и выполнить `xmake config`), затем переконфигурировать проект.
- Обновление xmake-repo может перезаписать этот файл; патч нужно применить повторно.

## 8. Ограничения и что не проверено

1. Клиент не запускался в реальной Skyrim SE 1.5.97: в окружении нет игры, SKSE64 и модов. Проверены сборка, линковка, структура поставки и наличие UI рядом с exe; игровой запуск, подключение к серверу и синхронизация требуют теста на игровой машине.
2. `ESLoader_Tests` и `TiltedReverse_Tests` не проходят без игровых данных/подготовленной DLL — это ограничение окружения, а не регрессия.
3. Патчер модпака не применялся к реальному модпаку; проверена только его CLI-оболочка.
4. Сервер протестирован только на старт: загрузка `loadorder.txt`, мир, ресурсы и сетевые сессии не проверялись.
5. `STServer.ini` после первого запуска неполный (41 строка) — известное поведение инициализации; настройки по умолчанию применяются, полный файл формируется при последующих сохранениях.
6. Антивирус/SmartScreen может реагировать на неподписанные exe и на инъекцию в процесс — это ожидаемо для Skyrim Together.

## 9. Установка

### 9.1. Клиент (MO2)

1. Установить `RFAB_Online_MO2.zip` как мод в MO2 (или распаковать `RFAB_Online_MO2` в `mods`).
2. Включить мод; убедиться, что `SkyrimTogether.esp` загружается после мастер-файлов.
3. Установить SKSE64 для Skyrim SE 1.5.97 (2.0.20) и Address Library для SE.
4. В MO2 добавить исполняемый файл `...\RFAB_Online_MO2\SkyrimTogetherReborn\SkyrimTogether.exe`, рабочую папку — тот же `SkyrimTogetherReborn`.
5. Запускать клиент через MO2, чтобы VFS видел `Data`-файлы мода.

### 9.2. Сервер

1. Распаковать `RFAB_Online_Server` в отдельный каталог.
2. Положить `loadorder.txt` (тот же порядок загрузки, что у клиентов) в `Data/`; при необходимости — игровые плагины/ресурсы.
3. Запустить `SkyrimTogetherServer.exe`; принять EULA в окне или задать `TILTED_ACCEPT_EULA=true` для headless-запуска.
4. Открыть порт `10578` (TCP/UDP) либо изменить порт в `config/STServer.ini`.
5. Для публичного сервера включить `bAnnounceServer=true` в `config/STServer.ini`.

### 9.3. Патчер

См. раздел 6.3; сначала dry-run без `--apply`, затем применение.

## 10. Индекс логов и файлов

| Файл | Содержимое |
|---|---|
| `01_env.log` | Аудит ОС, инструментов, MSVC, SDK |
| `02_submodules.log` | Клонирование и фиксация сабмодулей |
| `03_xmake_config.log` | Конфигурация xmake, установка пакетов (CEF и др.) |
| `04_xmake_build_attempt1_failed.log` … `attempt4_failed.log` | Неудачные попытки сборки и их причины |
| `04_xmake_build.log` | Успешная финальная сборка |
| `04b_xmake_install.log` | `xmake install -o distrib` |
| `05_tests.log` | TPTests, verify-скрипт, Console_Tests, Resources_Tests |
| `05_tests_list.log` | Список 17 кейсов TPTests |
| `05_tests_env_dependent.log` | ESLoader_Tests, TiltedReverse_Tests и пояснение |
| `06_ui_build.log` | pnpm 11 (ошибка), pnpm 8 install, production-сборка UI |
| `07_packaging.log` | Упаковка, проверки, smoke-test, ZIP |
| `local_changes.patch` | Патч исходников репозитория |
| `FINAL_REPORT.md` | Этот отчёт |

## Приложение. Команды воспроизведения

```powershell
# 1. Патч CEF (вне репозитория): default vs_runtime = MD, затем переустановка пакета.
# 2. Конфигурация и сборка
cmd /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" >nul && xmake config -p windows -a x64 -m releasedbg -y"
cmd /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" >nul && xmake -y -v"
cmd /c "call ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" >nul && xmake install -y -o distrib"

# 3. Тесты
.\build\windows\x64\releasedbg\TPTests.exe -r compact
python Tools\rfab\verify_se1597_port.py --strict-migrated
.\build\windows\x64\releasedbg\Console_Tests.exe
.\build\windows\x64\releasedbg\Resources_Tests.exe

# 4. UI
$env:NODE_OPTIONS = "--openssl-legacy-provider"
corepack pnpm@8.15.9 install --frozen-lockfile
corepack pnpm@8.15.9 run deploy:production
```
