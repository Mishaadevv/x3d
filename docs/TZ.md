# ZeqouX3D — Техническое задание (master spec)

> Главный документ проекта. Перед любой работой над движком читать соответствующий раздел.
> Прогресс отмечается чекбоксами в разделе 14. Решения, принятые по ходу, — в разделе 15 (ADR-лог).
> Версия ТЗ: 1.0 · дата: 2026-10-11

---

## 0. Суть

**ZeqouX3D** — 3D-движок общего назначения, написанный с нуля, с редактором уровня Unity/Unreal:
физически корректный рендер (PBR), редактор сцен, ассет-пайплайн, физика, анимация, аудио, UI,
скриптинг, упаковка игры в standalone-билд.

### 0.1 Цели
1. Собственный код всех подсистем (рендер, физика, математика, UI, загрузчики форматов, сжатие, шрифты).
2. Редактор: вьюпорт, иерархия, инспектор, контент-браузер, гизмо, undo/redo, Play-in-Editor.
3. Масштабируемый рендер: работает на встройке (Intel UHD 620) и раскрывается на дискретных GPU.
4. Data-driven: всё, что редактируется, сериализуется, рефлексируется и отображается в инспекторе автоматически.
5. Стабильный и тестируемый код: unit-тесты Core/Math/Physics, golden-image тесты рендера.

### 0.2 Не-цели (на первом этапе)
- Консоли, мобильные, macOS/Linux (архитектура не должна это запрещать — платформа за абстракцией).
- Сетевая игра (запланирована на позднюю фазу M19).
- Полноценный паритет с UE 5.x во всём (Nanite + Lumen + MetaHuman + Chaos и т.д. — это сотни человеко-лет).
  Ориентир: **архитектура и пользовательский опыт уровня Unity URP**, а «фичи UE5» (виртуальная геометрия,
  динамический GI, виртуальные тени) — как поздние фазы в «lite»-варианте под доступное железо.

### 0.3 Честная оценка
Это очень большой проект. Делаем вертикальными срезами: каждая веха (M0…M20) заканчивается чем-то, что
**запускается и проверяется**. Никаких «скелетов на будущее» без работающего кода. Сначала простое и
корректное, потом быстрое и красивое.

---

## 1. Целевое железо и платформа

### 1.1 Машина разработки (она же минимальная цель, «Tier Low»)
| Параметр | Значение |
|---|---|
| CPU | Intel Core i5-10310U (4C/8T, 1.7 ГГц base) |
| RAM | 8 ГБ |
| GPU | Intel UHD Graphics 620 (Gen9.5, встройка, общая память) |
| ОС | Windows 11 Pro 26200 |

Последствия для архитектуры:
- **Нет mesh shaders, нет аппаратного RT, слабый fill-rate.** Всё «некстген» — через compute + ExecuteIndirect,
  с фолбэками. Возможности GPU определяются в рантайме (раздел 6.3.2).
- Рендер-разрешение по умолчанию ниже выходного + апскейл (раздел 6.4.12). Целевой FPS на UHD 620:
  Sponza-класс сцена, 1600×900 вывод, ~60 % внутреннего разрешения, **30+ FPS**, редактор — **60 FPS** на пустой сцене.
- RAM: редактор с открытой средней сценой ≤ 1.5 ГБ. Стриминг обязателен для больших миров.
- Компиляция должна быть быстрой: unity-build по модулям опционально, PCH для каждого модуля.

### 1.2 Тиры качества
| Tier | Железо | Рендер |
|---|---|---|
| Low | iGPU (UHD 620 и т.п.) | Forward+, CSM 2–3 каскада, SSAO, baked/probe GI, FXAA/TAA, ≤ 70 % res-scale |
| Medium | GTX 1060 / RX 580 | + Deferred-путь, 4 каскада, SSR, volumetric fog, DDGI-lite |
| High | RTX 20xx+/RX 6xxx+ | + Virtual geometry (ZNano), virtual shadow maps, HW RT (DXR) для GI/теней если есть |
| Ultra | RTX 40xx+ | + Path tracer (референс), максимальные настройки |

### 1.3 Режимы движка: Lite и Full (требование пользователя, 2026-10-11)
Движок работает в одном из двух **режимов** (`EngineProfile`), выбираемых автоматически по железу или вручную
(`-lite` / `-full`, CVar `engine.Profile`, настройка проекта и редактора):

| | **Lite** (слабые устройства) | **Full** (мощные устройства) |
|---|---|---|
| Цель | iGPU (UHD 620), 8 ГБ RAM, 4 ядра | дискретные GPU уровня Medium/High/Ultra |
| Тиры качества | Low (Medium вручную) | Medium / High / Ultra (по caps) |
| Рендер | Forward+, baked/probe GI, CSM 2–3, FXAA/TAA, res-scale ≤ 70 % | Deferred + все техники раздела 6.4 (ZNano, VSM, DDGI/Lumen-lite, RT) |
| Редактор | упрощённый вьюпорт (без real-time GI превью), меньше фоновых задач, ленивые тумбнейлы | полный |
| Память | меньшие бюджеты стриминга, агрессивная выгрузка ассетов | расширенные бюджеты |
| Job system | worker'ов = ядра − 1, IO-потоков 1 | ядра − 1, IO-потоков 2–4 |

Правила: режим — это набор дефолтов (CVar-пресет `Scalability`), а не форк кода; любая фича Full обязана иметь
фолбэк или отключаться в Lite; авто-выбор: UMA/интегрированный GPU или < 3 ГБ VRAM или < 12 ГБ RAM → Lite.

---

## 2. Тулчейн и сборка

### 2.1 Что установлено и используется (проверено 2026-10-11)
- **MSVC 14.44** (VS 2022 BuildTools): `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`
  (есть ещё VS 18 BuildTools с MSVC 14.51 — запасной вариант).
- **CMake + Ninja** из BuildTools: `...\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\{CMake\bin,Ninja}`.
- **Windows SDK 10.0.26100.0**: D3D12, DXGI, XAudio2, WASAPI, Win32 — заголовки и либы.
- **DXC** (компилятор HLSL → DXIL): `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe`
  (рантайм-компиляция через `dxcompiler.dll` из того же SDK).
- Python 3.14 — только для вспомогательных скриптов (не обязателен для сборки).
- Vulkan SDK, git-репозитория пока нет.

### 2.2 Язык и стандарты
- **C++20** (MSVC `/std:c++20 /permissive- /Zc:__cplusplus /utf-8 /W4 /WX` для своего кода, `/MP`).
- **HLSL Shader Model 6.0+** (целевой 6.6 при поддержке, фолбэк 6.0/6.2).
- Исключения C++ **выключены** в рантайме (`/EHs-c-`, `_HAS_EXCEPTIONS=0`), RTTI **выключен** (`/GR-`) —
  своя рефлексия и `Result<T, E>`. В редакторе/инструментах тоже стараемся без исключений.
- STL: можно `<atomic>`, `<type_traits>`, `<utility>`, `<new>`, `<initializer_list>`, `<bit>`, `<concepts>`,
  `<cstring>`, `<cmath>`, `<charconv>` (to_chars/from_chars для float), `<cstdio>`. Контейнеры/строки — **свои** (`zx::Array`, `zx::HashMap`, `zx::String`, ...),
  потому что нужны аллокаторы, предсказуемые раскладки и быстрая отладка.

### 2.3 Политика сторонних зависимостей
**Ноль сторонних библиотек.** Разрешено только то, что идёт с ОС/Windows SDK: Win32, D3D12, DXGI, DXC,
XAudio2/WASAPI, `dbghelp` (стектрейсы), `ole32`/`shell32` (диалоги файлов).
Пишем сами: zlib-inflate/deflate, PNG, JPEG (baseline+progressive), TGA, HDR(RGBE), DDS, WAV, Ogg Vorbis,
JSON, glTF 2.0/GLB, OBJ, FBX (бинарный, поздняя фаза), TrueType/WOFF, BC1–BC7 энкодер, LZ4-подобный компрессор, хеши (xxHash-подобный, SHA-1 для GUID-ов).
Исключение возможно только через ADR (раздел 15) с обоснованием.

### 2.4 Система сборки
- CMake ≥ 3.28, генератор Ninja Multi-Config; конфигурации:
  - `Debug` — без оптимизаций, все проверки, D3D12 debug layer + GPU-based validation (опционально).
  - `Development` — `/O2`, ассерты включены, профайлер включён, debug layer выключен. **Основная для работы.**
  - `Shipping` — `/O2 /GL` + LTCG, без ассертов/логов уровня Debug, без редактора.
- `Tools/build.ps1` — поднимает окружение `vcvars64.bat`, вызывает `cmake --preset` и `cmake --build`.
  Пример: `powershell -ExecutionPolicy Bypass -File Tools/build.ps1 -Config Development -Target ZXEditor -Test` (pwsh не установлен — только Windows PowerShell 5.1).
- `CMakePresets.json` — пресеты `win64-debug`, `win64-dev`, `win64-shipping`.
- Каждый модуль = статическая библиотека (`ZXCore`, `ZXRender`, ...). Редактор и игра — exe.
  Игровой код проекта — **DLL с горячей перезагрузкой** (раздел 6.12).
- Выход сборки: `Build/<preset>/` (бинарники в `Binaries/Win64/<Config>/`). `Build/`, `Intermediate/`, `Saved/` — в `.gitignore`.
- Шейдеры компилируются в рантайме через `dxcompiler.dll` с кешем на диске (`Intermediate/ShaderCache/`),
  при упаковке — оффлайн-компиляция в `.zshaderlib`.

### 2.5 Как запускать сборку из агента (для Claude)
В Bash/PowerShell сначала надо окружение MSVC. Надёжный способ:
```
cmd /c "call \"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat\" && cmake --preset win64-dev && cmake --build --preset win64-dev"
```
Либо `Tools/build.ps1`, который делает это сам. Долгие сборки — в фоне.

---

## 3. Правила кода

### 3.1 Нейминг
- Namespace: `zx` (подпространства: `zx::rhi`, `zx::render`, `zx::phys`, `zx::ed` для редактора).
- Типы: `PascalCase` (`RenderGraph`, `Vec3`). Функции/методы: `PascalCase` (`CreateBuffer`). Переменные: `camelCase`.
  Члены класса: `m_camelCase`. Статики: `s_camelCase`. Глобалы: `g_camelCase`. Константы/enum-значения: `PascalCase`.
- Макросы: `ZX_UPPER_CASE`. Файлы: `PascalCase.h/.cpp`, шейдеры `PascalCase.hlsl`, общие шейдерные инклюды `.hlsli`.
- Интерфейсы без префикса `I` (кроме COM-обёрток). Хэндлы — `XxxHandle` (типизированный 32-бит индекс+поколение).

### 3.2 Принципы
- **Data-oriented** в горячих путях (ECS, рендер, физика): SoA, линейные массивы, никаких виртуальных вызовов на объект.
- Виртуальные функции допустимы на уровне систем/бэкендов (RHI, ассет-импортеры, панели редактора).
- Владение явное: `UniquePtr`, `RefPtr` (интрузивный счётчик) только для разделяемых ресурсов, хэндлы для ресурсов в пулах.
- Никаких глобальных конструкторов с побочными эффектами, кроме регистрации рефлексии/модулей через явные макросы.
- Всё, что может упасть при вводе извне (файлы, ассеты, шейдеры), возвращает `Result<T>`; внутренние инварианты — `ZX_ASSERT`.
- Потокобезопасность документируется в заголовке класса (`// Thread-safety: main thread only` и т.п.).
- Заголовки самодостаточны, минимум инклюдов, forward-declarations. `#pragma once`.
- Комментарии — на английском в коде; документация в `docs/` — на русском.
- Каждый модуль имеет `Public/` (API для других модулей) и `Private/` (реализация).

### 3.3 Форматирование
`.clang-format` (на основе LLVM: 4 пробела, 120 колонок, фигурные скобки Allman для функций/классов). `.editorconfig`.

### 3.4 Определение «готово» (DoD) для любой задачи
1. Компилируется без варнингов в Debug и Development.
2. Есть тест (unit или sample/golden), если это логика, а не UI-обвязка.
3. Нет утечек (трекинг аллокаций Core при выходе = 0 байт), нет ошибок D3D12 debug layer.
4. Обновлены чекбоксы в разделе 14 и, если было архитектурное решение, ADR в разделе 15.

---

## 4. Архитектура

### 4.1 Слои (зависимости только сверху вниз)
```
┌──────────────────────────────────────────────────────────────────────┐
│ Apps:  ZXEditor.exe   ZXGame.exe (Launcher)   Tools (ShaderC, Cooker) │
├──────────────────────────────────────────────────────────────────────┤
│ Editor:  EditorCore · EditorUI · Panels · Gizmos · AssetEditors       │
├──────────────────────────────────────────────────────────────────────┤
│ Engine:  Engine (World, GameLoop, Subsystems) · Script · GameFramework│
├──────────┬──────────┬──────────┬───────────┬──────────┬──────────────┤
│ Render   │ Physics  │Animation │  Audio    │   UI     │ Nav / AI     │
├──────────┴──────────┴──────────┴───────────┴──────────┴──────────────┤
│ Scene (ECS, Transform, Hierarchy, Prefab)   Asset (Registry, Loader)  │
├──────────────────────────────────────────────────────────────────────┤
│ RHI (abstract)  ──►  RHI_D3D12  (позже RHI_Vulkan)                    │
├──────────────────────────────────────────────────────────────────────┤
│ Platform (Win32: окно, ввод, файлы, потоки, DLL, время, диалоги)      │
├──────────────────────────────────────────────────────────────────────┤
│ Core (память, контейнеры, строки, математика, jobs, лог, рефлексия,   │
│       сериализация, ФС, конфиг, профайлер, хеши, сжатие)               │
└──────────────────────────────────────────────────────────────────────┘
```

### 4.2 Потоки
- **Main/Game thread** — ввод, логика, ECS-системы, физика (шаг запускает задачи в job system).
- **Render thread** — получает `RenderScene`-снапшот кадра N, строит render graph, записывает командные листы
  (запись параллелится через jobs). Игра работает на кадр впереди рендера (pipelining, 2 кадра в полёте на GPU).
- **Job system** — пул воркеров (кол-во логических ядер − 2, минимум 2), fiber-free work-stealing очередь,
  `JobHandle` + зависимости, `ParallelFor`.
- **IO thread(s)** — асинхронное чтение файлов, распаковка, загрузка ассетов; upload на GPU через copy-queue.
- **Audio thread** — микшер, отдельный, с высоким приоритетом.

### 4.3 Кадр (frame flow)
```
BeginFrame → Platform.PumpEvents → Input.Update → Time.Tick
→ Script/Game systems (Update) → Physics.Step (fixed 60 Hz, accumulator, интерполяция)
→ Animation (параллельно) → Transform propagation → Audio.UpdateListeners
→ Render.ExtractScene (снапшот в RenderScene) → [handoff на render thread]
→ Editor/UI build → EndFrame
Render thread: RenderGraph.Setup → Compile (culling ресурсов/барьеры/алиасинг) → Execute → Present
```

### 4.4 Модули рантайма и их ответственность (коротко)
| Модуль | Ответственность |
|---|---|
| Core | фундамент, без зависимостей от ОС кроме через Platform-интерфейс в Core/HAL |
| Platform | Win32-реализация HAL: окна, события, ввод, файловая система, потоки, DLL, диалоги, буфер обмена |
| RHI | абстракция GPU: устройства, очереди, буферы, текстуры, пайплайны, дескрипторы, команды |
| RHI_D3D12 | реализация RHI на D3D12 |
| Render | render graph, сцена рендера, материалы, все техники освещения/постобработки |
| Asset | GUID-ы, реестр, импорт, кукинг, асинхронная загрузка, хот-релоад, пакеты |
| Scene | ECS, компоненты, иерархия, префабы, сериализация сцен, World Partition |
| Physics | ZPhys: свой физдвижок (rigid body, коллизии, джоинты, персонаж, запросы) |
| Animation | скелеты, клипы, блендинг, state machine, IK, скиннинг |
| Audio | микшер, 3D-звук, декодеры, шины, эффекты |
| UI | ZUI: шрифты, рендер 2D, immediate-mode API (редактор) + retained виджеты (игра) |
| Script | hot-reload C++ модулей игры + ZGraph (визуальный скриптинг) |
| Nav | навмеш, поиск пути, crowd, behaviour trees |
| Engine | World, GameLoop, подсистемы, GameFramework (Actor-like API поверх ECS), настройки проекта |
| Net | (поздно) репликация, клиент-сервер |

---

## 5. Структура репозитория

```
ZeqouX3D/
├─ CLAUDE.md                     # краткие правила для агента + ссылка на этот ТЗ
├─ README.md
├─ CMakeLists.txt                # корневой
├─ CMakePresets.json
├─ .clang-format  .editorconfig  .gitignore
├─ cmake/                        # ZXModule.cmake (функция zx_add_module), флаги, хелперы
├─ docs/
│  ├─ TZ.md                      # этот документ
│  ├─ architecture/              # углублённые доки по подсистемам (Render.md, ECS.md, ...)
│  └─ formats/                   # спецификации своих форматов (.zasset, .zscene, .zpak ...)
├─ Tools/
│  ├─ build.ps1                  # сборка с vcvars
│  ├─ run.ps1                    # запуск редактора/сэмплов
│  └─ new_module.py              # генератор модуля
├─ Engine/
│  ├─ Source/
│  │  ├─ Runtime/
│  │  │  ├─ Core/        {Public,Private,Tests}
│  │  │  ├─ Platform/    {Public,Private/Win32}
│  │  │  ├─ RHI/         {Public,Private}
│  │  │  ├─ RHI_D3D12/   {Private}
│  │  │  ├─ Render/      {Public,Private/{Graph,Scene,Passes,Materials,Lighting,Post,GI,Geometry,Terrain,Sky,Particles}}
│  │  │  ├─ Asset/       {Public,Private/{Importers,Cook,Formats}}
│  │  │  ├─ Scene/       ...
│  │  │  ├─ Physics/     ...
│  │  │  ├─ Animation/   ...
│  │  │  ├─ Audio/       ...
│  │  │  ├─ UI/          ...
│  │  │  ├─ Script/      ...
│  │  │  ├─ Nav/         ...
│  │  │  └─ Engine/      ...
│  │  ├─ Editor/
│  │  │  ├─ EditorCore/  # undo/redo, selection, commands, project, PIE
│  │  │  ├─ EditorUI/    # докинг, темы, иконки, виджеты
│  │  │  ├─ Panels/      # Viewport, Outliner, Inspector, ContentBrowser, Console, Profiler, ...
│  │  │  └─ AssetEditors/# Material graph, Animation, Particle, Prefab, ...
│  │  └─ Programs/
│  │     ├─ ZXEditor/    # main() редактора
│  │     ├─ ZXGame/      # main() упакованной игры
│  │     ├─ ZXShaderC/   # оффлайн-компилятор шейдеров
│  │     └─ ZXCook/      # CLI кукинга/упаковки
│  ├─ Shaders/
│  │  ├─ Common/         # .hlsli: Math, BRDF, Bindless, Packing, Noise, ColorSpaces
│  │  ├─ Passes/         # DepthPrepass, GBuffer, ForwardPlus, Shadows, Lighting, ...
│  │  ├─ Post/           # TAA, Bloom, Tonemap, DOF, MotionBlur, Upscale
│  │  ├─ Compute/        # Culling, LightGrid, HiZ, Skinning, Particles
│  │  └─ UI/             # 2D, SDF-текст
│  └─ Content/
│     ├─ Fonts/          # Inter (WOFF, из inter.zip), Lucide (TTF, из lucide-font zip)
│     ├─ Icons/          # SVG Lucide (из lucide-icons zip) → растеризуются в атлас
│     ├─ Materials/      # дефолтные материалы
│     ├─ Textures/       # BRDF LUT, blue noise, дефолтные (white/black/normal/checker)
│     └─ Meshes/         # примитивы (куб, сфера, плоскость, цилиндр, капсула)
├─ Samples/
│  ├─ HelloTriangle/  Sponza/  PhysicsPlayground/  ThirdPersonDemo/
├─ Tests/                        # ZXTests.exe (свой мини-фреймворк), golden images
└─ Templates/                    # шаблоны новых проектов (Blank, FirstPerson, ThirdPerson)
```

