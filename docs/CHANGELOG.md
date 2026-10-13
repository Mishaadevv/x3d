# ZeqouX3D — история версий

Схема до 1.0: **0.<последняя закрытая веха>.<выпуск внутри неё>**. Каждая закрытая веха ТЗ (раздел 11) поднимает
среднюю цифру, каждая порция новых возможностей или исправлений между вехами — последнюю. Номер версии задаётся в
одном месте — `project(ZeqouX3D VERSION ...)` в корневом CMakeLists.txt (оттуда его берут `Core/Version.h`, ресурс
версии ZXEditor.exe, About и лог).

## 0.10.0 — M10 закрыта: EXR, BC6H/BC7, HDRI-небо (2026-10-13)
- **OpenEXR**: свой декодер — scanline и tiled, сжатие NONE / RLE / ZIPS / ZIP / PIZ / PXR24, каналы half / float / uint,
  RGB(A), Y и слои (`beauty.R`); результат совпадает с библиотекой OpenEXR бит-в-бит на эталонных файлах. Свой
  писатель EXR (half, ZIP). .exr импортируется как текстура
- **BC6H** (HDR) и **BC7**: свои кодеры и декодеры. BC6H — все 14 режимов, BC7 — режимы 6, 1 и 4/5 с ротацией каналов;
  раскладки битов проверены эталонным GPU-декодером (WARP). HDR-текстуры кукаются с мипами в BC6H (в 4 раза меньше
  half-float), текстуры с прозрачностью — в BC7 вместо BC3 (+3.9 dB при том же размере). Формат можно задать в .zmeta:
  `"compression": "BC7"`
- Компонент **HDRI Sky**: HDR-панорама (.hdr / .exr) заменяет физическое небо — фон в полном разрешении, освещение
  неба, цвет тумана, вода, куб отражений (128 px из самой панорамы), пробы. Солнце находится в панораме при кукинге и
  освещает сцену как направленный свет с тенями (и вырезается из освещения неба, чтобы не светить дважды); режимы
  From HDRI / Scene Sun / None, поворот, яркость в нитах. Create > HDRI Sky, двойной клик по .hdr/.exr в Content
  Browser или перетаскивание во вьюпорт, «Use as HDRI Sky» в меню файла; консоль `hdri.info`, `hdri.set`,
  `create hdrisky <панорама>`
- Content Browser: превью HDR-картинок с экспозицией и тонмаппингом, тип «HDR Image», .tga тоже как текстура
- Консоль: `camera x y z [yaw] [pitch]`
- Исправлено: повреждённый zlib-поток мог зациклить декодер DEFLATE (касалось PNG) — теперь отказ
- `Tools/fetch_samples.ps1` скачивает и пример панорамы Poly Haven (CC0); `Tools/gen_exr_testdata.py` — эталоны EXR
- Самопроверка `-hdritest`; тесты Exr.*, TextureTools.Bc7*, Bc6h*, HdrMipChain, RHI.BlockCompressionMatchesHardwareDecoder

## 0.9.4 — декали, ZTSR, динамическое разрешение (2026-10-13)
- Компонент **Decal**: проецирует картинку (альфа = покрытие) и при желании карту нормалей на всё внутри коробки,
  вдоль −Y сущности (по умолчанию — на землю под ней); меняет цвет, шероховатость, нормаль, может светиться;
  затухание по углу и глубине, порядок наложения. Декали применяются в forward-шейдере до освещения, поэтому
  освещаются как сама поверхность и видны в отражениях проб. Create > Decal ставит декаль на землю; коробка со
  стрелкой проекции во вьюпорте; текстура перетаскивается из Content Browser в поле Details
- **ZTSR** — временной апскейл: при разрешении рендера ниже 100 % TAA накапливает историю в выходном разрешении
  (реконструкция по джиттеру, гауссовы веса, клиппинг истории) — заметно чётче билинейного растяжения;
  постпроцесс (DOF, motion blur, bloom) работает в выходном разрешении. `r.TSR`, Show > Temporal Upscaling
- **Динамическое разрешение**: масштаб рендера следует за временем кадра GPU к цели (шаг 5 %, 4 раза в секунду,
  без сброса истории ZTSR). `r.DynamicResolution`, `r.DynamicResolution.TargetMs`, `.MinPercentage`, Show > Dynamic
  Resolution; масштаб и «ZTSR» показываются в статистике вьюпорта
- Outliner показывает иконку компонента сущности (камера, звук, декаль, проба...), а не общий значок
- Самопроверка `-decaltest`; тест DynamicResolution.*

## 0.9.3 — Reflection probes и SSR (2026-10-13)
- **Отражения неба**: sky-view LUT переносится в кубмап и префильтруется по GGX в мипы (шероховатость = мип);
  спекулярное окружение теперь из куба, а не из SH — металлы и глянец отражают настоящее небо