**Исходные архивы в корне** (`inter.zip`, `lucide-font-1.54.0.zip`, `lucide-icons-1.54.0.zip`) распаковываются
в `Engine/Content/Fonts|Icons` на вехе M0/M6, сами архивы не удаляются без разрешения пользователя.
Inter: в архиве есть static TTF → берём `inter-{latin,latin-ext,cyrillic,cyrillic-ext,greek}-{400..700}-normal.ttf` + 400 italic
(шрифт разбит на поднаборы по диапазонам Unicode — шрифтовая система грузит их как цепочку фолбэков). Lucide: `lucide.ttf` + `codepoints.json` (иконки как глифы шрифта — самый дешёвый путь),
SVG-версии — запас на будущее.

### 5.1 Проект игры (пользовательский)
```
MyGame/
├─ MyGame.zproj                  # JSON: имя, версия движка, модули, стартовая сцена, настройки
├─ Content/                      # исходные ассеты + .zmeta рядом с каждым (GUID + настройки импорта)
├─ Source/MyGame/                # C++ игровой модуль (DLL, hot-reload)
├─ Config/                       # Input.zcfg, Rendering.zcfg, Physics.zcfg, ...
├─ Intermediate/                 # кеши: DDC (cooked), ShaderCache, Thumbnails  (не в VCS)
└─ Saved/                        # логи, автосейвы, краш-дампы                 (не в VCS)
```

---

## 6. Подсистемы — детальные требования

### 6.1 Core

#### 6.1.1 Базовое
- `Types.h`: `u8..u64, i8..i64, f32, f64, usize`. `ZX_ASSERT(cond, fmt...)`, `ZX_VERIFY`, `ZX_UNREACHABLE`, `ZX_CHECK_SLOW` (только Debug).
- Платформенные макросы, `ZX_FORCEINLINE`, `ZX_NOINLINE`, `ZX_LIKELY`.
- `Result<T, Error>` + `Error{code, message}`; `ZX_TRY(expr)`.
- Крэш-хендлер: SEH-фильтр → минидамп (`dbghelp`) + стектрейс в лог + диалог.

#### 6.1.2 Память
- Глобальный аллокатор-обёртка с тегами (`MemTag::Render`, `::Physics`, ...) и статистикой; в Debug — трекинг утечек с колстеками.
- `LinearAllocator` (per-frame, сбрасывается каждый кадр), `StackAllocator`, `PoolAllocator<T>`, `TlsfAllocator` (общий heap с низкой фрагментацией), `VirtualArena` (резерв адресного пространства + коммит по мере роста).
- Frame-allocator на каждый поток для временных данных кадра.

#### 6.1.3 Контейнеры
`Array<T>` (dynamic), `FixedArray<T,N>`, `SmallArray<T,N>` (inline storage), `Span<T>`, `HashMap<K,V>` (open addressing, Robin Hood / Swiss-like), `HashSet`, `SparseSet`, `SlotMap<T>` (хэндлы с поколением), `RingBuffer`, `BitArray`, `Optional`, `Variant`, `Function<Sig>` (small buffer), `Delegate`/`MulticastDelegate`, lock-free `MPMCQueue`, `SPSCQueue`.

#### 6.1.4 Строки и текст
`String` (UTF-8, SSO), `StringView`, `Name` (интернированная строка, сравнение по id — для имён свойств/ассетов/тегов), форматирование `Format("{} {:.2f}", ...)` свой (без `<format>`), UTF-8↔UTF-16 для Win32, `Path` (нормализация, join, extension).

#### 6.1.5 Математика (ZMath)
- `Vec2/3/4`, `IVec*`, `UVec*`, `Quat`, `Mat3`, `Mat4` (column-major, совместимо с HLSL `row_major`-соглашением — фиксируем в ADR-001), `Transform` (TRS), `AABB`, `OBB`, `Sphere`, `Plane`, `Frustum`, `Ray`, `Color`/`LinearColor`, `Rect`.
- SIMD (SSE4.1/AVX2 с рантайм-детектом) для `Vec4`/`Mat4`/batch-операций. Скалярный фолбэк.
- Правосторонняя система координат? **Решение: левосторонняя, Y-up, +Z вперёд, метры**, reversed-Z depth (ADR-002).
- Функции: проекции (perspective reversed-Z infinite far, ortho), lookAt, slerp, decompose, half-float, packing (octahedral normals, RGBM, R11G11B10), шумы (Perlin/Simplex/Worley), сплайны (Catmull-Rom, Bezier), кривые анимации, RNG (PCG32, xoshiro).
- Пересечения: ray–AABB/sphere/triangle/plane, frustum–AABB/sphere, AABB–AABB.

#### 6.1.6 Многопоточность
`Thread`, `Mutex`, `SpinLock`, `RWLock`, `ConditionVariable`, `Semaphore`, `Atomic<T>`, `Event`.
**JobSystem**: `Schedule(job, deps)`, `ParallelFor(range, batch, fn)`, `Wait(handle)` (ожидающий поток сам выполняет задачи), приоритеты (High/Normal/Low/IO), счётчики зависимостей.

#### 6.1.7 Логирование и консоль
- `ZX_LOG(Category, Level, fmt...)`, уровни Trace/Debug/Info/Warn/Error/Fatal, категории регистрируются.
- Синки: файл (`Saved/Logs/`), OutputDebugString, консоль, редакторская консоль (кольцевой буфер).
- **CVars**: `CVar<int> r_shadowQuality("r.Shadow.Quality", 2, "help", CVarFlags::Scalability)` — читаются/пишутся из консоли, конфигов и командной строки. Консольные команды `ConCommand`.

#### 6.1.8 Рефлексия (ZRefl)
Ключевой механизм для инспектора, сериализации, undo, скриптинга.
- Макросы регистрации без кодогенерации (фаза 1):
  ```cpp
  struct LightComponent {
      ZX_REFLECT_STRUCT(LightComponent)
      LinearColor color{1,1,1,1};
      f32 intensity = 10.f;   // lux / lumens по типу
      LightType type = LightType::Point;
  };
  ZX_REFLECT_BEGIN(LightComponent)
      ZX_FIELD(color, Meta::Color, Meta::Tooltip("Light color"))
      ZX_FIELD(intensity, Meta::Range(0, 100000), Meta::Units("lm"))
      ZX_FIELD(type)
  ZX_REFLECT_END()
  ```
- `TypeInfo`: имя, размер, выравнивание, конструктор/деструктор/копирование, поля (имя, смещение, тип, метаданные), enum-значения, методы (для ZGraph), базовый тип.
- Типы: примитивы, enum (с именами), struct, `Array<T>`, `HashMap`, `String`, `Name`, `AssetRef<T>`, `EntityRef`, математика.
- Фаза 2 (опционально): `ZXHeaderTool` — парсер аннотаций `ZCLASS()/ZPROPERTY()` → генерирует тот же код (решение по ADR).

#### 6.1.9 Сериализация
- **Текстовый формат**: свой JSON (парсер/райтер с комментариями и trailing commas — «ZJSON») — для `.zscene`, `.zmeta`, `.zmat`, `.zproj`, конфигов. Дифф-дружелюбный, стабильный порядок ключей.
- **Бинарный формат**: `BinaryWriter/Reader` с версионированием, выравниванием, little-endian; для cooked-ассетов.
- Сериализация по рефлексии автоматически: `Serialize(Archive&, void* obj, const TypeInfo&)`; дельта относительно дефолта/префаба.
- Версионирование типов: `ZX_FIELD_RENAMED(old, new)`, `Upgrade(version)` хуки.

#### 6.1.10 Файловая система
VFS с монтированием: `/Engine/` → `Engine/Content`, `/Game/` → `MyGame/Content`, `.zpak`-архивы монтируются поверх. Асинхронный IO (`ReadFileAsync` → IOCP/overlapped). Отслеживание изменений (`ReadDirectoryChangesW`) для хот-релоада.

#### 6.1.11 Прочее Core
- Хеши: xxHash64-подобный (свой), CRC32, FNV-1a (compile-time), SHA-1 (для GUID v5).
- `Guid` (128-бит, генерация через `CoCreateGuid` или свой RNG), строковое представление.
- Сжатие: **inflate/deflate** (zlib/PNG/WOFF), **ZLZ** (свой LZ4-подобный быстрый формат для пакетов).
- Время: `Clock`, `Timer`, `FrameTime` (dt, smoothed dt, timescale).
- **Профайлер**: `ZX_PROFILE_SCOPE("Name")` CPU-зоны (per-thread кольцевые буферы), GPU-таймстемпы через RHI, вывод в панель редактора (flame graph/timeline), экспорт в формат Chrome Tracing (`.json`).
- Event bus (типизированные события), `Delegate`.
- Модульная система: `IModule { Startup(); Shutdown(); }`, порядок по зависимостям.

### 6.2 Platform (Win32)
- Окна: создание нескольких окон (редактор: главное + плавающие панели), DPI-aware (Per-Monitor V2), режимы fullscreen/borderless/windowed, изменение размера без мерцания, кастомный title bar редактора (`WM_NCHITTEST`).
- Ввод: Raw Input для мыши (относительные движения) и клавиатуры, `WM_CHAR` для текста (UTF-16 → UTF-8, IME базово), XInput для геймпадов (до 4), курсоры (системные + скрытие/захват).
- **Input system** поверх: Action Mapping (как Unity Input System / UE Enhanced Input): `InputAction` (Button/Axis1D/Axis2D), биндинги, модификаторы (dead zone, negate, swizzle), триггеры (Pressed/Released/Hold/Tap), контексты с приоритетами. Конфиг `Config/Input.zcfg`.
- Буфер обмена, файловые диалоги (IFileDialog), drag&drop файлов в окно (импорт ассетов), открытие URL/папки.
- Загрузка DLL, копирование DLL для хот-релоада, перехват `stdout`.
- Информация о системе: CPU (cpuid, кол-во ядер), RAM, мониторы.

### 6.3 RHI (Render Hardware Interface)

#### 6.3.1 Абстракция
- Объекты: `Device`, `Queue` (Graphics/Compute/Copy), `SwapChain`, `Buffer`, `Texture`, `TextureView`, `Sampler`, `Shader`, `GraphicsPipeline`, `ComputePipeline`, `CommandList`, `Fence`, `QueryPool`, (позже) `AccelerationStructure`, `RayTracingPipeline`.
- Хэндлы вместо указателей; отложенное удаление (ресурс живёт до завершения кадров в полёте).
- **Bindless-модель по умолчанию**: все SRV/UAV в одной большой shader-visible дескрипторной куче; шейдер получает индексы через root constants / constant buffer. На SM 6.6 — `ResourceDescriptorHeap[]`, на SM < 6.6 — неограниченные массивы `Texture2D g_Textures[] : register(t0, space1)` (требует Resource Binding Tier 2+).
- Единая root signature для всего движка (bindless): root constants (32 DWORD), 2–3 root CBV, дескрипторные таблицы bindless, статические сэмплеры (набор стандартных).
- Барьеры: **enhanced barriers** если поддерживаются (AgilitySDK недоступен — проверять ОС/драйвер), иначе legacy resource barriers. RHI-API в терминах «состояний доступа»; render graph вычисляет барьеры сам.
- Командные листы записываются в несколько потоков; аллокаторы на кадр/поток.
- Upload: кольцевой upload-буфер + copy queue для стриминга; staging-аллокатор на кадр для константных данных.
- Память: свой суб-аллокатор heap-ов (placed resources, 64 МБ-блоки, TLSF по блоку), алиасинг transient-ресурсов render graph.
- Отладка: имена ресурсов, PIX-маркеры (`BeginEvent/EndEvent` через `ID3D12GraphicsCommandList::BeginEvent` — без WinPixEventRuntime), DRED при device removed, debug layer + GBV в Debug.
- Pipeline State Cache: хеш описания → PSO, сериализация `ID3D12PipelineLibrary` на диск.

#### 6.3.2 Детект возможностей (Capabilities)
На старте заполняется `RHICaps`: shader model, resource binding tier, wave ops + размер волны, 16-bit ops, typed UAV loads, conservative raster, ROVs, variable rate shading, mesh shaders, raytracing tier, enhanced barriers, sampler feedback, UMA (встройка!). Рендер выбирает пути по caps, а не по тиру вручную.
UMA-оптимизация: на iGPU ресурсы можно писать CPU напрямую (`D3D12_HEAP_TYPE_CUSTOM` + write-combine) — без copy queue.

#### 6.3.3 Бэкенды
- **D3D12** — единственный на старте.
- **Vulkan 1.3** — веха M20+, свой загрузчик функций (без Vulkan SDK; заголовки — свои минимальные), шейдеры HLSL → SPIR-V через DXC (`-spirv`).
- **Null RHI** — для тестов без GPU и серверных билдов.

### 6.4 Render

#### 6.4.1 Render Graph (ZGraph-R, аналог RDG/FrameGraph)
- Пассы декларируют ресурсы (`Read`, `Write`, `Create transient`), лямбда исполнения.
- Компиляция: отсечение неиспользуемых пассов, вычисление времени жизни, **алиасинг памяти** transient-текстур, автоматические барьеры, async compute (позже: метки очереди на пассе, кросс-очередная синхронизация).
- Импорт внешних ресурсов (swapchain, history-буферы TAA, shadow atlas).
- Отладка: визуализация графа в редакторе, просмотр любой промежуточной текстуры («Buffer Visualization» как в UE).

#### 6.4.2 Render Scene и экстракт
- `RenderScene` — плотные массивы примитивов (instances), источников света, декалей, рефлекшн-проб, отражает ECS через `RenderProxy` с dirty-трекингом (обновляются только изменённые записи — GPU scene buffer).
- **GPU Scene**: все instance-данные (матрицы, prev-матрицы для motion vectors, индекс материала, bounds) в одном structured buffer; апдейты scatter-копированием.
- Visibility: CPU-фрустум-куллинг (BVH/грид, параллельно) на Low; GPU-driven culling (compute: frustum + HiZ occlusion, two-phase) + `ExecuteIndirect` начиная с M12.

#### 6.4.3 Геометрия и меши
- Формат вершин: позиция (f32×3), нормаль+тангенс (октаэдр/квантованные), UV0/UV1 (half), цвет (опционально), skin indices/weights (u8×4/unorm8×4 или ×8).
- Vertex pulling из bindless-буферов (без input layout) — единый путь для всех мешей.
- Подмеши (sections) с отдельными материалами. LOD-цепочка (генерируется при импорте — свой QEM-симплификатор), выбор LOD по screen size с dithered crossfade.
- Оптимизация при импорте: vertex cache (Forsyth/Tom F.), overdraw, vertex fetch; генерация мешлетов/кластеров (128 треугольников) — заготовка под M15.

#### 6.4.4 Материалы
- **Материальная модель**: Metallic/Roughness PBR (glTF-совместимая), shading models: `DefaultLit`, `Unlit`, `Subsurface` (простой wrap/pre-integrated), `ClearCoat`, `Cloth` (Charlie sheen), `TwoSidedFoliage`, `Hair` (поздно).
- Домены: Surface, Decal, PostProcess, UI, Sky, Volume.
- Blend: Opaque, Masked (alpha test + dithered), Translucent, Additive, Modulate.
- **Material Graph** (редактор нод, как UE Material Editor / Unity Shader Graph): ноды → генерация HLSL-функции `EvaluateMaterial(MaterialInputs) → SurfaceData`, вставляется в шаблоны пассов. Ноды: текстуры, параметры, математика, UV-операции, Fresnel, время, world position, vertex color, lerp/if, функции (subgraphs), triplanar, параллакс (POM), детализация, шумы.
- **Material Instance**: переопределение параметров без перекомпиляции; параметры в GPU-буфере материалов (bindless индексы текстур).
- Пермутации шейдеров: ключ = (материал, пасс, вершинная фабрика (static/skinned/terrain/particle), фичи). Компиляция асинхронно, пока не готово — fallback-материал (серый checker).

#### 6.4.5 Пути рендеринга
1. **Forward+ (clustered)** — основной для Tier Low и прозрачных: depth prepass → light clustering (compute: 3D-сетка кластеров 16×9×24, списки индексов света) → forward opaque pass с MSAA опционально.
2. **Deferred (visibility/GBuffer)** — Tier Medium+: GBuffer (A: base color+AO, B: normal (oct)+roughness+metallic, C: emissive/custom, Velocity) → tiled/clustered deferred lighting в compute.
Оба пути делят шейдерную библиотеку BRDF и освещения. Выбор через CVar `r.Renderer.Path`.

#### 6.4.6 Освещение
- Источники: Directional (солнце), Point, Spot, Rect area (LTC), Sky light (IBL из куба/неба). Физические единицы (люкс, люмены, кандела), экспозиция (EV100).
- BRDF: GGX/Trowbridge-Reitz + Smith height-correlated + Schlick Fresnel, Lambert/Burley diffuse, multiscatter energy compensation, specular occlusion.
- IBL: prefiltered specular cubemap (GGX importance sampling, mip-chain по roughness), irradiance через SH9, BRDF LUT (split-sum).
- Reflection probes: box/sphere с параллакс-коррекцией, захват в редакторе (bake) и realtime-обновление (по одному лицу за кадр).
- IES-профили для spot/point (поздно).
- Light functions/cookies.

#### 6.4.7 Тени
- Directional: **CSM** 2–4 каскада, стабилизация (texel snapping), PCF (5×5 Poisson / оптимизированный), фильтрация с blend между каскадами, contact shadows (screen-space ray march).
- Point/Spot: атлас теней (одна большая текстура, выделение тайлов по важности/размеру на экране), cube-тени в атласе.
- PCSS (мягкие тени) — опция High.
- Кэширование статичных теней (обновлять только при движении).
- **Virtual Shadow Maps** (16k² виртуальная, страницы 128²) — High-тир, M16.

#### 6.4.8 Глобальное освещение
Многоуровнево (ADR при реализации):
- **GI-0 Baked**: свой GPU/CPU-лайтмаппер (path tracing, xatlas-подобный свой UV-анвраппер для lightmap UV), лайтмапы + light probes (SH) для динамики. Работает везде, лучшее качество на Low.
- **GI-1 DDGI-lite**: сетка проб с irradiance+visibility (октаэдральные карты), лучи трассируются в compute против **глобального SDF сцены** (меш-SDF генерируются при импорте, склеиваются в clipmap-объём). Тир Medium.
- **GI-2 «Lumen-lite»**: screen-space probes + SDF-трассировка + surface cache. Тир High, M17.
- **SSGI** и **SSAO/GTAO** (GTAO по умолчанию), **SSR** (hierarchical ray march по HiZ) с фолбэком на пробы.
- HW RT (DXR 1.1 inline) — если caps позволяют: RT-тени/отражения/GI.

#### 6.4.9 Небо, атмосфера, погода
- Физическое небо: precomputed atmospheric scattering (Hillaire 2020: transmittance/multiscatter/sky-view LUT, aerial perspective froxel-объём).
- Объёмные облака (ray march по 3D-шуму, temporal reprojection) — Medium+.
- Exponential height fog + **volumetric fog** (froxels, светит от всех источников и теней).
- Звёзды/луна, time of day.

#### 6.4.10 Прозрачность и спецэффекты
- Сортировка прозрачных (по дистанции, back-to-front), OIT (weighted blended) — опция.
- Refraction (screen-space distortion), вода (FFT/Gerstner-волны, screen-space reflection + refraction, caustics — поздно).
- **Декали**: deferred decals (проекционные боксы) + в Forward+ через кластеры.
- **Частицы (ZFX)**: GPU-частицы (compute: эмит, симуляция, сортировка bitonic, рендер: billboard/stretched/mesh/ribbon), CPU-частицы для малых систем; модульный редактор как Niagara/VFX Graph (модули: spawn rate, burst, velocity, forces, noise, collision с depth buffer, color/size over life, sub-emitters, flipbook-анимация).

#### 6.4.11 Постобработка (в линейном HDR, порядок)
TAA → (Upscale) → Motion Blur (per-object, по velocity) → DOF (bokeh, gather-based) → Bloom (dual filter / FFT-convolution на High) → Auto Exposure (гистограмма в compute) → Lens effects (flare, dirt, chromatic aberration, vignette, grain) → **Tonemapping** (ACES-fitted / AgX / Neutral — выбор) + color grading (3D LUT 32³, white balance, lift/gamma/gain, curves) → Sharpen → UI composite → вывод (sRGB, позже HDR10/scRGB).
**Post-process volumes** с блендингом по позиции камеры (как в UE/Unity Volume).

#### 6.4.12 Антиалиасинг и апскейл
FXAA (Low), **TAA** (jitter Halton, neighborhood clamp в YCoCg, motion vectors, history rejection), свой **temporal upsampler ZTSR** (как TAAU/TSR, 50–100 % render scale). Dynamic resolution по GPU-бюджету. MSAA только в Forward+.

#### 6.4.13 Террейн и растительность
- Heightmap-террейн: CDLOD / clipmap, GPU тесселяция через compute-генерацию патчей, до 8 слоёв материалов (splat/weight maps), holes, стриминг тайлов.
- Инструменты редактора: sculpt (raise/lower/smooth/flatten/noise), paint layers.
- Foliage: инстансинг (GPU-culling), LOD + импостеры (октаэдральные), ветер (vertex animation), процедурное рассаживание по правилам.

#### 6.4.14 Virtual Geometry «ZNano» (аналог Nanite, M15)
Без mesh shaders: кластеры по 128 треугольников, иерархия LOD-групп (DAG, как Nanite) строится при импорте (QEM-симплификация групп кластеров с фиксированными границами), GPU: persistent-culling кластеров по иерархии (compute), two-pass HiZ occlusion, запись видимых кластеров → **visibility buffer** (`instanceID|clusterID|triangleID` в R32G32_UINT) через ExecuteIndirect с компактными индекс-буферами, затем материальные пассы по тайлам (material classify). Софтверный растеризатор мелких треугольников в compute — опция High. Стриминг страниц кластеров.

#### 6.4.15 Отладка рендера
Режимы вьюпорта: Lit, Unlit, Wireframe, Lighting only, Detail lighting, Normals, Roughness, Metallic, Base color, AO, Overdraw, Shader complexity, Light complexity (кол-во источников на кластер), LOD coloration, Cascade visualization, Motion vectors, GI probes. Статистика: draw calls, треугольники, PSO, память GPU, время пассов. Debug draw API (`DebugDraw::Line/Box/Sphere/Text`, persistent с длительностью, depth-tested/overlay).

#### 6.4.16 Камера
Физическая камера: FOV или sensor+focal length, aperture (f-stop), shutter, ISO → экспозиция; near plane, reversed-Z infinite far; ортографическая. Камера-рендер-таргеты (render to texture, для порталов/зеркал/миникарт). Сплит-скрин (несколько вьюпортов).

### 6.5 Asset System

#### 6.5.1 Модель
- Каждый ассет идентифицируется **GUID** (хранится в `.zmeta` рядом с исходником: `rock.png` + `rock.png.zmeta`).
- `AssetRef<T>` (сильная, держит загрузку) и `SoftAssetRef<T>` (путь/GUID, грузится по запросу).
- **Asset Registry**: индекс всех ассетов проекта (тип, путь, GUID, зависимости, теги, превью), строится сканированием, кешируется, обновляется по file watcher.
- Типы: Texture, Mesh (static/skinned), Material, MaterialInstance, Skeleton, AnimationClip, AnimGraph, AudioClip, Font, Scene, Prefab, PhysicsMaterial, ParticleSystem, Script/Graph, InputActions, DataAsset (пользовательские структуры по рефлексии), Curve, Cubemap/HDRI, TerrainData, NavMesh, ShaderInclude.

#### 6.5.2 Импорт → Кукинг → Загрузка
1. **Импорт**: исходник → внутреннее представление (настройки в `.zmeta`).
2. **Кукинг** (Derived Data Cache, `Intermediate/DDC/<hash>.zasset`): ключ = хеш(исходник + настройки + версия кукера + платформа). Текстуры → BCn + mips, меши → оптимизированные буферы + LOD + кластеры + SDF, аудио → декодированный ADPCM/PCM или Vorbis-стрим.
3. **Загрузка**: асинхронно (IO thread → декомпрессия job → GPU upload copy queue), состояния `Unloaded/Loading/Loaded/Failed`, счётчик ссылок, выгрузка по LRU с бюджетом памяти.
4. **Хот-релоад**: изменение исходника → реимпорт → замена данных на месте (все `AssetRef` видят новую версию).

#### 6.5.3 Импортёры (свои)
| Формат | Что | Веха |
|---|---|---|
| PNG | все типы цвета, 8/16 бит, interlace | M3 |
| JPEG | baseline + progressive, chroma subsampling | M3 |
| TGA, BMP | базовые | M3 |
| HDR (Radiance RGBE), EXR (PIZ/ZIP, half/float) | HDRI для неба | M5 / M10 |
| DDS | BCn, кубы, массивы | M3 |
| glTF 2.0 / GLB | меши, материалы, иерархия, скины, анимации, morph targets, KHR_materials_* основные, KHR_texture_transform | M4 |
| OBJ/MTL | меши | M4 |
| FBX (binary 7.x) | меши, скелеты, анимации | M18 |
| WAV (PCM/IEEE float/ADPCM) | аудио | M9 |
| Ogg Vorbis | аудио (стриминг музыки) | M9 |
| TTF/OTF(glyf)/WOFF1 | шрифты | M6 |
| SVG (подмножество: path/circle/rect/line/polyline) | иконки | M8 |

Энкодеры: BC1/BC3/BC4/BC5 (быстрые, свои), BC7 (качество, режимы 1/6 минимум), BC6H (HDR) — M10.
Генерация mip-ов: Kaiser/box-фильтр в линейном пространстве, сохранение покрытия альфы для masked.

#### 6.5.4 Пакеты
`.zpak`: оглавление (GUID → смещение, размер, сжатие), блоки сжаты ZLZ, выравнивание 64 КБ под DirectStorage-подобную загрузку, шифрование — нет. Упаковка игры: кукинг только достижимых из стартовых сцен ассетов.

### 6.6 Scene / ECS

#### 6.6.1 ECS (ZECS)
- **Archetype-based** (как Unity DOTS / flecs): чанки по 16 КБ, SoA внутри чанка, `Entity` = 32-бит индекс + 32-бит поколение.
- Компоненты — POD-структуры с рефлексией; теги (zero-size); shared components (материал, меш — для батчинга); singletons.
- Запросы `world.Query<Transform, const Velocity>().Each(...)` + фильтры (With/Without/Changed), параллельное выполнение через JobSystem.
- Change detection (версии чанков), отложенные структурные изменения (`CommandBuffer`).
- **Системы** с фазами (PreUpdate, Update, PostUpdate, FixedUpdate, PreRender) и явными зависимостями чтения/записи → автоматический граф параллельного выполнения.
- Observers/хуки: OnAdd/OnRemove/OnSet.

#### 6.6.2 Слой GameFramework (для удобства как Unity GameObject)
- `GameObject`-подобная обёртка над Entity в редакторе и скриптах: имя, активность, слой, теги, иерархия, компоненты.
- Базовые компоненты: `Transform` (local TRS) + `WorldTransform` (кеш матрицы), `Parent/Children`, `Name`, `MeshRenderer`, `SkinnedMeshRenderer`, `Camera`, `Light` (Directional/Point/Spot/Rect/Sky), `ReflectionProbe`, `PostProcessVolume`, `Decal`, `ParticleSystem`, `RigidBody`, `Collider*`, `CharacterController`, `Joint*`, `AudioSource`, `AudioListener`, `Animator`, `Script` (ссылка на C++-компонент/ZGraph), `Terrain`, `NavMeshAgent`, `UICanvas`.
- **Префабы**: иерархия сущностей как ассет; инстансы хранят только overrides (дельта); вложенные префабы; варианты префабов; «Apply/Revert overrides».
- **Сцены**: `.zscene` (ZJSON, стабильные ID сущностей), аддитивная загрузка нескольких сцен, асинхронная загрузка.
- **World Partition** (M18): мир на ячейки по сетке, стриминг по источникам (камера/игрок), HLOD (объединённые упрощённые меши далёких ячеек), уровни данных (data layers).

### 6.7 Physics (ZPhys)
Свой физдвижок, ориентир — Jolt/PhysX по функционалу базового уровня.
- **Формы**: Sphere, Box, Capsule, Cylinder, ConvexHull (генерация Quickhull при импорте), TriangleMesh (статическая, BVH), HeightField (террейн), Compound.
- **Broadphase**: динамическое AABB-дерево (два: static/dynamic) с инкрементальными обновлениями; слои коллизий + матрица взаимодействия.
- **Narrowphase**: специализированные пары (sphere-sphere, sphere-box, capsule-capsule, box-box SAT), общий случай **GJK + EPA**, контакт-манифолды (до 4 точек, кеширование между кадрами, warm starting), mesh-vs-convex через BVH треугольников.
- **Солвер**: Sequential Impulse (PGS) с warm starting, 8 velocity + 2 position итерации (настраиваемо), Baumgarte/split impulse, трение (кулоново, 2 направления), restitution, rolling friction.
- **Острова** и сон (sleeping) по порогу энергии; параллельное решение островов через jobs.
- **CCD**: speculative contacts + опционально swept sphere для быстрых тел.
- **Джоинты**: Fixed, Hinge (лимиты, мотор), Slider, BallSocket (Cone-twist), Distance/Spring, 6DOF.
- **Character controller**: kinematic capsule, sweep & slide, ступеньки (step offset), склоны (max slope), платформы, push rigidbody.
- **Запросы**: Raycast, Shapecast (sweep), Overlap, closest point — по слоям/фильтрам, batched и потокобезопасные (read-only снапшот).
- Триггеры (события Enter/Stay/Exit), коллбеки контактов.
- Fixed timestep 60 Гц (настраиваемо), подшаги, интерполяция трансформов рендера.
- Детерминизм в пределах одной сборки/машины (порядок обработки стабилен) — для повторов и тестов.
- Позже (M19+): ragdoll (из скелета), транспорт (raycast-подвеска, колёса), ткань (XPBD), разрушаемость (Voronoi-фракции при импорте), мягкие тела.
- Debug draw: формы, контакты, AABB, острова, джоинты.

### 6.8 Animation
- `Skeleton` (иерархия костей, bind pose, inverse bind), `AnimationClip` (сжатые треки: ключи квантованы, curve fitting / ACL-подобное сжатие — свой), события/нотифаи на таймлайне, кривые (для параметров материала и т.п.).
- Сэмплинг + блендинг в local space (SIMD), layered blending с масками костей, additive-анимации.
- **AnimGraph** (как UE AnimBP / Unity Animator): State Machine (состояния, переходы по условиям/времени, blend-длительность, interrupt), Blend Space 1D/2D, ноды: Clip, Blend, Layered blend per bone, Additive, Two-bone IK, FABRIK/CCD IK, Look-at, Foot IK (raycast к земле), Modify bone; параметры (float/bool/trigger).
- **Root motion** (извлечение и передача в character controller).
- **GPU skinning** (compute, результат кешируется в буфер — используется во всех пассах + motion vectors от prev-позиций), dual quaternion — опция. Morph targets (blend shapes) в compute.
- Retargeting между скелетами (по именам/маппингу) — M18.
- Sequencer/Timeline (кат-сцены: треки трансформов, анимаций, событий, камеры, аудио) — M18.

### 6.9 Audio (ZAudio)
- Бэкенд **WASAPI** (shared mode, event-driven) или **XAudio2** — выбор в ADR; микширование — своё (float32, 48 кГц, блоки 256–512 сэмплов) в отдельном потоке.
- Источники: 2D/3D, voices с приоритетами и виртуализацией (лимит одновременно слышимых), стриминг длинных треков.
- 3D: дистанционное затухание (кривые), панорамирование (VBAP для стерео/5.1/7.1), доплер, конусы направленности, простая HRTF — опция; окклюзия raycast-ом (low-pass).
- Шины/миксер (Master, Music, SFX, UI, Voice) с громкостью, ducking (sidechain), снапшоты.
- DSP-эффекты: low/high-pass (biquad), реверб (Freeverb-подобный / FDN), задержка, компрессор/лимитер на мастере.
- Reverb-зоны (volumes).
- Декодеры: PCM/ADPCM/Vorbis (свои).

### 6.10 UI (ZUI)
Два слоя на одном 2D-рендерере.
- **Рендер 2D**: батчинг квадов/треугольников, скруглённые прямоугольники/тени/обводки через SDF в шейдере (без тесселяции), клиппинг (scissor + rounded clip), градиенты, изображения, 9-slice, векторные пути (заливка через тесселяцию — для иконок SVG).
- **Шрифты**: свой TTF/WOFF-парсер (cmap, glyf/loca, hmtx, kern/GPOS-pairs базово), растеризатор глифов (scanline с аналитическим покрытием) → атлас (динамический, с вытеснением); **MSDF**-режим для масштабируемого текста в мире. Шейпинг: базовый (кернинг, лигатуры не обязательны), UTF-8, кириллица+латиница обязательно. Хинтинг не нужен, субпиксельное позиционирование — да. DPI-масштаб.
- **Immediate-mode API (для редактора)** — в стиле Dear ImGui, но своё: `ui::Begin/End`, Button, Checkbox, Slider, DragFloat, InputText (полноценный: выделение, буфер обмена, undo внутри поля), ComboBox, TreeNode, Table (resizable columns, сортировка), Tabs, Menu/Context menu, Tooltip, Popup/Modal, ColorPicker (HSV wheel + eyedropper), CurveEditor, Splitter, ScrollArea (виртуализация длинных списков), DragDrop payload, Docking (вкладки, сплиты, плавающие окна, сохранение лэйаута), Gizmo-интеграция, иконки Lucide.
- **Retained UI для игр** (M14): `UICanvas` (screen space / world space), виджеты (Panel, Text, Image, Button, Slider, Toggle, ScrollView, InputField, ProgressBar), **флекс-лэйаут** (свой, подмножество flexbox: direction, wrap, grow/shrink, align, padding/margin/gap), якоря/привязки, стили (подмножество CSS-подобных правил в `.zstyle`), анимации свойств, навигация геймпадом, привязка данных к рефлексии. Редактор UI (WYSIWYG) — M14+.
- Локализация: строковые таблицы (`.zloc`), `LOCTEXT("ns","key","default")`.

### 6.11 Editor (ZXEditor)

#### 6.11.1 Оболочка
- Кастомный заголовок окна (лого, меню File/Edit/Assets/GameObject/Window/Tools/Help, кнопки Play/Pause/Step по центру, поиск команд).
- Тёмная тема по умолчанию (в духе UE5 / Unity 6 dark), шрифт **Inter**, иконки **Lucide**, масштаб интерфейса.
- **Докинг**: все панели перетаскиваются, табы, сплиты, плавающие окна (отдельные OS-окна), сохранение/загрузка лэйаутов, пресеты (Default, Tall, Wide, Animation).
- **Command palette** (Ctrl+P / Ctrl+Shift+P), настраиваемые хоткеи (`Config/EditorKeybindings.zcfg`).
- Project Browser на старте: создать проект из шаблона / открыть недавний.

#### 6.11.2 Панели
| Панель | Функции |
|---|---|
| **Viewport** | несколько вьюпортов, перспектива/орто (Top/Front/Side), навигация (RMB+WASD fly, Alt+LMB orbit, MMB pan, F — фокус, колесо — скорость), режимы отображения (6.4.15), сетка, привязка (snap) позиции/поворота/масштаба, **гизмо** Move/Rotate/Scale/Universal (W/E/R), local/world, пикинг (ID-буфер), box/marquee-выделение, outline выделенного (jump flood), drag&drop ассетов в сцену, иконки-билборды для света/камер/звука, превью камеры, статистика |
| **Outliner (Hierarchy)** | дерево сущностей, drag&drop перепривязка, поиск/фильтр, видимость/блокировка, мульти-выделение, переименование (F2), папки, контекстное меню (создать, дублировать Ctrl+D, удалить, префаб) |
| **Inspector (Details)** | автоматически по рефлексии: все типы полей, мета (Range, Units, Tooltip, Category, Hidden, ReadOnly, EditCondition), мульти-редактирование, добавление/удаление компонентов, кастомные редакторы для типов (`PropertyDrawer`), префаб-оверрайды (жирным, Revert) |
| **Content Browser** | дерево папок + сетка/список с превью-тумбнейлами (рендерятся в фоне), импорт (кнопка + drag&drop из проводника), создание ассетов, переименование/перемещение с обновлением ссылок (по GUID — бесплатно), поиск/фильтры по типам/тегам, Reference Viewer (зависимости) |
| **Console / Output Log** | лог с фильтрами по категориям/уровням, ввод команд/CVar с автодополнением, клик по ошибке шейдера → строка |
| **Profiler** | CPU timeline по потокам, GPU-пассы, память по тегам, счётчики, запись/сохранение сессии |
| **Project Settings / Editor Preferences** | всё по рефлексии (рендер, физика, ввод, слои, теги, сборка) |
| **Scene Settings / Lighting** | окружение, небо, туман, GI-бейк, пост-процесс по умолчанию |
| **Stats / Render Debug** | режимы буферов, граф кадра, память GPU |

#### 6.11.3 Редакторы ассетов
Material Editor (нод-граф + превью сферы/куба/меша, live-обновление), Mesh viewer (LOD, UV, коллизия, сокеты), Texture viewer (каналы, мипы, настройки импорта), AnimGraph editor, Animation clip viewer (таймлайн, нотифаи), Particle editor, Prefab editor (изолированная сцена), ZGraph script editor, Physics material, Input actions editor, UI designer (M14), Terrain tools (режим вьюпорта), Sequencer (M18).
**Нодовый редактор** — общий компонент (ноды, пины с типами, связи-кривые Безье, комменты, reroute, зум/пан, мини-карта, копи-паст, поиск нод по контексту) — используется Material/Anim/ZGraph/Particle.

#### 6.11.4 Ядро редактора
- **Undo/Redo**: транзакции на основе рефлексии (снапшот изменённых свойств/сущностей до/после), группировка (drag гизмо = одна транзакция), история в UI.
- **Selection** (сущности, ассеты), событие смены выделения.
- **Play-in-Editor (PIE)**: копия мира (сериализация → десериализация в отдельный World), Play/Pause/Step/Stop, «Simulate» (физика без игрока), изменения в PIE откатываются (с опцией «keep changes»), отдельный game-view с выбором разрешения/аспекта.
- Автосейв, восстановление после краша.
- Режим «Standalone» — запуск игры отдельным процессом.
- **Build/Package**: окно упаковки → кукинг → `.zpak` + ZXGame.exe + DLL игры в папку `Builds/`.
- Плагины редактора (поздно): API регистрации панелей/меню/импортёров.

### 6.12 Скриптинг / геймплей
1. **C++ Game Module (основное)**: игра — DLL (`MyGame.dll`), экспортирует `ZX_IMPLEMENT_GAME_MODULE`. Компоненты/системы игры регистрируются через рефлексию → видны в инспекторе. **Hot-reload**: редактор пересобирает DLL (вызов сборки CMake/Ninja), загружает копию, сериализует состояние компонентов игры, выгружает старую DLL, загружает новую, десериализует (по именам полей → переживает изменения раскладки).
   API для геймплея в стиле Unity: `class PlayerController : public ScriptComponent { void OnStart(); void OnUpdate(f32 dt); void OnCollisionEnter(const Collision&); }` + доступ к Input/Physics/Audio/Spawn/Destroy/Find/Coroutines (через свои таймеры/задачи).