- Компонент **Reflection Probe**: кубмап сцены вокруг точки (64 px в Lite, 128 в Full), захват по одной грани за кадр
  отдельным рендерером сцены, автоматический перезахват при изменении сцены, солнца или неба и при догрузке
  мешей, текстур и шейдеров материалов; box projection (отражения пола и стен совпадают с комнатой), бленд по краю
  коробки и приоритету. Create > Reflection Probe, коробка во вьюпорте, `reflections.recapture`, `reflections.stats`
- **SSR**: трассировка отражений по буферу глубины на половинном разрешении после непрозрачного прохода (нормаль и
  шероховатость — второй render target через вариант шейдера), используется следующим кадром поверх проб; по
  умолчанию включено в Full, `r.SSR` / Show > Screen Space Reflections
- Features: Reflections, Screen Space Reflections; пробы в шаблонах (площадка First/Third Person, хижина Water)
- Самопроверка `-reflectiontest` (зеркальный пол, светящиеся фигуры, захват и перезахват пробы, SSR под debug layer)

## 0.9.2 — Post Process Volumes, глубина резкости, motion blur (2026-10-13)
- Компонент **Post Process Volume**: коробка (или Unbound — весь мир) с радиусом и весом бленда и приоритетом;
  переопределяет экспозицию, bloom, контраст, насыщенность, баланс белого, цветовой фильтр, виньетку, DOF и motion
  blur. Волюмы блендятся как в UE: по приоритету, вес плавно спадает за пределами коробки; переключатели меняются при
  весе ≥ 0.5. Create > Post Process Volume, иконка и контур коробки (с зоной бленда) во вьюпорте
- **Глубина резкости**: физическая модель камеры (сенсор 24 мм, фокусное расстояние из угла обзора, диафрагма f/N,
  дистанция фокуса), боке на половинном разрешении (спираль золотого угла, scatter-as-gather), передний план
  размывается поверх фона; режим вьюпорта **Depth of Field Focus** (ближе — красный, дальше — синий, в фокусе — зелёный)
- **Motion blur камеры**: скорость пикселя из репроекции глубины, длина выдержки и предел в пикселях, без размытия
  на склейках камеры; в редакторе — только во время Play
- Цветовой фильтр в грейдинге; новые Features: Post Process Volumes, Depth of Field, Motion Blur; флажки в меню Show
- Details: секции по категориям полей и галочки override перед подписью (EditCondition, как в UE); длинные подписи
  обрезаются с «…»
- Самопроверка `-posttest` (волюмы блендятся при движении камеры, DOF и motion blur под debug layer), флаг `-uiscale=`;
  MeshViewer: `-dof=<фокус>,<f-число>`, `-motionblur=<доля>`

## 0.9.1 — C++ game module, hot reload, Input Actions (2026-10-13)
- **C++ геймплей**: модуль игры проекта (`Source/<Name>/`) собирается в `Binaries/<Name>.dll` и подгружается
  редактором; компоненты — наследники `ScriptComponent` из однофайлового Game SDK (`Engine/Source/GameSDK/ZeqouGame.h`),
  регистрация макросами `ZX_GAME_COMPONENT` / `ZX_GAME_PROP` (float, int, bool, Vec3 с диапазоном и подсказкой)
- **Hot reload в PIE**: кнопка Compile (молоток у Play, F7) собирает модуль в фоне (`Tools/build_game.ps1`: MSVC + CMake +
  Ninja, окружение vcvars кешируется — пересборка ~1 с); после сборки работающие компоненты пересоздаются на новом коде
  с теми же props и получают `OnReloaded()`, игра не останавливается. Ошибки компилятора — в Output Log
- Компонент **C++ Script** (NativeScript): выбор класса и props в Details (только отличия от умолчаний сохраняются
  в сцене), сообщение о несобранном модуле с кнопками Create / Compile
- Host API для C++: сущности (Find, Spawn фигур, Destroy, позиция/поворот/масштаб), ввод (клавиши, Input Actions, мышь),
  время, Character Controller, звук, лог
- **Input Actions**: именованные кнопки и оси (Jump, Sprint, Crouch, Interact, Fire, Aim, Pause; MoveForward, MoveRight,
  LookX, LookY) с привязками к клавиатуре, мыши и геймпаду; окно Window > Input Actions, файл `Config/Input.json`;
  ZeqouScript: `input.action/actionPressed/actionReleased/axis`
- Project Browser: язык **C++** доступен при создании проекта (пишет `Source/<Name>` с примерами Spinner, Bouncer,
  PlayerController и сразу собирает); Tools > Create C++ Game Module / Open Game Source Folder для существующих проектов