2. **ZGraph** (визуальный скриптинг, как Blueprints): event-ноды (BeginPlay, Tick, Input action, Overlap), flow-контроль (Branch, Sequence, ForEach, Delay, DoOnce, Gate), переменные, функции, макросы, вызов любых рефлексированных функций (`ZX_FUNCTION`), кастом-события. Исполнение: компиляция графа в байткод для стековой VM. Отладка: брейкпоинты, подсветка активных связей, watch-значения.
3. **Текстовый скриптинг** (опционально, M20+): свой лёгкий язык «ZScript» на той же VM, если понадобится — ADR.

### 6.13 Navigation & AI
- Генерация **NavMesh** (как Recast): вокселизация геометрии сцены → фильтрация по высоте агента/шагу/наклону → регионы (watershed) → контуры → полигоны → детальный меш. Тайловый, перестраивается по тайлам в рантайме (динамические препятствия), off-mesh links (прыжки/лестницы).
- Поиск пути: A* по полигонам + string pulling (funnel). Запросы асинхронные.
- Crowd/avoidance: RVO/ORCA.
- **Behaviour Trees** (редактор нод) + Blackboard; простые сенсоры (зрение — конус + raycast, слух — события шума).
- Utility AI / State machines — через ZGraph.

### 6.14 Networking (M19, поздно)
UDP-транспорт со своим протоколом надёжности (ack-битмаски), клиент-сервер с авторитетным сервером, репликация компонентов по рефлексии (флаг `Replicated`), RPC, интерполяция/экстраполяция, client-side prediction для character controller, dedicated server build (Null RHI), сетевой профайлер. Лобби/матчмейкинг — нет.

### 6.15 Упаковка и запуск игры
- `ZXGame.exe` + `MyGame.dll` (или статически слинкованный Shipping-вариант) + `Content.zpak` + `Config/`.
- Сплэш, настройки графики игрока (по тирам + каждая опция), сохранение пользовательских настроек в `%LOCALAPPDATA%/<Game>/`.
- Система сохранений (SaveGame через рефлексию, слоты).
- Крэш-репорт (минидамп + лог в папку).

---

## 7. Свои форматы файлов (сводка)
| Расширение | Тип | Формат |
|---|---|---|
| `.zproj` | проект | ZJSON |
| `.zmeta` | GUID + настройки импорта | ZJSON |
| `.zscene` | сцена | ZJSON |
| `.zprefab` | префаб | ZJSON |
| `.zmat` / `.zmati` | материал (граф) / инстанс | ZJSON |
| `.zanimgraph`, `.zgraph`, `.zfx`, `.zbt` | графы (анимация, скрипт, частицы, behaviour tree) | ZJSON |
| `.zcfg` | конфиги | ZJSON (с наследованием Default → Project → User) |
| `.zasset` | cooked-ассет | binary: header{magic 'ZXA1', version, type, guid, deps}, chunks |
| `.zpak` | пакет | binary: TOC + сжатые блоки |
| `.zshaderlib` | скомпилированные шейдеры | binary |
| `.zloc` | локализация | ZJSON |
Спецификация каждого — в `docs/formats/` при реализации.

---

## 8. Бюджеты производительности (Tier Low, UHD 620, Development)
| Что | Бюджет |
|---|---|
| Кадр игры (CPU main) | ≤ 8 мс |
| Render thread запись команд | ≤ 6 мс |
| GPU кадр (900p вывод, 60 % render scale) | ≤ 33 мс |
| Тени (CSM 3 каскада) | ≤ 5 мс GPU |
| Пост-процесс целиком | ≤ 4 мс GPU |
| Draw calls (CPU-путь) | ≤ 2000 |
| Физика 500 активных тел | ≤ 3 мс |
| Старт редактора (тёплый кеш) | ≤ 3 с |
| Открытие сцены Sponza (тёплый DDC) | ≤ 2 с |
| Хот-релоад игровой DLL | ≤ 5 с без учёта компиляции |
| Память редактора (Sponza) | ≤ 1.5 ГБ RAM, ≤ 1 ГБ видео (общая) |

---

## 9. Тестирование
- **ZXTests.exe** — свой мини-фреймворк (`ZX_TEST(Suite, Name) { ZX_EXPECT_EQ(...) }`), запуск всех/по фильтру, вывод в консоль, ненулевой код возврата при провале.
- Unit: Core (контейнеры, строки, аллокаторы, хеши, сжатие round-trip, JSON), Math (против эталонных значений), декодеры форматов (PNG/JPEG/WAV/Vorbis/TTF на тестовых файлах), Physics (стек кубов стабилен N секунд, детерминизм, известные столкновения), ECS (запросы, структурные изменения), сериализация (round-trip), NavMesh.
- **Golden-image** тесты рендера: сэмплы рендерят фиксированный кадр → сравнение с эталоном (PSNR/порог), эталоны в `Tests/Golden/`.
- Smoke-тест редактора: запуск, открыть сэмпл, PIE 5 секунд, выход без ошибок/утечек (`ZXEditor -autotest smoke`).
- Debug layer D3D12 = 0 ошибок — обязательное условие.

---

## 10. Сэмплы (валидация вех)
| Сэмпл | Демонстрирует | Веха |
|---|---|---|
| HelloTriangle | окно + RHI + шейдер | M2 |
| SceneDemo | render graph, BCn-текстуры, debug draw, fly-камера, просмотр ресурсов графа | M3 |
| MeshViewer (по умолчанию Sponza) | асинхронная загрузка glTF/OBJ, ECS-сцена, чанки + LOD + куллинг, PBR | M4 (тени/пост — M5) |
| PhysicsPlayground | стеки, джоинты, персонаж, рейкасты | M11 |
| ThirdPersonDemo | анимированный персонаж, ввод, камера, звук, UI HUD, навигация NPC | M14+ |
| OpenWorld | террейн, растительность, стриминг, небо/time-of-day | M18 |

---

## 11. Вехи (Roadmap)

Каждая веха: цель → задачи → критерий приёмки. Порядок можно корректировать через ADR.

### M0 — Фундамент репозитория
- git init, `.gitignore`, `.clang-format`, `.editorconfig`, `CLAUDE.md`, README.
- CMake: корневой + `cmake/ZXModule.cmake` (`zx_add_module(NAME ... DEPS ...)`), пресеты, `Tools/build.ps1`.
- Пустые модули Core/Platform + exe `ZXEditor` (пока печатает версию) + `ZXTests`.
- Распаковка шрифтов/иконок в `Engine/Content`.
**Приёмка:** `Tools/build.ps1` собирает все конфиги без варнингов, `ZXTests` проходит 1 тест.

### M1 — Core
Типы, ассерты, крэш-хендлер, память (аллокаторы + трекинг), контейнеры, строки/Name/Format, математика (+SIMD), хеши, GUID, потоки + JobSystem, лог + CVars, ZJSON, бинарная сериализация, рефлексия (фаза 1), VFS + async IO, профайлер CPU, inflate/deflate.
**Приёмка:** ≥ 150 unit-тестов зелёные; бенчмарк HashMap/JobSystem в разумных рамках; 0 утечек.

### M2 — Platform + RHI D3D12 + первый кадр
Окно (DPI, resize), ввод (Raw Input, XInput), главный цикл; RHI-интерфейс; D3D12: device, queues, swapchain (flip model, tearing), дескрипторные кучи (bindless), единая root signature, буферы/текстуры, upload, PSO + кеш, командные листы, fence/кадры в полёте, отложенное удаление, debug layer, GPU timestamps; DXC-компиляция в рантайме + кеш; caps-детект.
**Приёмка:** HelloTriangle + вращающийся текстурированный куб (bindless), resize/fullscreen без ошибок debug layer, GPU-время на экране.

### M3 — Render Graph + текстуры
Render graph (transient, алиасинг, барьеры), загрузка PNG/JPEG/TGA/DDS, генерация mip-ов, BC1/3/4/5-энкодер, debug draw, камера (fly), простая forward-сцена с несколькими кубами.
**Приёмка:** сцена 1000 кубов с разными текстурами, просмотр промежуточных ресурсов графа.

### M4 — Меши, материалы, glTF, ECS
glTF/GLB + OBJ импорт, меш-оптимизация, LOD-генерация; ZECS (архетипы, запросы, системы, command buffer); Transform/иерархия; MeshRenderer; материалы PBR (без графа — фиксированный шейдер с параметрами), material instances; GPU Scene; asset system v1 (GUID, .zmeta, DDC, async загрузка).
**Приёмка:** Sponza загружается асинхронно, PBR без освещения кроме одного направленного света, 60 FPS на UHD 620 в 900p @ 70 %.

### M5 — Освещение и тени
Clustered Forward+, Point/Spot/Directional/Sky, физические единицы + экспозиция, IBL (HDR → cubemap, prefilter, SH, BRDF LUT), CSM + PCF, атлас теней point/spot, GTAO, физическое небо (Hillaire), height fog, tonemapping (ACES/AgX) + базовый grading, FXAA, TAA.
**Приёмка:** Sponza с солнцем, 50 точечных источников, тенями и небом выглядит «как в движке», бюджеты раздела 8 соблюдены.

### M6 — ZUI ядро
2D-рендер (SDF-примитивы), TTF/WOFF парсер + растеризатор + атлас, Inter + Lucide, immediate-mode виджеты базовые (кнопки, слайдеры, текст-ввод, деревья, таблицы, скролл), DPI.
**Приёмка:** демо-окно со всеми виджетами, кириллица и иконки чёткие при 100/125/150 %.

### M7 — Редактор v1
Докинг + плавающие окна, главное меню, кастомный title bar, Viewport (навигация, пикинг, гизмо W/E/R, snap, outline выделения), Outliner, Inspector по рефлексии, Content Browser (импорт drag&drop, тумбнейлы), Console/Log, сохранение/загрузка `.zscene`, Undo/Redo, Project Browser + `.zproj`.
**Приёмка:** создать проект → импортировать glTF → расставить объекты/свет → сохранить → перезапустить → всё на месте; undo/redo для всех операций.

### M8 — Префабы, PIE, материалы-графы
Префабы (overrides, вложенность), Play-in-Editor (копия мира, Play/Pause/Step/Stop), нодовый редактор (общий), Material Graph → HLSL, асинхронная компиляция пермутаций, SVG-иконки, command palette, хоткеи.
**Приёмка:** материал с текстурами и параметрами собирается в графе и обновляется в вьюпорте < 1 c; PIE не портит сцену.

### M9 — Аудио
Микшер, WASAPI/XAudio2, WAV/ADPCM/Vorbis, 3D-звук, шины, эффекты, AudioSource/Listener, превью в контент-браузере.
**Приёмка:** 64 одновременных 3D-источника без щелчков, стриминг музыки Vorbis.

### M10 — Скриптинг C++ + hot reload, качество рендера
Game module DLL, ScriptComponent API, hot-reload с сохранением состояния; Input Actions; EXR/HDR, BC6H/BC7; Reflection probes; SSR; Bloom/DOF/Motion blur/Auto exposure; Post-process volumes; ZTSR апскейл + dynamic resolution; декали.
**Приёмка:** изменение C++ кода игры применяется в PIE без перезапуска редактора; пост-процесс-волюмы блендятся.

### M11 — Физика
ZPhys целиком из раздела 6.7 (кроме «позже»), интеграция с ECS, компоненты, debug draw, PhysicsPlayground.
**Приёмка:** стек 20 кубов стабилен 60 с, 500 тел ≤ 3 мс, персонаж ходит по лестницам/склонам, рейкасты по слоям.

### M12 — GPU-driven рендер + Deferred путь
GPU culling (frustum + HiZ two-phase) + ExecuteIndirect, deferred путь, volumetric fog, кеширование теней, PCSS, контактные тени, частицы v1 (GPU).
**Приёмка:** 100k инстансов в Sponza-подобной сцене на UHD 620 без падения ниже 30 FPS (CPU не узкое место).

### M13 — Анимация
Скелеты/клипы/скиннинг GPU, AnimGraph (state machine, blend spaces, IK), root motion, morph targets, редакторы анимаций, нотифаи.
**Приёмка:** персонаж из glTF бегает/прыгает с плавными переходами и foot IK.

### M14 — Игровой UI + ZGraph
Retained UI (флекс, стили, виджеты, геймпад-навигация), UI designer, локализация; ZGraph VM + редактор + отладка.
**Приёмка:** ThirdPersonDemo: HUD + меню паузы + логика двери/пикапов на ZGraph.

### M15 — Virtual geometry ZNano
Кластеры + DAG LOD при импорте, persistent culling, visibility buffer, материальные пассы, стриминг страниц.
**Приёмка:** сцена с 1 млрд исходных треугольников (инстансами) рендерится на High-тире; на UHD 620 — работает с деградацией, без краха.

### M16 — Virtual Shadow Maps + облака + вода
VSM, объёмные облака, time-of-day, вода (Gerstner/FFT).

### M17 — GI
Лайтмаппер + light probes (GI-0), DDGI-lite на глобальном SDF (GI-1), SSGI, HW RT пути при наличии, «Lumen-lite» прототип (GI-2).
**Приёмка:** Sponza с динамическим солнцем и непрямым светом на Medium-тире, бейк лайтмапов на Low.

### M18 — Открытые миры и кино
Террейн (+ инструменты), foliage, World Partition + HLOD, Sequencer, FBX-импорт, ретаргетинг, NavMesh + AI + Behaviour Trees.
**Приёмка:** OpenWorld-сэмпл 4×4 км со стримингом без фризов > 50 мс.

### M19 — Сеть + продвинутая физика
Репликация, prediction, dedicated server; ragdoll, транспорт, ткань.

### M20 — Платформенная экспансия и полировка
Vulkan-бэкенд, HDR-вывод, плагины редактора, ZScript (если ADR одобрит), документация пользователя, шаблоны проектов, инсталлятор.

---

### M21+ — Экосистема Zeqou (после движка; приоритет — сам движок)
- **ZeqouX Hub (лаунчер)**: отдельное приложение (на ZUI движка), простое, но профессиональное и красивое.
  Скачивание/обновление версий движка, шаблонов и сэмплов, документации; список проектов, создание проекта
  из шаблона; выбор режима Lite/Full. Манифест релизов (JSON) + проверка хешей, докачка, установка в выбранную папку.
- **Документация**: два слоя — для людей (руководства, туториалы, справочник API) и для ИИ (машиночитаемые
  `llms.txt`/markdown-справочники по модулям, примеры кода, правила проекта). Генерация справочника API из рефлексии.
- **ИИ-агент в редакторе** (как в Visual Studio): чат-панель; модели через BYOK (свой ключ) или подключённые
  в настройках; **MCP-сервер движка** (инструменты: создать/открыть проект, сцены, сущности, ассеты, запуск PIE,
  сборка, логи, скриншоты вьюпорта) + инструкция для агента. Часть системы Zeqou.

## 12. Риски
| Риск | Митигация |
|---|---|
| Масштаб («UE5 с нуля») | вертикальные срезы, жёсткие критерии приёмки, «lite»-версии фич |
| Слабое железо разработки (UHD 620, 8 ГБ) | тиры, caps-детект, профилирование с M2, бюджеты раздела 8 |
| Нет aппаратного RT/mesh shaders для проверки High-путей | compute-фолбэки как основной путь; High-пути изолированы за caps; WARP для функциональной проверки |
| Ноль зависимостей → много своего кода форматов | порядок по необходимости, тестовые файлы-эталоны, сначала подмножества форматов |
| Драйвер Intel и SM 6.6 | bindless через unbounded arrays (SM 5.1+/6.0) как базовый путь |
| Долгая компиляция C++ | PCH, модульные статические либы, Ninja, минимум инклюдов в Public-заголовках |
| Hot-reload нестабилен | сериализация состояния по рефлексии, а не memcpy; фолбэк — перезапуск PIE |

---

## 13. Порядок работы агента (Claude) с этим ТЗ
1. Перед задачей — найти веху и раздел подсистемы; работать **только в рамках текущей вехи**, не забегать вперёд.
2. Маленькие проверяемые шаги: написать → собрать → запустить тест/сэмпл → отметить чекбокс.
3. Архитектурное решение, не описанное здесь, → ADR в разделе 15 (номер, дата, решение, причина).
4. Расхождение кода и ТЗ → правим ТЗ (с ADR) или код, но не оставляем расхождение.
5. Коммиты — только по просьбе пользователя.
6. Перед завершением вехи — пройти критерий приёмки и DoD (3.4).

---

## 14. Трекер прогресса

### M0 — Фундамент
- [x] git init + .gitignore/.clang-format/.editorconfig
- [x] CMake (корень, ZXModule.cmake, пресеты) + Tools/build.ps1
- [x] Модули-заглушки Core/Platform, exe ZXEditor, ZXTests
- [x] Распаковка Inter (TTF) и Lucide (TTF + codepoints + SVG) в Engine/Content
- [x] Приёмка M0 (2026-10-11: все 3 конфигурации без варнингов, тесты зелёные)

### M1 — Core
- [x] Types/Assert/Result/крэш-хендлер (SEH → стектрейс + минидамп, `Result<T>` + `ZX_TRY`)
- [x] Аллокаторы + трекинг памяти (теги, утечки с колстеками в Debug, `LeakIgnoreScope`, Linear/Pool/VirtualArena, OffsetAllocator-TLSF)
- [x] Контейнеры (Array/SmallArray, HashMap/HashSet (Robin Hood), SlotMap, Span, FixedArray, RingBuffer, BitArray, Optional, Function, MulticastDelegate, SPSC/MPMC очереди, Sort/StableSort)
- [x] String/StringView/Name/Format/Path/UTF/Parse
- [x] Математика + SSE + пересечения (Vec/Mat/Quat/Transform, reversed-Z проекции, Frustum, AABB/OBB/Sphere/Plane/Ray, цвет, шумы, кривые, упаковка half/oct/R11G11B10/RGB9E5)
- [x] Хеши (xxHash64, CRC32, Adler32, FNV, SHA-1), GUID v4/v5, inflate/deflate (сверено с zlib), ZLZ
- [x] Threads + JobSystem (зависимости, приоритеты, ParallelFor, ожидающий поток выполняет задачи)
- [x] Log + CVars + ConCommands + CommandLine
- [x] Рефлексия фаза 1 (структуры, наследование, enum/flags, Array<T>, методы с вызовом, метаданные)
- [x] ZJSON + бинарная сериализация + сериализация по рефлексии (дельта от дефолтов, переименования полей)
- [x] VFS (точки монтирования, оверлеи) + async IO + DirectoryWatcher
- [x] CPU-профайлер + экспорт Chrome Tracing
- [x] Приёмка M1 (2026-10-11): 155 unit-тестов зелёные в Debug/Development/Shipping, 0 утечек (проверка утечек встроена в прогон
  тестов и в выход ZXEditor). Бенчмарки (Development, i5-10310U): HashMap<u32> find 1M случайных ключей ~240 нс (упор в DRAM),
  в кэше ~20 нс; задача job system ~1.2 мкс (schedule+run); ParallelFor ×2.4 на 4C/8T.

### M2 — Platform + RHI
- [x] Окно (DPI, resize, fullscreen), цикл событий
- [x] Raw Input, XInput, курсор
- [~] RHI интерфейс (готов); Null RHI — отложен (тесты идут на WARP)
- [x] D3D12: device/queues/swapchain/fences/frames in flight
- [x] Bindless дескрипторы + root signature
- [~] Буферы/текстуры/upload (готово, committed-ресурсы); суб-аллокатор heap-ов — отложен
- [~] PSO (готово) + кеш PSO (ID3D12PipelineLibrary) — отложен; DXC рантайм-компиляция + дисковый кеш — готово
- [x] GPU timestamps, PIX-маркеры, DRED
- [x] Caps-детект (UHD 620: SM 6.5, binding tier 3, waves 16, без RT/mesh)
- [x] HelloTriangle + текстурированные кубы (vertex pulling, bindless, mips, depth)
- [~] Приёмка M2: 159 тестов (вкл. GPU-тесты на WARP), HelloTriangle на UHD 620 — fullscreen/resize без ошибок debug layer,
  0 утечек, скриншот через `-screenshot`. Осталось: GPU-время выводится в заголовок окна (текст на экране — после ZUI), и 3 отложенных пункта выше.

### M3 — Render Graph + текстуры
- [x] Render graph: транзиентные ресурсы, отсечение неиспользуемых проходов, алиасинг (переиспользование пула в кадре + межкадровый пул), автоматические барьеры, `Dump()` графа
- [x] Кодеки (свои, Core/Image): PNG (все типы цвета, 1–16 бит, Adam7, tRNS) + PNG-энкодер, JPEG (baseline + progressive, restart, 4:2:0/4:2:2/4:4:4, fancy upsampling), TGA (RLE), BMP (палитра/RLE4/RLE8/16/24/32, BITFIELDS, V5), HDR (RGBE), DDS (BCn, DX10)
- [x] Mip-ы (Box/Kaiser в линейном пространстве, сохранение alpha coverage, ренормализация нормалей)
- [x] BC1/3/4/5-энкодер (+ декодер для тестов), параллельный
- [x] TextureLoader (Auto: BC1/BC3/BC5, fallback RGBA8 при размерах не кратных 4)
- [x] Debug draw (линии/боксы/сферы/сетка/оси/фрустум/стрелки, depth-tested + overlay)
- [x] Модуль Engine: FlyCamera (ПКМ + WASD/QE, Shift)
- [x] EngineProfile Lite/Full (авто по железу, `-lite`/`-full`, CVar `engine.Profile`); в Lite сцена рендерится в 75 % разрешения
- [x] SceneDemo: 1000 кубов, 13 текстур (12 процедурных BC1 + JPEG), граф Opaque → DebugDraw → Composite (ACES)
- [x] Приёмка M3 (2026-10-11): SceneDemo на UHD 620, 0 ошибок debug layer, 0 утечек. Просмотр промежуточных ресурсов графа:
  F2 — финал / сырой HDR / глубина / тепловая карта яркости, F4 — дамп графа (проходы, ресурсы, время жизни, алиасинг).
  Development, `-novsync`, 1920×1080: Lite (1440×810) — CPU-работа 2.0 мс, GPU 8.5–10 мс; Full — CPU 2.2 мс, GPU 14–15 мс.
  Тесты: 171 (Debug/Development), Shipping — 170 (assert-тесты выключены).

### M4 — Меши, материалы, glTF, ECS
- [x] ZECS (модуль Scene): архетипы, чанки 16 КБ (SoA), Entity = индекс + поколение, теги, синглтоны, запросы
  `Query<T, const U>().With/Without/Changed().Each/ParallelEach/EachChunk`, change detection по версиям чанков,
  CommandBuffer (отложенные структурные изменения, placeholder-сущности), наблюдатели OnAdd/OnRemove/OnSet,
  системы с фазами и Reads/Writes/After → автоматические параллельные батчи на job system
- [~] Shared components — отложены (батчинг решается сортировкой draw-листа по материалу)
- [x] Transform/иерархия: `zx::Transform` как локальный компонент, WorldTransform, Parent/Children (хуки держат связи
  консистентными), SetParent с сохранением мировой позы, DestroyHierarchy, параллельная пропагация по корням
- [x] Импорт glTF 2.0 (.gltf/.glb, data URI, sparse, strided, strips/fans, KHR_texture_transform/emissive_strength/unlit,
  иерархия узлов) и OBJ/MTL (полигоны, отрицательные индексы, PBR-расширения MTL) → правая СК переводится в левую
  (зеркало X + разворот winding; проверено на всех моделях: нормали совпадают с геометрией на 100 %)
- [x] Обработка мешей: генерация нормалей (угловые веса)/касательных, сварка, вырожденные треугольники,
  vertex cache (Forsyth, ACMR 2.6 → 0.7) + vertex fetch, QEM-упрощение (швы, границы, флипы), разбиение больших секций
  на пространственные чанки, независимые LOD-цепочки секций с запертыми границами (без трещин), пороги переключения по
  ошибке ≤ 1 px
- [x] Asset system v1 (модуль Asset): GUID в `.zmeta`, подассеты (меш/материал/текстура/префаб), кукинг в DDC
  (`Intermediate/DDC`, ключ = хеш исходника + настроек + CookerVersion), текстуры ключуются по своему содержимому,
  асинхронная загрузка на job system, повторный запуск — только чтение кэша
- [x] Кукнутые форматы: меш (вершина 36 байт: позиция f32, нормаль/касательная oct16, UV0 f32, UV1 half, цвет),
  текстура (BCn + mips, лимит размера для Lite), материал, префаб; бинарный контейнер с версией
- [x] Рендер: RenderAssets (стриминг на GPU с бюджетом байт/кадр, таблица материалов bindless), SceneRenderer
  (куллинг и LOD по чанкам, depth pre-pass, opaque/masked/translucent, сортировка), Forward PBR (GGX + Smith + Schlick,
  normal/occlusion/emissive, alpha test, двусторонние), GPU-инстансы (ByteAddressBuffer)
- [x] Engine: компонент MeshRenderer, InstantiatePrefab, GatherRenderInstances
- [~] Material instances (переопределение параметров) — после редактора материалов (M7/M8); GPU Scene с dirty-трекингом —
  сейчас инстанс-буфер пересобирается каждый кадр (дёшево до тысяч инстансов)
- [x] Приёмка M4 (2026-10-11): Sponza (262k треугольников, 69 текстур, 25 материалов) грузится асинхронно: первый
  кукинг ≈ 18 с, из DDC — префаб за 60 мс, всё резидентно за ≈ 2 с. UHD 620, 1600×900, рендер 70 % (Lite): стабильные
  60 FPS с vsync (кадр 16.7 мс, GPU 7–11 мс; после чанков рисуется ≈ 126k треугольников), 0 ошибок debug layer.
  Тесты: 201 (Debug/Development), Shipping — 200.

### M5 — Освещение и тени
- [x] CSM: до 4 каскадов, сферы срезов фрустума (стабильные) + texel snapping, PCF 4/9 тапов, кросс-фейд каскадов,
  normal offset + slope bias, LOD теней по разрешению каскада, отсев мелких кастеров; **кэш статических теней** (каскад
  перерисовывается, только когда срез вышел за его расширенную сферу; ≤ 2 каскада за кадр) → в статике 0 мс, в полёте ≈ 0.3 мс
- [x] Clustered Forward+: сетка 16×9×24 (экспоненциальные срезы), кластеризация в compute, до 63 источников на кластер,
  point/spot (оконное обратно-квадратичное затухание), отсев невидимых при текущей экспозиции; компоненты PointLight/SpotLight
- [x] Физические единицы: люкс (солнце), люмены/кандела (лампы), кд/м² (небо), EV100 + pre-exposure (HDR-цель в разумном диапазоне)
- [x] Физическое небо (Hillaire 2020): LUT пропускания и многократного рассеяния (один раз), sky-view LUT (каждый кадр),
  солнечный диск с потемнением к краю, цвет солнечного света на земле из той же модели (CPU) → закаты
- [x] Sky IBL: проекция неба в SH9 (compute), диффузный и упрощённый спекулярный ambient (SH по отражению + EnvBRDF),
  SH читается как root CBV
- [x] Пост: гистограмма яркости + автоэкспозиция (readback → pre-exposure следующих кадров), bloom (dual filter),
  тонмаппинг AgX/ACES + грейдинг (контраст, насыщенность, баланс белого в Кельвинах, виньетка), FXAA (3.11-подобный) + апскейл
- [x] GTAO по глубине pre-pass (нормали восстанавливаются из глубины), XeGTAO-подобный falloff, depth-aware blur, multi-bounce
  + specular occlusion в forward; Lite — половинное разрешение, 2×4, Full — полное, 3×6
- [x] Экспоненциальный высотный туман (аналитический интеграл), in-scattering = SH неба + солнце (Хеньи–Гринстейн)
- [x] TAA: джиттер Halton(2,3), репроекция по глубине (камера; per-object velocity — с динамическими объектами), variance
  clipping в YCoCg, Catmull-Rom (Full) / билинейная (Lite) история, Karis-взвешивание, учёт смены экспозиции; FXAA — при TAA выкл.
- [x] Атлас теней point/spot: постоянный D32 2048² (плитки 256), point = 6 граней, приоритет — ближайшие, кэш плиток по
  стабильному id источника, ≤ 12 перерисовок граней за кадр, PCF 4 тапа; LightDesc.castShadows / PointLight.castShadows
- [x] Кэш sky-view LUT и SH (пересчёт только при смене солнца/высоты камеры); тонмап пишет сразу в back buffer без FXAA
- [x] Префильтрованный кубмап для спекуляра — сделан в M10 (0.9.3) вместе с reflection probes
- [x] Приёмка M5 (2026-10-12): Sponza, UHD 620, 1600×900, рендер 70 % (Lite), vsync, 0 ошибок debug layer:
  день (солнце, CSM, небо, туман, GTAO, TAA, bloom, автоэкспозиция) — GPU ≈ 13.7 мс, 60 FPS;
  ночь (50 фонарей, 4 с тенями в атласе) — GPU ≈ 27 мс (forward ≈ 17.5 мс; бюджет раздела 8 — 33 мс).
  CSM в статике 0 мс (кэш), пост (гистограмма + bloom + тонмап) ≈ 2.4 мс, TAA ≈ 1.8 мс. Тесты: 201/201/200.

### M6 — ZUI ядро
- [x] Модуль `UI` (зависит от Platform + Render). Шрифты: свой парсер TrueType/WOFF 1.0 (cmap 4/12, glyf/loca с составными
  глифами, hmtx, kern + GPOS PairPos форматов 1/2 и extension-лукапы), защита от битых файлов (все чтения с проверкой границ)
- [x] Растеризатор путей с точным аналитическим покрытием (signed area accumulation), квадратичные/кубические кривые;
  глифы растеризуются в точном пиксельном размере с 4 субпиксельными фазами по X, без хинтинга
- [x] FontSystem: цепочки фолбэков (Inter latin / latin-ext / cyrillic / cyrillic-ext / greek), веса 400/500/600/700 + italic,
  Lucide как иконочный шрифт (PUA, макросы `ZX_ICON_*` в UI/Icons.h, генератор Tools/gen_lucide_icons.py), кернинг,
  измерение/перенос по словам/hit-test; динамический атлас R8 4×1024² (полки, вытеснение страниц по LRU)
- [x] 2D-рендер: DrawList примитивов на SDF (скруглённые прямоугольники с разными радиусами, обводки, градиенты, мягкие тени,
  AA-линии/кривые Безье, треугольники/выпуклые полигоны, глифы, изображения), клип прямоугольником + скруглённый клип;
  всё — один инстансированный draw call (64 Б на примитив, vertex pulling); квады обрезаются по клипу в VS,
  рамки и тени рисуются «кольцом» (без невидимой середины); UIRenderer (дозагрузка изменённых строк атласа через
  новый `CommandList::UploadTextureRegion`, проход для render graph)
- [x] Immediate-mode API `ui::`: окна (перетаскивание, ресайз, сворачивание, z-порядок, скролл колесом и полосой),
  child-области, раскладка (SameLine, Indent, группы, AlignTextToFramePadding), ID-стек ("Метка##id"), шрифты/цвета/disabled;
  виджеты: Text/Wrapped/Heading/Bullet/LabelText/Link, Button (4 варианта), IconButton, Checkbox, Toggle, Radio, Selectable,
  ProgressBar, Spinner, SliderFloat/Int, DragFloat/2/3/Int (Ctrl+клик/двойной клик — ввод числа), InputText (выделение
  мышью и Shift, слова по Ctrl, буфер обмена, undo/redo, пароль, подсказка), InputTextMultiline, InputFloat/Int, Combo,
  ColorEdit/ColorPicker (SV-квадрат, оттенок, альфа, hex), TreeNode/CollapsingHeader, вкладки (закрываемые), таблицы
  (изменяемые колонки, сортировка, фон строк, ScrollY), ListClipper (виртуализация), всплывающие окна, модальные окна,
  контекстные меню, главное меню + подменю, подсказки, drag & drop, Splitter, Image; тёмная и светлая темы
- [x] DPI: все размеры API в логических пикселях × (DPI окна × пользовательский масштаб); шрифты растеризуются под итоговый размер
- [x] Тесты: парсер (метрики, кириллица, составные глифы, кернинг, WOFF = TTF, мусор/обрезанные файлы), растеризатор (точная
  площадь, отверстия, субпиксель), FontSystem (шейпинг, перенос, hit-test, атлас и вытеснение), DrawList; headless-тесты
  взаимодействия (клик, флажок, слайдер, драг, редактирование текста с undo/Escape, дерево, popup, перемещение окна, масштаб,
  сортировка таблицы + клиппер на 100k строк, drag & drop, ID)
- [x] Сэмпл UIDemo (все виджеты, 10 000 строк в таблице, сценарии скриншотов `-scenario=`), оверлей ZUI в MeshViewer
  (статистика, GPU по проходам, переключатели рендера) — закрыт хвост M2 «GPU-время на экране»
- [~] MSDF для текста в мире, 9-slice, векторные пути/SVG — перенесены: SVG-иконки в M8, MSDF/9-slice в M14 (игровой UI)
- [x] Приёмка M6 (2026-10-12): демо-окно со всеми виджетами, кириллица и иконки чёткие при 100/125/150 % (скриншоты UIDemo),
  0 ошибок debug layer, без утечек. Стоимость: сборка UI на CPU ≈ 0.6 мс; GPU — по площади (UHD 620 ограничен заливкой):
  оверлей MeshViewer ≈ 0.37 мс, полноэкранное демо (5 окон, перекрытие ≈ 1.6×) ≈ 3 мс на полной частоте GPU. Тесты: 221/221/220.

### M7 — Редактор v1 (закрыта 2026-10-12)
- [x] Язык интерфейса движка, редактора и сэмплов — английский (решение пользователя 2026-10-12); Unicode/кириллица поддерживаются в данных
- [x] Нейтральная графитовая тема в духе UE5/Unity 6; оболочка ZXEditor: свой title bar (логотип, меню File/Edit/Create/Window/Help,
  заголовок сцены, кнопки окна; клики по кнопкам обрабатывает платформа, snap layouts Win11 работают), главный тулбар
  (Select/Move/Rotate/Scale, local/world, snap + шаг, сетка, Play/Pause/Step по центру, бейдж профиля), статус-бар (последнее
  сообщение лога, ошибки/предупреждения, сущности, FPS, GPU)
- [x] Докинг ZUI: дерево сплитов + вкладки, перетаскивание вкладки наружу → плавающее окно, бросок окна на цели (центр/4 стороны),
  сплиттеры, скрытие пустых узлов, раскладка по умолчанию, сохранение/загрузка (Saved/Config/EditorLayout.ini, `-resetlayout`)
- [x] ViewportRenderer (Engine): общий рендер вида (сцена, небо, туман, TAA, пост) для редактора и сэмплов; вьюпорт рендерится
  в свою текстуру под размер панели; мини-тулбар вьюпорта (режимы отладки, Show-флаги, солнце, скорость камеры, FPS)
- [x] Навигация (ПКМ + WASD/QE, колесо), пикинг кликом (секции мешей в пространстве меша + иконки света), гизмо Move/Rotate/Scale
  (рисуются в 2D через ZUI; оси, кольца, равномерный масштаб, плоскость экрана; snap; local/world), рамка выделенного меша, иконки света
- [x] Outliner (дерево, поиск, drag & drop перепривязка, видимость, контекстное меню), Details по рефлексии (Add/Remove/Reset
  компонентов, Vec/Quat-как-углы/цвет/диапазоны/единицы/углы/enum/строки/массивы/ассеты), реестр компонентов (Engine/ComponentRegistry),
  Content Browser (дерево папок, плитки по типам, хлебные крошки, поиск, импорт через диалог, двойной клик, drag & drop во вьюпорт),
  Output Log (уровни, счётчики, поиск, автопрокрутка, виртуализация), Statistics, About
- [x] `.zscene` (Engine/SceneSerialization): JSON по рефлексии, компоненты из ComponentRegistry дельтой от умолчаний, EntityGuid,
  иерархия в порядке обхода, список исходников ассетов (относительно Content), настройки окружения/камеры редактора;
  File → New/Open/Save/Save As (Ctrl+N/O/S/Shift+S), диалог «Save Changes?» при закрытии/смене сцены, `-scene=*.zscene`
- [x] Undo/Redo (Engine/SceneUndo): транзакции из снимков сущностей до/после (по EntityGuid); покрывают создание, удаление
  (с поддеревом и позицией среди соседей), дублирование иерархии, перепривязку, гизмо, правки в Details (одна транзакция на
  перетаскивание/ввод), Add/Remove/Reset компонента, видимость, импорт; Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z, Edit → Undo History,
  «грязность» сцены = позиция в истории; `-undotest` прогоняет все операции через редактор
- [x] Проекты (Engine/Project): `<Name>.zproj` (имя, версия движка, стартовая сцена), Content/ + Content/Scenes + Saved/,
  список недавних проектов в %LOCALAPPDATA%/ZeqouX3D/RecentProjects.json (общий с будущим ZeqouX Hub); Project Browser
  (карточки недавних проектов с поиском и контекстным меню, «New Project» с шаблонами Blank / Sample Scene, выбор папки,
  валидация имени, «Open Other...»), File → Open Project...; стартовая сцена = последняя сохранённая/открытая; импорт извне
  копирует исходник с зависимостями (буферы/текстуры glTF, mtl/текстуры OBJ) в Content/<Имя>/; флаги `-project=`,
  `-newproject=<папка/Имя> [-template=sample]`, `-projectbrowser [-browserpage=new]`, `-recentfile=`; Samples/Samples.zproj
- [x] Исправлено: ключ DDC не учитывал GUID/путь исходника — копия того же файла получала чужой манифест и не загружалась
- [x] Outline выделения (Render/SelectionOutline): маска выделенных мешей (R = видимо, G = покрыто) с проверкой по глубине
  сцены, мягкая обводка по двум кольцам выборок поверх финального кадра, скрытые части тусклее; ножницы по экранному
  прямоугольнику выделения (0.15 мс на UHD 620 вместо 1.5 мс полноэкранно); рамка-AABB выделения убрана
- [x] Тумбнейлы Content Browser (ZXEditor/Thumbnails): 128² sRGB-текстуры по запросу видимых плиток; изображения — blit
  с aspect-fit/шахматкой/linear→sRGB, модели — отдельный ViewportRenderer (без TAA, фиксированная экспозиция) в своём мире,
  кадрирование по bounds; не больше одной картинки и одной модели за кадр
- [x] Console: строка ввода в Output Log (Enter — выполнить, Tab — дополнить, ↑/↓ — подсказки/история, список подсказок со
  значениями CVar и справкой), `help [префикс]` в Core, CVars редактора (r.ScreenPercentage, r.TAA, r.AO, r.Shadows, r.Sky,
  r.Bloom, r.ExposureCompensation, r.ViewMode, r.ForceLOD, editor.CameraSpeed, editor.ShowGrid) и команды (clear, quit,
  stat.gpu, stat.scene, undo, redo, scene.new/open/save, select, focus, import, create, editor.ResetLayout, project.info);
  `-exec="cmd; cmd"`; ZUI: `InputTextFlags::KeepFocusOnEnter`, `SetActiveTextCursorToEnd()`
- [x] Приёмка M7 (Debug, 0 утечек/ошибок D3D12): новый проект → `import` glTF извне (копия в Content/DamagedHelmet) →
  лампа → undo/redo → сохранение → перезапуск с `-project` → сцена восстановлена (3 сущности, модель, свет, камера);
  `-undotest` на этом проекте и на Samples — все 6 видов операций; 229 тестов
- Перенесено: панели в отдельных OS-окнах (multi-viewport) — бэклог (ADR 050)

### M8 — Префабы, PIE, материалы-графы (закрыта 2026-10-12)
- [x] Play-in-Editor: снимок мира (.zscene в память) на Play, игра идёт в том же World, Stop восстанавливает снимок
  (те же guid → история Undo валидна; запись Undo на время PIE приостановлена); Play/Stop F5/Esc, Pause F6, Step F10,
  кнопки тулбара, команды play/stop/pause/step; зелёная/янтарная рамка вьюпорта + плашка, «[Playing]» в заголовке;
  Save/New/Open сначала останавливают PIE; компоненты поведения Rotator и Oscillator (Engine/Gameplay, TickGameplay);
  `-pietest` (анимация идёт, удаление/создание в PIE не переживают Stop, история цела)