- Документация проекта: `Docs/CppGameCode.md`, Input Actions в README / Getting Started / AGENTS.md
- Самопроверка `-cpptest` (с `-newproject=... -language=cpp`): сборка модуля, Spinner в PIE, правка исходника,
  пересборка и hot reload во время игры, проверка нового поведения и сохранённого prop, Stop восстанавливает сцену
- Исправлено: утечка памяти при выходе из редактора посреди `-audiotest`

## 0.9.0 — M9: звук (2026-10-13)
- Свой звуковой движок: микшер 48 кГц на отдельном потоке, вывод WASAPI, шины Master/Music/SFX/UI/Voice с громкостью,
  mute и метрами, ducking, снапшоты, реверб, лимитер; DSP: фильтры, Freeverb, задержка, компрессор
- Свои декодеры: WAV (PCM, float, IMA/MS ADPCM) и **Ogg Vorbis** (совпадает с эталоном libvorbis), потоковая музыка
- 3D-звук: затухание, панорама, конусы, доплер, окклюзия рейкастом; 64+ голоса с виртуализацией, без щелчков
- Компоненты **Audio Source**, **Audio Listener**, **Reverb Zone**; звук в Play-in-Editor (пауза глушит)
- Редактор: превью звуков в Content Browser, слот клипа в Details, окно **Audio Mixer**, иконки и радиусы во вьюпорте
- ZeqouScript: `audio.play(...)`, `audio.playAt(...)`, `audio.setVolume(...)`, `entity.playSound()`
- Starter Content: синтезированные звуки; в шаблонах — гул у куба (3D), море и ветер на острове, реверб в хижине
- Самопроверка `-audiotest` (64 источника на реальном устройстве), флаг `-noaudio`

## 0.8.6 — инструменты, Features, документация проекта (2026-10-13)
- Окно **Features** (Tools > Features): включение/выключение теней, AO, неба и тумана, bloom, TAA, воды, ZeqouScript,
  физики персонажей, инструментов Brush/Measure — хранится в `.zproj` (`disabledFeatures`)
- **Brush**: рисование моделями по поверхностям — несколько моделей с весами (Add Selected), радиус, плотность,
  интервал, разброс масштаба, случайный поворот и наклон, выравнивание по нормали, макс. уклон, смещение; Shift —
  стирание; `[` `]` — радиус; один мазок = один шаг undo; экземпляры собираются в «Brush Painted»
- **Measure** (M): расстояние между двумя точками с проекциями по осям; **Drop to Ground** (End)
- Рейкаст по треугольникам мешей (`Engine/Raycast.h`, LOD0 на CPU); выбор кликом во вьюпорте теперь точный
- New Project: опции **Documentation** (README.md, Docs/, AGENTS.md и CLAUDE.md для ИИ-ассистентов) и
  **Git repository** (git init, .gitattributes, первый коммит); флаги `-nodocs`, `-git`
- Project Browser: одна иконка (в боковой панели), без дубля в заголовке
- Самопроверка `-tooltest`; консоль: `tool brush|measure|none|drop`, `features`, `create water`

## 0.8.5 — реалистичная вода, шаблон Water (2026-10-12)
- Компонент **Water** (Create > Create Water): сетка в вершинном шейдере с 4 волнами Герстнера, отражение
  физического неба по Френелю, солнечные блики (GGX), экранное преломление, поглощение и рассеяние по толщине
  воды, пена у берегов и на гребнях, процедурная рябь; вода пишет глубину (туман, TAA работают поверх)
- Шаблон **Water**: остров с пляжем, лугом, скалами, хижиной и пирсом, море 300 x 300 м, игрок от первого лица,
  низкое солнце с дорожкой на воде; стартовые материалы M_Sand и M_Rock
- Тонмаппер по умолчанию — ACES (AgX давал серое, обесцвеченное небо и воду)
- Стоимость на UHD 620: ~1.3 мс копия сцены + ~1.5 мс вода (1600x900, Lite)

## 0.8.4 — персонаж, камера, шаблоны First/Third Person (2026-10-12)
- Новая иконка приложения по артам пользователя (знак и логотип с надписью): `Tools/import_brand_icons.py` →
  `ZeqouX3D.ico` (16–256), `Branding/ZeqouX3D_Icon.png`, `Branding/ZeqouX3D_Logo.png`; в заголовке окна,
  Project Browser и About
- Компоненты **Camera** и **Character Controller** (ходьба со скольжением вдоль стен, ступеньки, гравитация, прыжок,
  возврат при падении); у Mesh Renderer флаг **Collision**
- Play-in-Editor: вид из игровой камеры, захват ввода (клик — захватить, Shift+F1 — отпустить), F8 — eject
- ZeqouScript: объект `character` (`move`, `jump`, `grounded`, `velocity`), `entity.child(name)`, `entity.parent`
- Шаблоны **First Person** и **Third Person**: полигон (лестница, платформы, ящики, стены) и готовые скрипты
  FirstPersonController / ThirdPersonController / ThirdPersonCamera (также в Starter Content)