- [x] Префабы `.zprefab` (Engine/Prefab): PrefabInstance на корне + PrefabLink{instance, source} на участниках,
  детерминированные guid участников (FromName(instance, source)); сцена хранит инстанс как overrides (поля компонентов по
  рефлексии, removedComponents, removedFromPrefab по source-id на корне своего инстанса); вложенные префабы на любую
  глубину; Create Prefab (Outliner, prefab.create), Apply (overrides снимаются относительно старой версии и
  переносятся на новую для всех инстансов), Revert (остаётся только размещение корня), Unpack, Select Asset; плашка
  префаба в Details с числом overrides, изменённые поля жирным с маркером; синие инстансы в Outliner; .zprefab в
  Content Browser (двойной клик / drag & drop во вьюпорт); всё через Undo; тесты Prefab.* (round trip, apply/revert,
  вложенность, удаления на вложенном уровне). Варианты префабов — позже
- [x] Нодовый редактор (ZXEditor/MaterialEditor): холст с сеткой, панорамой (ПКМ/СКМ) и зумом к курсору, ноды с цветными
  заголовками по категориям и превью (текстура/цвет), пины по размерности, связи-кривые Безье, выделение рамкой и Ctrl,
  перетаскивание связи (в том числе с готового входа), Alt+клик — разрыв, отпускание связи в пустоте — меню нод с
  автоподключением, поиск нод, копирование/вставка/дублирование, удаление, F — показать всё, своя история Undo/Redo
- [x] Material Graph `.zmat` (Render/MaterialGraph): 34 типа нод (Output, Scalar/Color-параметры, Texture Sample с normal map,
  TexCoord, Time, Vertex Color, World Position, Camera Vector, Vertex Normal, Panner, Fresnel, Desaturate, Component Mask,
  Append, арифметика/Lerp/Power/Clamp/Saturate/тригонометрия/Dot/Normalize/Length), вывод размерностей с приведением
  типов, генерация `EvaluateGraphMaterial` (Shaders/Material/MaterialGraph.hlsli) → ForwardPBR с ZX_GRAPH_MATERIAL;
  все значения и текстуры — в буфере параметров материала (правка значения без перекомпиляции, 0.1–0.2 мс); ошибки по
  нодам (подсветка), превью на сфере/кубе/цилиндре/плоскости с вращением, Blend/Two Sided/Unlit/Cutoff
- [x] Инстансы `.zmati`: родитель + переопределение именованных параметров, общий шейдер; изменения родителя расходятся
  по инстансам; редактор инстанса (галочки override, предпросмотр)
- [x] Асинхронная компиляция (Render/MaterialShaderCache): job system, кэш по хэшу кода, только нужные варианты (main или
  alpha test + depth); пока шейдер компилируется — нейтральная строка стандартного шейдера; структурная правка
  доходит до вьюпорта за 240–310 мс на UHD 620
- [x] Материалы в редакторе: Content Browser (New Material/Folder, Create Material Instance, миниатюры-сферы, двойной
  клик), назначение перетаскиванием на меш во вьюпорте или в слот Details (имена ассетов вместо GUID, кнопка Edit),
  консоль material.*; встроенные фигуры Cube/Sphere/Cylinder/Plane (Engine/BuiltinAssets, меши в памяти AssetManager),
  Create → Shapes
- [x] Command palette (Ctrl+P — команды, ассеты, сущности; Ctrl+Shift+P — только команды; нечёткий поиск) и реестр
  действий (ZXEditor/EditorActions): меню и горячие клавиши из одного списка, Saved/Config/Keys.ini (до двух сочетаний,
  создаётся с умолчаниями); ZUI: SetKeyboardFocusHere, модальные окна без заголовка
- [x] SVG-иконки (UI/Svg): парсер подмножества (path со всеми командами и дугами, circle/ellipse/rect rx/line/polyline/
  polygon, g с наследованием fill/stroke, transform), обводка как объединение квадов и кругов, кэш в атласе шрифтов
  (FontSystem::GetCustomGlyph), DrawList::AddSvg / ui::SvgIcon; логотип редактора — Engine/Content/Icons/ZeqouLogo.svg
- [x] Приёмка M8 (Debug, 0 утечек/ошибок D3D12): `-materialtest` — материал с текстурой и параметрами собирается из нод,
  структурная правка видна через 241–305 мс, правка значения — без перекомпиляции; `-pietest` — PIE не портит сцену;
  `-undotest` на проекте с префабами и материалами; 237 тестов

### M8.x — Пакет запросов пользователя (версии 0.8.1+, в работе)
- [x] Версия = закрытые вехи (0.8.x), единый источник (`project(VERSION)`), ресурс версии и иконка в exe, docs/CHANGELOG.md
- [x] Иконка приложения в стиле экосистемы Zeqou (`Tools/gen_app_icon.py` → .ico/.png/.svg), логотип в редакторе
- [x] Project Browser: плитки «первая буква на случайном фоне» (цвет хранится в .zproj), нарисованные превью шаблонов (0.8.3)
- [x] Выбор языка при создании проекта: ZeqouScript (новый язык движка), C++ (с 0.9.1); Visual Script (M14) показан
  недоступным (0.8.3)
- [x] Starter Content: материалы с параметрами + скрипты, витрина в пустом проекте (0.8.3)
- [x] Тусклое небо: причина — тонмаппер AgX по умолчанию, переключено на ACES (0.8.5)
- [x] ZeqouScript: лексер, парсер, байткод-VM, компонент Script со свойствами в Details, hot reload, запуск в PIE,
  встроенный редактор скриптов (подсветка, ошибки на лету); документация docs/ZeqouScript.md (0.8.2)
- [x] (0.8.4) Игровой рантайм для шаблонов: Camera, захват ввода в PIE, CharacterController (простые коллизии до M11)
- [x] Иконка по артам пользователя (знак + логотип с надписью), импорт `Tools/import_brand_icons.py` (0.8.4)
- [ ] Шаблоны: Blank, Sample Scene, First Person, Third Person (0.8.4), Water (0.8.5) — все готовы; опция «Starter Content» — настраиваемые
  примеры (контроллеры FPS/TPS, вода, материалы, префабы, скрипты)
- [x] Реалистичная вода (волны Герстнера, преломление, поглощение по глубине, френель, пена) (0.8.5)
- [x] Окно Features: включение/выключение подсистем и инструментов (хранится в .zproj) (0.8.6)
- [x] Инструменты: Brush (рисование несколькими моделями: радиус, плотность, разброс масштаба/поворота, выравнивание
  по нормали, максимальный уклон, стирание), Measure, Drop to Ground; рейкаст по треугольникам мешей

- [x] New Project: Documentation (README, Docs/, AGENTS.md / CLAUDE.md для ИИ) и Git repository (0.8.6)

### M9 — Аудио (закрыта, 0.9.0)
- [x] Модуль Audio (ZAudio): свой микшер float32 48 кГц стерео в потоке TimeCritical + MMCSS «Pro Audio», блоки 256
- [x] Вывод WASAPI shared event-driven (AUTOCONVERTPCM), переоткрытие при смене/потере устройства, счётчик underrun;
  Null-бэкенд для тестов и офлайн-рендера
- [x] Декодеры свои: WAV (PCM 8/16/24/32, float 32/64, IMA ADPCM, MS ADPCM), Ogg Vorbis (кодбуки Хаффман+VQ, floor 1,
  residue 0/1/2, coupling, IMDCT через FFT, overlap-add, обрезка по granule) — совпадает с libvorbis до шага 16 бит
- [x] Стриминг: длинные Vorbis-клипы декодируются при воспроизведении (луп без шва), короткие — при загрузке
- [x] Голоса: интерполяция Эрмита (питч/ресемплинг), рампы громкости/панорамы/фильтра на блок, fade-in/stop без щелчков,
  приоритеты, кража голосов, виртуализация сверх лимита слышимых (64)
- [x] 3D: затухание Inverse/Linear/Logarithmic, панорама равной мощности, конусы, доплер, spread, окклюзия рейкастом
  (low-pass + ослабление)
- [x] Шины Master/Music/SFX/UI/Voice: громкость, mute, пиковые метры, ducking музыки под голосом, снапшоты с плавным
  переходом; реверб-send (Freeverb), лимитер на мастере; DSP: biquad, Freeverb, delay, компрессор, лимитер
- [x] Компоненты AudioSource / AudioListener / ReverbZone, AudioSystem в PIE (слушатель — игровая/редакторская камера)
- [x] ZeqouScript: `audio.play/playAt/stop/setVolume`, `entity.playSound/stopSound`
- [x] Редактор: превью в Content Browser, слот клипа в Details, окно Audio Mixer, иконки и радиусы во вьюпорте,
  Create > Audio Source / Reverb Zone, консоль `audio.play`, `audio.stats`, `mixer`; пауза PIE глушит звук
- [x] Starter Content: синтезированные звуки (клик, колокольчик, бесшовные лупы гула/ветра/моря); звук в шаблонах
- [x] Приёмка: 64 одновременных 3D-источника без щелчков (тест + `-audiotest` на реальном устройстве: 0 underrun,
  0.3 мс на блок), стриминг музыки Vorbis (тест)

### M10 — Скриптинг C++ + hot reload, качество рендера (закрыта, 0.10.0)
- [x] Game module DLL проекта: `Source/<Name>` → `Binaries/<Name>.dll`, сборка в фоне (`Tools/build_game.ps1`, MSVC +
  CMake + Ninja, кеш окружения vcvars), загрузка копии DLL (исходная не блокируется), ошибки компилятора в Output Log
- [x] Game SDK (`GameSDK/ZeqouGame.h`, без зависимостей от движка): `ScriptComponent` (OnStart/OnUpdate/OnStop/OnReloaded),
  Host-интерфейс (сущности, ввод, время, персонаж, звук), рефлексия props макросами `ZX_GAME_COMPONENT` / `ZX_GAME_PROP`
- [x] Hot reload с сохранением состояния: props переносятся по имени, работающие компоненты пересоздаются в PIE без
  остановки игры; компонент C++ Script в Details; Compile (F7) в тулбаре
- [x] Input Actions: кнопки и оси с привязками клавиатура/мышь/геймпад, окно редактора, `Config/Input.json`,
  ZeqouScript `input.action/axis`, C++ `host->ActionDown/Axis`
- [x] Язык C++ при создании проекта, Create C++ Game Module, документация `Docs/CppGameCode.md`
- [x] Приёмка (часть 1): изменение C++ кода игры применяется в PIE без перезапуска редактора (`-cpptest`)
- [x] Post-process volumes (бленд по приоритету, весу и радиусу), DOF (физический CoC, боке на полуразрешении),
  motion blur камеры (репроекция глубины); per-object velocity — вместе со скиннингом (M13)
- [x] Reflection probes (префильтрованный GGX-кубмап, захват по грани за кадр, box projection, бленд), кубмап неба
  для спекуляра, SSR (полуразрешение, используется следующим кадром)
- [x] Декали (forward, до освещения: цвет/шероховатость/нормаль/свечение), ZTSR (TAAU в выходное разрешение) +
  dynamic resolution (по времени кадра GPU); mip bias текстур при апскейле — позже
- [x] EXR импорт (свой декодер: scanline и tiled, NONE/RLE/ZIPS/ZIP/PIZ/PXR24, half/float/uint, слои; бит-в-бит с
  OpenEXR на эталонных файлах), писатель EXR (half, ZIP); HDR (Radiance) — с M5. B44/DWA, multi-part, deep — не нужны
- [x] Энкодеры BC6H (все 14 режимов декодируются, кодер: 1-регионные 11–14 + 2-регионные 1–10 на лучших разбиениях) и
  BC7 (режимы 6, 1, 4/5 с ротацией каналов); раскладки битов проверены на эталонном декодере WARP (тест RHI); HDR
  кукается в BC6H с мипами (box), текстуры с альфой — в BC7 (+3.9 dB к BC3 при том же размере), `compression` в .zmeta
- [x] HDRI Sky: панорама (.hdr/.exr) заменяет физическое небо — фон, SH-освещение, туман, вода, отражения (куб 128 из
  панорамы); солнце находится при кукинге (HdrInfo) и становится направленным светом с тенями, вырезается из
  освещения неба; поворот, яркость, режимы солнца; превью HDR в Content Browser, двойной клик / перетаскивание,
  самопроверка `-hdritest`
- [x] Приёмка (часть 2): пост-процесс-волюмы блендятся (тесты PostProcessVolume.*, `-posttest`)

### M11 — Физика (закрыта, 0.11.0)
- [x] Модуль Physics (ZPhys): формы Sphere / Box / Capsule / TriangleMesh (статика, BVH), составные тела (несколько
  форм с локальными позами), масса и тензор инерции из плотностей (теорема о параллельных осях)
- [x] Broadphase: два динамических AABB-дерева (static / moving), «толстые» AABB с предсказанием движения, слои +
  матрица взаимодействия; поиск пар и узкая фаза параллельно (jobs), слияние в детерминированном порядке
- [x] Narrowphase: SAT с клиппингом граней для коробок и треугольников (до 4 точек), точные ближайшие точки для сфер и
  капсул, speculative contacts, фильтр «призрачных» рёбер меша, кеш контактов и warm starting
- [x] Солвер: soft step (как Box2D v3) — 4 подшага, мягкая коррекция проникновения, relax-проходы, трение, restitution;
  острова и сон, пробуждение касанием; детерминизм (тест)
- [x] Raycast по слоям (сфера, коробка, капсула, меш), QueryAabb
- [x] Интеграция: компоненты Rigid Body, Box / Sphere / Capsule / Mesh Collider; Mesh Renderer с Collision без
  коллайдеров — статический треугольный меш; PIE с фиксированным шагом 60 Гц и интерполяцией, кинематические тела,
  телепорт сущностей, капсула Character Controller толкает тела; Create-фигуры получают коллайдеры; каркасы
  коллайдеров во вьюпорте; Features → Physics; консоль `physics.spawn`, `physics.stats`, `action`; `-physicstest`
- [x] Приёмка (часть 1): стопка из 20 кубов стоит 60 с (тест Physics.StackOf20CubesStableFor60Seconds), 500 тел —
  2.9 мс на шаг (ManualPhysics.Pile500Bodies, i5-10310U)
- [x] ConvexHull (Quickhull, до 32 вершин; строится при старте Play из меша, кешируется по мешу и масштабу) + GJK (ближайшие точки). Вместо EPA проникновение считается по SAT граней и рёбер — точнее для манифолдов (см. ADR 081)
- [x] HeightField (сетка высот, запросы по клеткам, луч — обход клеток, «под поверхностью — земля», частичное обновление высот) + компонент Terrain v0 (шум, M_Terrain); лепка и слои — в M13 (см. ADR 082)
- [x] Запросы: raycast, shapecast (консервативное продвижение по узкой фазе), overlap; триггеры (сенсоры, события
  входа/выхода) и события контактов (с силой удара); слои у коллайдеров; API в ZeqouScript (`entity.velocity`,
  `addForce/Impulse/Torque`, `physics.raycast/sphereCast/overlapSphere/gravity`, колбэки `collisionEnter/Exit`,
  `triggerEnter/Exit`) и C++ (Host + `OnCollisionEnter/Exit`, `OnTriggerEnter/Exit`, Game SDK v2)
- [x] CCD: speculative contacts с запасом по относительной скорости (тест: 100 м/с сквозь стенку 10 см не проходит);
  swept sphere — если понадобится для снарядов быстрее ~150 м/с
- [x] Closest point (сфера-зонд через узкую фазу), батчевые лучи (RaycastBatch, jobs; запросы только читают мир), Stay-события (TouchStay / SensorStay, колбэки в скриптах и C++)
- [x] (0.11.2, вне плана вехи) Звуковой проход: эмбиент во всех шаблонах, шаги по поверхности (Surfaces.h), прыжок/приземление, петли качения/скольжения (ImpactSound.rollSound), озвучка пропов, звуки интерфейса редактора
- [x] (0.11.1, вне плана вехи; основа для M20) Упаковка игры: File → Package Project, рантайм ZXGame, формат .zpak (блоки ZLZ + оглавление, монтирование в VFS), кукинг под пути пакета, общий игровой цикл редактора и игры (Engine/GameRuntime.h). Позже: Shipping-рантайм, кукинг только достижимого из сцен, настройки графики игрока, сохранения
- [x] (0.10.15, вне плана вехи) Компоненты Directional Light и Sky Light, светлее экспозиция и небо по умолчанию
- [x] (0.10.14, вне плана вехи) Буфер скоростей движущихся объектов: motion blur объектов, TAA по их движению
- [x] (0.10.13, вне плана вехи) Флипбуки частиц, запечённые из 2D-симуляции жидкости (огонь, дым)
- [x] (0.10.12, вне плана вехи) Parallax Occlusion (узел графа), Subsurface (выход материала), LUT в Post Process
  Volume, стартовые текстуры и LUT
- [x] (0.10.11, вне плана вехи) Симуляция огня и дыма на GPU (компонент Fluid Volume)
- [x] (0.10.10, вне плана вехи) Объёмные облака с тенями на земле и в тумане, атмосфера в шаблонах
- [x] (0.10.9, вне плана вехи) Объёмный туман (лучи света, свет ламп и огня в тумане), компоненты Height Fog и Fog
  Volume, объёмный дым над огнём, освещение частиц объёмом, тепловое марево, столкновения частиц
- [x] (0.10.8, вне плана вехи) Записанные звуки CC0 в стартовом наборе, компонент Impact Sound, наборы вариантов,
  шаги и звуки пропсов
- [x] (0.10.7, вне плана вехи) Живой огонь: компонент Flammable (нагрев, воспламенение, фронт пламени по поверхности,
  дым и свет по площади горения, обугливание и угли в шейдере, Charred / Crumble, разрыв суставов, тушение водой),
  скриптовое API, горючие стартовые пропсы, языки пламени в шейдере частиц
- [x] (0.10.6, вне плана вехи) Стартовый набор ассетов (эффекты, персонажи, танк, пропсы на суставах), частицы v0
  (CPU, компонент Particle Emitter, превью в редакторе), узлы материалов World Grid / Noise, новые уровни шаблонов,
  миниатюры шаблонов — снимки из редактора
- [x] (0.10.5) Джоинты: Fixed, Hinge (лимиты, мотор), Slider (лимиты, мотор), Ball (конус + twist), Distance (стержень /
  верёвка / пружина), Configurable (6DOF: Locked / Limited / Free по осям); разрыв по силе/моменту, collideConnected;
  строки soft-step солвера, блок 3×3 для точки и вращения; компоненты сцены, гизмо, `entity.jointValue/setMotor`
- [x] Character controller на физике: капсула, sweep & slide (shape cast), ступеньки (step up / step down по нормали
  опорной грани под точкой контакта), склоны (maxSlope), съезд с крутых, движущиеся платформы, толкание тел по массе
  (pushMass), кинематическая капсула для остальных тел; без физики — прежние AABB-коробки
- [x] Параллельное решение островов (jobs, когда ни один остров не занимает больше половины работы; результат совпадает с последовательным), debug draw тел по островам / контактов / джоинтов (Show → Physics Debug), шаблон проекта Physics Playground; Cylinder Collider (выпуклая оболочка), трение качения
- [x] Приёмка (часть 2): персонаж ходит по лестницам и склонам (тесты CharacterPhysics.*), рейкасты по слоям из
  геймплея (тест PhysicsSystem.ScriptsGetCollisionsAndQueries)

### M12 — GPU-driven рендер + Deferred путь
- [x] (0.11.3) GPU culling: frustum + HiZ two-phase (HiZ прошлого кадра → предпроход → HiZ → вторая фаза по
  кандидатам), выбор LOD на GPU, `ExecuteIndirect` (draw id в корневой константе), каскады теней на том же пути;
  `r.GPUDriven`, `r.OcclusionCulling`, `stress.instances`, `stat.cpu`
- [x] (0.12.0) Приёмка: 100k инстансов в Sponza на UHD 620 — CPU 26 мс, GPU 32 мс в редакторе (≈ 8 мс из них —
  интерфейс и обводка выделения): ≥ 30 FPS, CPU не узкое место (`stress.instances 100000`)
- [x] (0.12.0) Deferred путь: G-буфер (A base color + AO sRGB, B oct-нормаль + roughness R10G10B10A2 — его же
  читает SSR, C subsurface + metallic), эмиссия в HDR-цель, полноэкранный проход освещения с той же функцией
  LightSurface, что и forward; `r.Renderer.Path`, Full = deferred, Lite = forward+
- [x] (0.11.4) PCSS для солнца (поиск блокеров + фильтр по диску Фогеля, мягкость по угловому размеру солнца),
  контактные тени (screen-space марш к солнцу после предпрохода), кеш каскадов с инвалидацией по сдвинувшимся
  кастерам (старое и новое место)
- [ ] Раздельный кеш статики / динамики (копия статической карты + дорисовка движущихся) — если понадобится
- [x] (0.11.5) Частицы v1 на GPU: emit / simulate / args в compute, два буфера состояния со сжатием, столкновения
  по буферу глубины, DrawIndirect тем же шейдером частиц; префаб P_SparkShower, `stress.particles`
- [ ] GPU-сортировка частиц (для дыма с альфа-смешиванием) — следующий шаг
- [x] Volumetric fog (сделан раньше, в 0.10.9)

### M13 — Анимация
- [x] (0.12.1) Skeleton / AnimationClip из glTF (скины, каналы TRS, STEP / LINEAR, CUBICSPLINE по ключам), веса в
  вершинах (u8×4 + unorm8×4), bind shape переносится в пространство узла (палитра в позе привязки = единичная),
  GPU-скиннинг (compute, два буфера на экземпляр: velocity), Animator + AnimationSystem (кроссфейд),
  масштаб импорта в .zmeta, стартовый персонаж SK_Human (CC0)
- [x] (0.12.2) AnimGraph (.zanimgraph): параметры float / bool / trigger, состояния (клип / Blend Space 1D с
  синхронизацией фазы), переходы (условия, длительность, exit time, any state); Character Controller задаёт Speed /
  VerticalSpeed / Grounded; граф Locomotion в Starter Content, анимированный персонаж в шаблоне от третьего лица;
  скриптовое API animParam / animTrigger / playAnim / animState
- [x] (0.12.5) Blend Space 2D (gradient band interpolation, сэмплы где угодно на плоскости двух параметров), слои графа
  (свои машины состояний на общих параметрах: override / additive относительно первого кадра состояния, маска костей =
  кость + потомки, вес и параметр веса, «пустые» состояния без движения глушат слой с плавным входом/выходом);
  слой UpperBody с ударом в Locomotion, удар в шаблоне от третьего лица (ЛКМ / F, событие PunchHit толкает тела)
- [x] (0.13.0) Root motion: горизонтальное смещение корневой кости клипов (с учётом циклов, смешивания blend space
  и кроссфейдов) двигает Character Controller или верхнюю сущность, модель остаётся над ней; кость и вкл. — в Animator
- [x] (0.12.3) IK: two-bone (точный, двухшаговый), foot IK (рейкаст к земле через физику, опускание таза,
  выравнивание стопы по нормали, сглаживание), look-at головы с ограничением угла
- [x] (0.12.3) Нотифаи: события состояний графа (`events`) и автоматические Footstep по касанию ступни → скрипты
  `onAnimEvent(name)`
- [x] (0.13.0) Morph targets: импорт glTF (targets POSITION / NORMAL, имена из extras.targetNames, веса по
  умолчанию, анимация канала weights), смещения проходят через сварку / оптимизацию / LOD, GPU — в compute-проходе
  скиннинга (и для мешей без скелета), компонент Morph Targets, `entity.setMorph / morph`; стартовый слайм SK_Blob
- [x] (0.12.4) Редакторы: Animation Viewer (клипы, таймлайн с перемоткой, скорость, loop, орбитальная камера) и
  AnimGraph Editor (узлы / переходы / Any State, параметры, свойства состояний и переходов, undo, сохранение с
  перезагрузкой графа у играющих аниматоров, подсветка текущего состояния в Play)
- [x] Приёмка: персонаж из glTF бегает и прыгает с плавными переходами и foot IK (шаблон от третьего лица с 0.12.3;
  тесты AnimationSystem.LocomotionGraphFollowsTheCharacter / FootIK*) — **M13 закрыта в 0.13.0**

### M13.x — запросы пользователя после M13
- [x] (0.13.1) Мягкие тела (node-beam, как BeamNG.drive): компонент Soft Body + SoftBodySystem (XPBD-подшаги, балки с
  пластической деформацией и разрывом, контакты узлов с миром и толкание твёрдых тел, подвеска на лучах, шины с кругом
  трения, привод / тормоз / ручник), меш кузова скиннится к узлам; скрипты `carInput / carSpeed / carDamage /
  carImpact / carReset`, `SoftBodyCar.zs`, `CarCamera.zs`, префаб Characters/P_SoftCar, шаблон Vehicle Playground
- [x] (0.13.6) Ветер: Wind Zone (направленная / сферическая, порывы, завихрения) + Wind Response на объект (сила,
  укрытие, свой ветер, сопротивление тел); частицы CPU / GPU, огонь, Fog Volume, облака, вода, твёрдые тела, машина
- [x] (0.13.7) Вода v2: FFT-океан (3 каскада, JONSWAP + разброс, барашки по якобиану), SSR + небо, блик с тенями,
  SSS гребней, каустики, кольцевая сетка до горизонта, пресеты Ocean / Lake / Pool / Storm / Tropical
- [x] (0.13.8) Паки ассетов при создании проекта (Medieval Village, Stylized Nature, Survival — Quaternius, CC0) и
  смена профиля Lite / Full из редактора (с предупреждением на встроенной графике и перезапуском)

### M14 — Игровой UI + ZGraph
- [x] (0.13.2) Retained UI v1: виджеты — сущности сцены под UI Canvas (Outliner, Details, префабы, undo, сохранение);
  UI Element = флекс-подмножество (direction, justify, align, gap, padding, margin, grow) + абсолютная привязка
  (anchor / pivot / offset), масштаб по высоте экрана (reference height); Panel, Text (вес, выравнивание, перенос, тень),
  Image, Button, Progress Bar, Slider, Toggle; мышь (hover / нажатие / клик по отпусканию / перетаскивание);
  скрипты `onUIClick / onUIChanged`, `entity.text / value / visible`, `ui.cursor`, `time.scale`; рисуется поверх
  вьюпорта (редактор: превью, Show → Game UI) и кадра ZXGame (шрифты в Content.zpak); Create → UI; HUD и меню паузы в
  Vehicle Playground
- [ ] Стили (`.zstyle`), анимации свойств, навигация геймпадом, ScrollView, InputField, перенос во флексе, world-space canvas
- [ ] Локализация (`.zloc`, `LOCTEXT`)
- [ ] UI designer (WYSIWYG в вьюпорте)
- [ ] ZGraph: формат графа, компиляция в байткод VM, события / flow / переменные / вызовы рефлексированных функций
- [ ] Редактор ZGraph + отладка (брейкпоинты, подсветка активных связей, watch)
- [ ] Приёмка: ThirdPersonDemo — HUD + меню паузы + логика двери / пикапов на ZGraph

### M15 … M20
Чекбоксы детализируются при старте вехи (переносом задач из раздела 11).

---