## 0.8.3 — Project Browser (2026-10-12)
- Плитки проектов: первая буква на случайном цвете, цвет хранится в `.zproj` (`"color"`), язык — `"language"`
- Страница New Project в стиле UE5: сетка шаблонов с нарисованными превью (Blank, Sample Scene, First Person,
  Third Person, Water — последние три помечены «COMING IN 0.8.4/0.8.5»), справа — настройки проекта
- Выбор языка: ZeqouScript (C++ — M10, Visual Script — M14, пока недоступны)
- **Starter Content**: 10 PBR-материалов с редактируемыми параметрами и 5 скриптов (Rotator, Bobbing, SimpleMover,
  LookAt, Blinker); в пустом проекте — витрина со скриптами; флаг `-starter` для `-newproject`
- Исправлено: знак компенсации экспозиции (теперь «+» = ярче, как в UE/Unity), по умолчанию +1 EV — дневные сцены
  больше не выглядят тёмными

## 0.8.2 — ZeqouScript (2026-10-12)
- Новый язык движка **ZeqouScript** (`.zs`): компилятор в байткод и стековая VM (модуль Script), `prop`-поля с
  `@range/@tooltip/@color`, списки, vec3/quat, стандартная библиотека; справочник — `docs/ZeqouScript.md`
- Компонент **Script**: свойства скрипта в Details (переопределения жирным, Reset to Default), запуск `start()/update(dt)`
  в Play-in-Editor, объекты `entity`, `input`, `time`, `find()`
- Горячая перезагрузка во время Play с сохранением состояния; ошибки с файлом и строкой, падает только один экземпляр
- **Script Editor**: подсветка синтаксиса, номера строк, проверка ошибок при наборе, автоотступ, Tab/Shift+Tab, Ctrl+S
- Content Browser: New Script, иконка `.zs`, двойной клик открывает редактор, Add to Selected Entity
- ZUI: виджет `InputTextCode`, `PushFont(FontHandle)`; моноширинный шрифт Consolas в редакторе кода
- Самопроверка редактора `-scripttest`

## 0.8.1 (2026-10-12)
- Версия движка = закрытые вехи (0.8.x), единый источник версии, ресурс версии и иконка в ZXEditor.exe
- Иконка приложения в стиле экосистемы Zeqou (`Tools/gen_app_icon.py`), новый логотип в редакторе

## 0.8.0 — M8: префабы, Play-in-Editor, материалы-графы (2026-10-12)
- Play-in-Editor: Play/Pause/Step/Stop, мир восстанавливается после Stop, компоненты Rotator/Oscillator
- Префабы `.zprefab`: инстансы с overrides, вложенность, Apply/Revert/Unpack
- Material Graph `.zmat` (34 типа нод → HLSL), нодовый редактор, инстансы `.zmati`, асинхронная компиляция (< 1 с)
- Встроенные фигуры (Cube/Sphere/Cylinder/Plane), command palette (Ctrl+P), хоткеи в Keys.ini, SVG-иконки

## 0.7.0 — M7: редактор v1
- Оболочка в стиле UE5/Unity: свой title bar, тулбар, докинг панелей, Viewport, Outliner, Details, Content Browser,
  Output Log с консолью
- Сцены `.zscene`, Undo/Redo для всех операций, проекты `.zproj` и Project Browser, обводка выделения, миниатюры ассетов

## 0.6.0 — M6: ZUI
- Свой UI: SDF-примитивы, TTF/WOFF и растеризатор, атлас, Inter + Lucide, все базовые виджеты, DPI 100–150 %

## 0.5.0 — M5: освещение и тени
- Clustered Forward+, физические единицы и экспозиция, CSM + атлас теней point/spot, GTAO, физическое небо, туман,
  тонмаппинг, FXAA/TAA

## 0.4.0 — M4: меши, материалы, glTF, ECS
- Импорт glTF/GLB/OBJ, оптимизация и LOD мешей, ZECS, иерархия, PBR-материалы, asset system (GUID, DDC, async)

## 0.3.0 — M3: render graph и текстуры
- Render graph (transient-ресурсы, алиасинг, барьеры), PNG/JPEG/TGA/DDS, mip-ы, BC-сжатие, debug draw, fly-камера

## 0.2.0 — M2: платформа и D3D12
- Окно, ввод, главный цикл; RHI и бэкенд D3D12 (bindless, PSO, fence, timestamps), компиляция шейдеров DXC

## 0.1.0 — M0–M1: фундамент и Core
- CMake-сборка, модули; Core: память, контейнеры, строки, математика, потоки и JobSystem, лог и CVars, ZJSON,
  сериализация, рефлексия, VFS, профайлер, deflate