## 15. ADR-лог (архитектурные решения)
| # | Дата | Решение | Причина |
|---|---|---|---|
| 001 | 2026-10-11 | C++20 + D3D12 первым бэкендом, HLSL/DXC | Всё есть в установленном Windows SDK, лучшая поддержка на Intel UHD 620, без внешних зависимостей |
| 002 | 2026-10-11 | Левосторонняя система, Y-up, +Z вперёд, метры, reversed-Z | Совпадает с D3D/Unity, лучшая точность глубины |
| 003 | 2026-10-11 | Матрицы column-major в C++, `row_major` не используется в HLSL; умножение `mul(M, v)` | единообразие CPU/GPU (уточнить при M1) |
| 004 | 2026-10-11 | Ноль сторонних библиотек, только ОС/SDK | требование «с нуля» |
| 005 | 2026-10-11 | Bindless по умолчанию (unbounded arrays, SM 6.6 heap-индексация при наличии) | меньше CPU-оверхеда, проще материалы, GPU-driven путь |
| 006 | 2026-10-11 | Archetype ECS + GameObject-фасад в редакторе | производительность DOD + удобство Unity |
| 007 | 2026-10-11 | Геймплей: C++ DLL с hot-reload + визуальный ZGraph | паритет с UE (C++/Blueprints) без внешней VM |
| 008 | 2026-10-11 | Шрифты: Inter TTF (поднаборы), Lucide TTF; WOFF2 не поддерживаем | TTF есть в архиве; WOFF2 требует Brotli |
| 009 | 2026-10-11 | Низкоуровневый HAL (память ОС, потоки, файлы, время, стектрейсы, CPU-инфо) живёт в Core (`Core/HAL`, реализация `Core/Private/Windows`); модуль Platform — окна, ввод, диалоги, DLL | Core не может работать без потоков/файлов; так же устроен UE |
| 010 | 2026-10-11 | Глобальный heap = HeapAlloc ОС с заголовком-тегом (статистика/утечки); TLSF реализуется как offset-аллокатор (нужен для суб-аллокации GPU-памяти) | надёжно и быстро; TLSF переиспользуется в RHI |
| 011 | 2026-10-11 | Контейнеры хранят указатель на `Allocator` (nullptr = глобальный heap), а не template-политику | простота API, возможность per-frame аллокаторов без смены типа |
| 012 | 2026-10-11 | Модули линкуются как WHOLE_ARCHIVE | статическая саморегистрация (типы, CVars, категории логов, operator new) не должна выкидываться линкером |
| 013 | 2026-10-11 | Job system v1: общие очереди по приоритетам под mutex + condvar (не work-stealing) | проще и корректнее; ~1.2 мкс/задача достаточно для M1–M10. Work-stealing — при упоре в профайлере |
| 014 | 2026-10-11 | Рефлексия через ADL `ZxTypeOf(T*)` + ленивое построение с поддержкой рекурсивных типов | работает в любом namespace, без кодогенерации; самоссылающиеся структуры (Node с Array<Node>) |
| 015 | 2026-10-11 | Натуральный логарифм называется `Ln`, не `Log` | конфликт имени с namespace `zx::Log` |
| 016 | 2026-10-11 | Кодеки изображений и BCn-энкодер живут в Core (`Core/Image`), без зависимости от RHI | нужны импортёрам, тулам и тестам без GPU; RHI-часть (создание текстур) — в Render/TextureLoader |
| 017 | 2026-10-11 | Render graph v1: граф пересоздаётся каждый кадр, ресурсы — из пула по совпадению desc (алиасинг = переиспользование после конца жизни), барьеры — через трекинг состояний командного листа; одна очередь | просто и корректно; placed-ресурсы на heap-ах и async compute — после суб-аллокатора (M2-долг) |
| 018 | 2026-10-11 | Модуль Engine (поверх Platform + Render) для игровых систем (камера, позже ECS/сцена/ассеты) | Render не зависит от ввода и окна |
| 019 | 2026-10-11 | EngineProfile Lite/Full выбирается при старте (UMA/WARP, VRAM < 3 ГБ или RAM < 12 ГБ → Lite), переопределяется флагами и CVar | одна кодовая база, разные дефолты качества; см. 1.3 |
| 020 | 2026-10-11 | ECS в отдельном модуле Scene; ComponentId назначается при первом `ComponentIdOf<T>()` (без рефлексии), имя типа из `__FUNCSIG__`; `Get<T>`/запрос с не-const T помечает колонку изменённой | компоненты не обязаны быть отражены; change detection бесплатна для чтения |
| 021 | 2026-10-11 | Структурные изменения во время итерации запрещены (assert), только через CommandBuffer; системы без объявленного доступа исполняются эксклюзивно | безопасность параллельных систем без блокировок |
| 022 | 2026-10-11 | Импорт приводит всё к движковой СК: зеркало X + разворот winding (RH CCW → LH CW), tangent.w меняет знак | одна конвенция во всём рантайме; доказано тестом совпадения нормалей с геометрией |
| 023 | 2026-10-11 | Меш = секции-чанки, у каждой своя LOD-цепочка с запертыми границами; большие секции режутся при кукинге (≤ 4096 треуг.) | уровни-«один меш» (Sponza) получают куллинг и LOD; нет трещин между чанками/материалами; шаг к кластерам M15 |
| 024 | 2026-10-11 | Модуль Asset без зависимости от RHI: свои TextureFormat/CookedTexture; Render переводит в rhi::Format | кукинг и тесты работают без GPU |
| 025 | 2026-10-11 | Bindless-геттеры по умолчанию однородные (без NonUniformResourceIndex), отдельные `*NonUniform` | NonUniform стоил заметное время на Intel; индексы из push/cbuffer однородны |
| 026 | 2026-10-11 | Lite: HDR-цель R11G11B10, анизотропия 4x (bindless-сэмплер), рендер 70 %, текстуры ≤ 1024 | UHD 620 упирается в пропускную память; aniso 16x стоил ≈ 6 мс на Sponza |
| 027 | 2026-10-12 | RHI пишет root constants/CBV одновременно в графический и compute root signature | привязка до SetPipeline или через переключение compute↔graphics молча терялась (баг найден на небе) |
| 028 | 2026-10-12 | Теневые каскады — отдельные постоянные текстуры с кэшем по «сфере с запасом»; статичная сцена не перерисовывает тени | геометрия — узкое место UHD 620; 3 каскада Sponza стоили ≈ 5 мс каждый кадр |
| 029 | 2026-10-12 | Освещение в физических единицах с pre-exposure (экспозиция умножается в шейдерах освещения), автоэкспозиция возвращается на CPU с задержкой 2 кадра | значения в HDR-цели остаются в диапазоне R11G11B10; один источник правды для экспозиции |
| 030 | 2026-10-12 | Небо: модель Hillaire (LUT), ambient — SH9 от sky-view LUT; спекулярный ambient на Lite — SH по направлению отражения | дёшево и динамично (время суток); кубмап-префильтр — для Full позже |
| 031 | 2026-10-12 | Тонмаппер по умолчанию AgX (ACES-fit — опция) | AgX мягче ведёт насыщенные цвета (фонари, закат) |
| 032 | 2026-10-12 | GTAO считается из depth pre-pass до forward (без G-буфера), результат — R8 текстура для ambient | Forward+ не имеет нормалей до основного прохода; восстановление нормалей из глубины достаточно для AO |
| 033 | 2026-10-12 | Атлас локальных теней — фиксированный размер плиток + стабильные id источников; грань перерисовывается только при движении/инвалидации | простая аллокация, кэш между кадрами; переменные размеры плиток — при необходимости позже |
| 034 | 2026-10-12 | TAA v1 репроецирует по глубине (только движение камеры) | сцены пока статичны; velocity-буфер добавится вместе со скиннингом/динамикой (M13) |
| 035 | 2026-10-12 | UI рисуется SDF-примитивами: 64-байтный примитив = инстанс квада, весь UI — один draw call; клип хранится в примитиве (индекс) | без тесселяции и смены состояний; любой масштаб без артефактов; CPU-стоимость минимальна |
| 036 | 2026-10-12 | Глифы растеризуются на CPU в точном размере (аналитическое покрытие, 4 субпиксельные фазы, без хинтинга), атлас R8 2D-array с LRU-вытеснением страниц; MSDF — только для текста в мире (M14) | максимальная чёткость мелкого текста (как macOS/браузеры), простая реализация; MSDF размывает мелкие кегли |
| 037 | 2026-10-12 | UI смешивается в пространстве дисплея (sRGB-значения), premultiplied alpha; покрытие текста ^0.85 | совпадает с тем, что видят дизайнеры в браузере/Figma; степень компенсирует «тонкость» светлого текста на тёмном |
| 038 | 2026-10-12 | Шрифты: статичные TTF-поднаборы Inter как цепочка фолбэков + Lucide-шрифт (PUA) в той же цепочке; иконки — макросы-строки UTF-8 | иконки прямо в тексте ("ZX_ICON_SAVE \" Сохранить\""), один путь шейпинга/атласа для текста и иконок |
| 039 | 2026-10-12 | Immediate-mode API в стиле Dear ImGui (ID из хэша метки, "##"), логические пиксели × DPI × масштаб пользователя; взаимодействие покрыто headless-тестами с синтетическим вводом | минимум состояния у редактора, быстрые итерации; регрессии ловятся без GPU и ручных кликов |
| 040 | 2026-10-12 | «Пустые» примитивы (рамки, тени под непрозрачными окнами) рисуются кольцом из 4 полос (24 вершины на инстанс), квады обрезаются по клипу в вершинном шейдере | UI на UHD 620 упирается в заливку; −30 % пикселей в демо |
| 041 | 2026-10-12 | Весь пользовательский текст движка/редактора — на английском | решение пользователя; стандарт индустрии, документация для людей и ИИ |
| 042 | 2026-10-12 | Докинг внутри ZUI: узлы-сплиты/листья с вкладками, док-окна — обычные окна ZUI со слоем ниже плавающих; раскладка — текстовый файл | один механизм ввода/рендера для всего; простая сериализация и сброс |
| 043 | 2026-10-12 | Вьюпорт рендерится в отдельную текстуру (RGBA8, размер панели) и показывается Image; гизмо/иконки рисуются 2D-примитивами ZUI поверх | чёткие сглаженные гизмо без TAA-дрожания, независимость от render scale |
| 044 | 2026-10-12 | Компоненты для редактора/сериализации регистрируются в ComponentRegistry (ECS id ↔ TypeInfo, имя, иконка, флаги) | Details и .zscene работают с любым отражённым компонентом без ручного кода |
| 045 | 2026-10-12 | Undo/Redo — снимки сущностей (JSON того же формата, что .zscene) до/после транзакции, идентичность по EntityGuid | одна реализация для всех операций, без ручных команд; проверяется тем же кодом, что и сохранение сцен |
| 047 | 2026-10-12 | Импорт извне проекта копирует исходник (с зависимостями) в Content/<Имя>/ | проект самодостаточен и переносим, как в UE/Unity; .zscene ссылается только на файлы проекта |
| 048 | 2026-10-12 | Список недавних проектов — per-user файл в %LOCALAPPDATA%/ZeqouX3D | его же будет читать ZeqouX Hub; не зависит от расположения движка |
| 049 | 2026-10-12 | Outline выделения — маска + кольцевые выборки в прямоугольнике выделения, без jump flood | для обводки 2–3 px JFA (log2(N) полноэкранных проходов) дороже на iGPU; ножницы делают стоимость пропорциональной размеру объекта |
| 051 | 2026-10-12 | PIE играет в редакторском World, а не в копии; на Stop мир восстанавливается из снимка | панели и рендер работают с одним миром без переключения; та же сериализация, что у .zscene, гарантирует точный откат и сохранение guid для Undo |
| 052 | 2026-10-12 | Инстанс префаба в мире раскрыт полностью (обычные сущности + PrefabLink), overrides вычисляются только при сохранении/Apply | рантайм и редактор не знают о префабах; дельта считается рефлексией против раскрытого префаба, вложенность — тем же механизмом |
| 053 | 2026-10-12 | Material Graph компилируется в HLSL-функцию, вставляемую в общий шаблон ForwardPBR (не в отдельный шейдер) | освещение, тени, кластеры и туман остаются одним кодом; граф описывает только поверхность |
| 054 | 2026-10-12 | Все значения графа (и константы, и параметры) и индексы текстур — в буфере параметров, а не в коде | правка значения не перекомпилирует шейдер; инстансы делят шейдер родителя |
| 055 | 2026-10-12 | Шейдер графа компилируется асинхронно, до готовности материал рисуется стандартным шейдером | вьюпорт не замирает; < 1 с до результата на UHD 620 |
| 056 | 2026-10-12 | Встроенные фигуры — меши в памяти AssetManager под фиксированными GUID | сцены ссылаются на них как на обычные меши; превью материалов без файлов |
| 057 | 2026-10-12 | Все команды редактора — в реестре действий; хоткеи в Saved/Config/Keys.ini | меню, горячие клавиши и палитра согласованы; переназначение без перекомпиляции |
| 058 | 2026-10-12 | SVG-иконки растеризуются в атлас шрифтов как «кастомные глифы» | один примитив и один кэш с текстом; монохромные иконки с тонировкой, как Lucide |
| 059 | 2026-10-12 | Собственный язык ZeqouScript (модуль Script: компилятор в байткод + стековая VM, привязки в Engine/Scripting) как основной язык геймплея; C++ DLL (ADR 007) остаётся для M10, ZGraph — для M14 | пользователю нужен простой язык с hot reload без внешней VM и без компилятора C++; байткод-VM без сторонних библиотек, ошибки с файлом и строкой |
| 060 | 2026-10-12 | Значения `prop` хранятся в компоненте Script как JSON только для отличающихся от умолчания; инициализаторы `prop` пропускаются, если хост уже задал значение | правка скрипта меняет умолчания у всех сущностей, переопределения сохраняются; `var x = speed * 2` видит значение сущности |
| 061 | 2026-10-12 | Вода — отдельный проход (Render/Water) после неба и до тумана: компьют-копия цвета и глубины сцены, затем сетка из SV_VertexID с волнами Герстнера и запись глубины | без вершинных буферов и материальной системы; преломление и толщина из копий; туман/TAA обрабатывают воду как обычную поверхность |
| 062 | 2026-10-12 | Тонмаппер по умолчанию ACES (AgX остаётся опцией) | AgX заметно обесцвечивает небо и воду; ACES — как в UE/Unity URP |
| 063 | 2026-10-13 | GpuMesh хранит копию LOD0 (позиции + индексы) на CPU для рейкастов | точный пикинг и инструменты без чтения GPU; память ~16 байт на вершину/индекс (Sponza ~3 МБ) |
| 065 | 2026-10-13 | Аудиовывод — WASAPI shared + собственный микшер (не XAudio2) | полный контроль над 3D, шинами и DSP; один поток, предсказуемая задержка ~20 мс; AUTOCONVERTPCM снимает зависимость от формата устройства |
| 066 | 2026-10-13 | 3D, виртуализация и ducking считаются на игровом потоке раз в кадр; микшер получает целевые значения и интерполирует их внутри блока | дешёвый микшер без математики 3D, отсутствие щелчков при любой частоте кадров |
| 067 | 2026-10-13 | Аудиоклипы грузятся по пути из Content без DDC; Vorbis длиннее 8 с остаётся сжатым и декодируется при воспроизведении | быстрый старт и малая память для музыки; кукинг аудио — при появлении пакетов (M19) |
| 068 | 2026-10-13 | C++ модуль игры общается с движком только через Game SDK: однофайловый заголовок с абстрактным `Host` и C-экспортом `ZxGetGameModule`; движок не делится на DLL | модуль собирается за секунды без линковки с движком, ABI = vtable + POD-структуры (версия `ApiVersion`), DLL можно выгружать в любой момент; расширение API — новые методы в конце Host и подъём версии |
| 091 | 2026-10-17 | IK — после сэмплинга графа, в пространстве скелета: two-bone решатель в два шага (сгиб среднего сустава по теореме косинусов, затем поворот цепочки от корня на цель — точное попадание вместо однопроходной формулы с погрешностью сантиметры); foot IK только в игре и только по физическому рейкасту, игнорирующему капсулу персонажа; шаги — по контакту ступни (высота над ногами персонажа с гистерезисом), а не по разметке клипов | разметка шагов в каждом клипе — ручная работа, контакт по высоте работает для любой гуманоидной модели; в редакторе меш-рейкаст попадал бы в собственную модель, а предпросмотр на рельефе там не нужен |
| 092 | 2026-10-17 | Слои AnimGraph — отдельные машины состояний на общих параметрах; маска = кости + потомки; additive относительно первого кадра состояния; состояние без движения «глушит» слой | как в Unity / UE, без отдельного «аддитивного» формата клипов: любой клип годится и для override, и для additive |
| 093 | 2026-10-17 | Root motion извлекается из корневой кости в пространстве скелета (горизонтальная часть относительно «вверх» сущности), смещение кадра применяется через CharacterState::rootMotion на следующем шаге персонажа; morph targets — плотный буфер смещений (16 байт на вершину на цель), применяемый тем же compute-проходом, что и скиннинг | персонаж с root motion продолжает упираться в стены и ступени; один проход для скиннинга и морфов, все остальные проходы видят готовые вершины |
| 094 | 2026-10-17 | Игровой UI — сущности ECS (UI Canvas / UI Element / виджеты-компоненты), а не отдельный документ-ассет; раскладка и отрисовка — GameUISystem через тот же 2D-рендерер ZUI | редактирование, префабы, undo, сериализация и поиск из скриптов работают без новой инфраструктуры (как uGUI в Unity); один рендерер текста для редактора и игры |
| 095 | 2026-10-17 | Вода v2 — FFT-океан (Тессендорф): спектр h0 строится на CPU (JONSWAP + направленный разброс) один раз на «состояние моря», симуляции общие для поверхностей с одинаковым ключом спектра (до 2), БПФ radix-2 в groupshared, 8 вещественных полей упакованы в 4 комплексных; заменяет волны Герстнера | реалистичный спектр и пена по якобиану при цене ~0,4 мс на UHD 620; ADR 061 (отдельный проход после неба с копией цвета/глубины) сохраняется |
| 090 | 2026-10-17 | AnimGraph — данные в JSON-файле `.zanimgraph` (Content), клипы по имени, параметры хранятся в рантайм-состоянии аниматора (не в компоненте), переходы проверяются по порядку и первый подходящий срабатывает; Blend Space 1D сэмплирует соседние клипы в одной нормализованной фазе; Character Controller задаёт параметры автоматически | файл редактируется руками и будущим редактором графа (0.12.4) и переносится между моделями с одинаковыми именами клипов; параметры, меняющиеся каждый кадр, не должны попадать в undo и сохранение сцены; синхронизация фазы убирает «перепрыгивание» ног при смешивании ходьбы и бега |
| 089 | 2026-10-17 | Скелетная анимация: кости не сущности, а данные ассета Skeleton (родители раньше детей, bind pose, inverse bind); вершины скина хранят до 4 костей (u8) и веса (unorm8, сумма 255) в MeshVertex, поэтому вся обработка мешей (сварка, LOD, разбиение, оптимизация) переносит их без изменений; bind shape модели запекается в пространство узла при импорте; скиннинг — compute в собственный буфер экземпляра (два буфера по очереди для velocity), скинированные экземпляры рисуются CPU-списками и в GPU-driven кадрах | сотни сущностей-костей на персонажа раздули бы сцену и иерархию; разные экспортёры кладут вершины скина в разные пространства — запекание делает меш правильным и без анимации (границы, пикинг, превью); один раз позированный буфер дешевле скиннинга в каждом из ~6 проходов, а персонажей в кадре немного |
| 088 | 2026-10-17 | Deferred-путь — G-буфер из трёх целей + эмиссия прямо в HDR-цель, освещение — полноэкранный пиксельный проход с аддитивным смешиванием (не compute), функции поверхности и освещения вынесены из forward-шейдера (EvaluateSurface / LightSurface) и общие для обоих путей; нормаль G-буфера — та же текстура normal + roughness, что читает SSR; отладочные режимы и захваты проб — forward | один источник правды для освещения: пути не расходятся (тест сравнивает картинки); пиксельный проход не требует UAV у цели сцены и сразу кладёт результат поверх эмиссии; на iGPU deferred не быстрее в сценах с парой источников, поэтому Lite остаётся на forward+ |
| 087 | 2026-10-17 | GPU-частицы — два буфера состояния в раскладке SoA, выжившие дописываются подряд (сжатие), а не пул с фиксированными слотами и списком свободных; запись для отрисовки — тот же 64-байтный GpuParticle, что у CPU-частиц, и тот же шейдер через DrawIndirect; столкновения — по буферу глубины текущего кадра; эмиттер — постоянный слот с поколением, частицы чужого поколения умирают | на UHD 620 разрозненная запись 64-байтных записей пула стоила ~20 мс на 200k частиц, последовательная — ~5 мс; общий формат отрисовки даёт GPU-частицам флипбуки, свет и туман без второго шейдера; глубина вместо физики позволяет сотням тысяч частиц сталкиваться бесплатно (цена — частицы за кадром не сталкиваются) |
| 086 | 2026-10-17 | Кеш теней солнца — инвалидация каскада целиком, когда его задевает сдвинувшийся кастер (сферы на новом и старом месте; список сдвинувшихся приходит от ViewportRenderer из прохода пар «прошлый кадр»), а не раздельные статическая / динамическая карты; контактные тени — отдельный compute-проход в R8 после предпрохода, PCSS — режим фильтра каскадов | на iGPU копия карты 2048² D32 стоит почти столько же, сколько перерисовка каскада через GPU-driven путь, поэтому раздельный кеш почти ничего не даёт; в неподвижной сцене каскады по-прежнему не перерисовываются; контактные тени в отдельной текстуре не утяжеляют forward-шейдер и выключаются без перекомпиляции |
| 085 | 2026-10-17 | GPU-driven путь: CPU каждый кадр пишет строки инстансов и по элементу отсечения на (инстанс, секцию) в upload-буферы, а таблицы групп (меш-секция + материал) и слотов (группа × LOD) задают раскладку списков видимости; один compute-проход на вид (основной — две фазы с HiZ, каскады — только пирамида) атомарно заполняет списки и `instanceCount` аргументов; draw id (слот + вид) приходит корневой константой из командной сигнатуры, VS читает индекс инстанса из списка. HiZ — две текстуры (чётные / нечётные мипы), чтобы не читать и не писать один ресурс в одном диспатче. Прозрачное и velocity-проход остаются CPU-списками | без мешлетов и mesh shaders (их нет на UHD 620) и без компакции на GPU: число групп в Sponza — сотни, поэтому «один indirect на шейдер + меш» уже снимает CPU-нагрузку; двухфазный HiZ не даёт мерцания при резких поворотах камеры; кадр со 100k объектов упирается в пропускную способность памяти (CPU и iGPU делят её), поэтому CPU-путь сведён к минимуму проходов по массивам |
| 084 | 2026-10-15 | Упаковка: пакет содержит исходники Content + DDC, «скуканный» заново с Content, смонтированным в `/Game` (ключи DDC включают путь исходника, поэтому кукинг под пути игры даёт попадания в кэш при запуске); игра регистрирует исходники так же, как редактор. Игровой цикл вынесен в Engine/GameRuntime и используется и редактором, и ZXGame | без отдельного формата «только cooked» и второго загрузчика ассетов; исходники в пакете — цена простоты (картинки и glTF небольшие), кукинг только достижимого из сцен и выкидывание исходников — следующий шаг; один цикл исключает расхождения между Play и сборкой |
| 083 | 2026-10-15 | Параллельность солвера — по островам (у каждого острова свои списки тел и ограничений, общие статические/кинематические тела только читаются), с барьером внутри подшага для кинематики; включается, только когда ни один остров не берёт больше половины работы. Раскраска графа ограничений внутри острова (как Box2D v3) отложена | результат побитово совпадает с последовательным (детерминизм сохранён); на 4 ядрах iGPU-ноутбука накладные расходы десятков диспатчей на шаг съедают выигрыш раскраски для одной кучи, а для многих отдельных групп (типичная игра) острова дают ×2.3 |
| 082 | 2026-10-14 | Террейн v0 — высоты генерируются детерминированно из параметров компонента (шум) и не хранятся в сцене; меш собирается напрямую в CookedMesh (чанки-секции для отсечения) и регистрируется как memory-меш с id от guid сущности, рисуется через Mesh Renderer той же сущности; физика — отдельная форма HeightField по тем же высотам с той же диагональю клеток | сцена остаётся маленькой, а все системы, работающие с Mesh Renderer (пикинг, тени, обводка, инструменты), сразу работают с террейном; хранение высот (лепка) добавится вместе с инструментами sculpt; CDLOD/клипмапы — когда понадобятся террейны больше ~1 км |
| 081 | 2026-10-14 | Выпуклые оболочки — Quickhull с слиянием компланарных граней и ограничением до 32 вершин; пересечение оболочка–многогранник через SAT (нормали граней + попарные направления рёбер) и отсечение опорной грани, оболочка–сфера/капсула через GJK вне и наименее проникшую грань внутри; EPA не используется | SAT даёт стабильные многоточечные манифолды (как у box–box), которые EPA в одиночку не даёт без дополнительного отсечения; 32 вершины держат SAT дешёвым (≤ 64 граней, ≤ 64 направлений рёбер); GJK нужен только для скруглённых форм |
| 080 | 2026-10-14 | Симуляция газа — stable fluids на 3D-сетке RGBA16F в ящике сущности: адвекция (полулагранжева), плавучесть, vorticity confinement, 20 итераций Якоби для давления (пол — Нейман, стороны и верх — Дирихле, открытый воздух), рендер — отдельный марш по ящику с самозатенением и излучением по температуре | даёт настоящую динамику дыма и огня (вихри, плавучесть) при цене ~3 мс на iGPU; отдельный марш по ящику точнее фроксельного тумана для резких языков; запекание симуляций во флипбуки — следующий шаг |
| 079 | 2026-10-14 | Объёмные облака — плоский слой, марш по лучу в 1/4 разрешения (двойные шаги по пустоте, одинарные с деталями в облаке, 4 шага к солнцу) + временное накопление с клампом по соседям; шумовые объёмы (64³ форма, 32³ детали) считаются на CPU при старте; карта теней облаков 256² на 3 км пишется последним проходом кадра и читается следующим кадром | первая версия на GPU занимала 20 мс и в первом кадре вызывала сброс устройства на iGPU; четверть разрешения + адаптивный шаг дают 3–4 мс; кадр задержки у теней облаков незаметен, а запись в конце кадра исключает чтение ресурса в состоянии UAV |
| 078 | 2026-10-14 | Объёмный туман — фроксельная сетка (тайлы экрана x срезы глубины d = D·t², 64 среза, тайл 10 px / 16 px в Lite) с временным накоплением; свет — солнце через CSM + кластеры локальных источников сцены; ближняя часть объёмная, дальняя — аналитическая с началом на границе сетки; та же сетка — объём освещения для частиц | подход UE / Frostbite: стоимость фиксирована и не зависит от числа объёмов; переиспользует кластеры и тени без новых проходов теней; аналитический хвост сохраняет дальний туман дешёвым |
| 077 | 2026-10-14 | Огонь — система Engine (FireSystem) над частицами: состояние горения в компоненте Flammable, тепло между точками поверхности (выборка 64 точки по коллайдеру или границам меша), фронт пламени — сфера от точки возгорания, обугливание — в шейдере по сфере (без новых текстур и без изменения мешей) | нужно «как в жизни», но дёшево и предсказуемо: сотни горючих объектов на iGPU; сфера-фронт даёт правдоподобное распространение по доскам и ящикам без UV-развёрток и симуляции жидкости; объёмный огонь и дым — в M12 вместе с GPU-частицами |
| 076 | 2026-10-14 | Частицы v0 на CPU (симуляция в Engine, отрисовка одним draw call из upload-буфера, премультиплицированное смешивание для аддитивных и дымовых частиц вместе) раньше GPU-частиц M12; стартовые ассеты — обычные префабы, собранные кодом редактора из встроенных фигур и стартовых материалов | шаблонам и стартовому набору нужны огонь, дым, туман уже сейчас; CPU-путь прост и хватает на тысячи частиц на iGPU, GPU-симуляция M12 заменит только симуляцию; префабы редактируются и переносятся как любые пользовательские |
| 075 | 2026-10-14 | Суставы — скалярные строки того же soft-step солвера, пересобираемые из поз на каждом подшаге; точка и полная фиксация вращения — блок 3×3 (обратная матрица связанных строк); координаты суставов «тело относительно соединённого»; отдельные компоненты на тип (как в Unity), Connected Body по имени сущности | одна модель с контактами (тёплый старт, мягкость, speculative-пределы) без отдельного солвера; построчный Гаусс–Зейдель давал ошибку периода маятника 3 %, блок — < 1 %; имена вместо ссылок сохраняются в JSON сцены и префабах без ремаппинга guid |
| 074 | 2026-10-13 | Физика: свой модуль Physics (зависит только от Core); солвер soft step с подшагами (Box2D v3) вместо PGS + NGS; Unity-семантика компонентов (коллайдер без Rigid Body = статика, дочерние коллайдеры = составное тело); Mesh Renderer с Collision без коллайдеров = статический треугольный меш; перемещение сущности игрой = телепорт с остановкой | PGS разваливал стопку 20 кубов (медленная сходимость на длинной цепочке), подшаги решили это при той же цене; привычная модель для пользователей Unity; старые сцены и шаблоны получают коллизию без настройки |
| 073 | 2026-10-13 | HDRI-небо: панорама пересэмплируется в sky-view LUT (солнце обрезано), поэтому SH, туман, вода и проба-захваты работают без изменений; фон и куб отражений (128) читают панораму напрямую; солнце ищется при кукинге (HdrInfo в cooked-текстуре) и становится направленным светом. BC6H/BC7 — свои кодеры с проверкой раскладок через WARP; авто: альфа → BC7, HDR → BC6H | один источник неба для всех потребителей; анализ солнца на CPU раз при кукинге, а не на GPU каждый кадр; WARP — эталонный декодер без внешних библиотек; BC7 того же размера, что BC3, но заметно точнее |
| 072 | 2026-10-13 | Декали применяются в forward-шейдере (перебор видимых декалей, до 256, с явными градиентами), а не D-buffer; ZTSR — отдельная точка входа TAA с историей в выходном разрешении | без G-буфера декали должны менять поверхность до освещения; при десятках декалей перебор дешевле отдельного прохода; кластеризация декалей — вместе с GPU-driven путём (M12) |
| 071 | 2026-10-13 | Пробы: отдельные кубы на пробу (до 16/8), захват одной грани за кадр вторым SceneRenderer, хранение при эталонной экспозиции EV10; SSR трассируется после непрозрачного прохода и применяется в forward-шейдере следующего кадра | без G-буфера: спекуляр (EnvBRDF, AO) знает только forward-шейдер, поэтому SSR подмешивается туда с репроекцией; захват по грани делит стоимость на 6 кадров; эталонная экспозиция держит значения в half-float |
| 070 | 2026-10-13 | Post Process Volume — плоский компонент с парами «overrideX + X» (EditCondition); бленд на CPU раз в кадр по позиции камеры; DOF и motion blur — compute-проходы после TAA перед экспозицией/bloom | одна сериализация и универсальный редактор полей; порядок как в UE (TAA → DOF → MB → bloom/tonemap), DOF не мерцает от джиттера; motion blur только от камеры до появления буфера скоростей |
| 069 | 2026-10-13 | Hot reload: загружается копия DLL (`Intermediate/HotReload/<Name>-N.dll`), состояние переносится только через props (по имени), объекты пересоздаются; сборка — отдельный процесс CMake + Ninja (Release, /MD) | исходная DLL не блокируется линкером, без PDB-локов; перенос props надёжен при изменении раскладки класса; полный перенос памяти (как Live++) — не нужен для приёмки |
| 064 | 2026-10-13 | Features: отключённые подсистемы хранятся списком `disabledFeatures` в .zproj и применяются масками поверх настроек вьюпорта на кадр | настройки пользователя/CVars не затираются; по умолчанию всё включено |
| 050 | 2026-10-12 | Плавающие панели остаются внутри главного окна; вынос в отдельные OS-окна — бэклог | требует мульти-swapchain и мульти-окна в ZUI/платформе; ТЗ M7 требует плавающие окна, что уже выполнено докингом |
| 046 | 2026-10-12 | .zscene хранит пути к исходникам ассетов, а не только GUID'ы | у AssetManager нет глобального реестра: загрузка исходника регистрирует его под-ассеты (меши/материалы) |
