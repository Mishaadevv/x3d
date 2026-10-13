# ZeqouScript — справочник языка

ZeqouScript (`.zs`) — собственный скриптовый язык ZeqouX3D: простой, читаемый, динамически типизированный.
Код компилируется в байткод и исполняется стековой VM внутри движка (модуль `Engine/Source/Runtime/Script`,
без сторонних библиотек). Скрипт вешается на сущность компонентом **Script**; его `prop`-поля редактируются в
панели Details, отдельно для каждой сущности.

```zs
// Spinner.zs
script Spinner {
    @range(-360, 360) @tooltip("Degrees per second")
    prop speed = 90.0
    @color prop tint = vec3(1, 0.5, 0)

    var turns = 0

    fn start() {
        log("Spinner on " + entity.name)
    }

    fn update(dt) {
        entity.rotate(vec3(0, speed * dt, 0))
        if input.pressed("Space") {
            turns += 1
            log("turns:", turns)
        }
    }
}
```

## Работа в редакторе

- **Content Browser → правый клик → New Script** создаёт файл-заготовку и открывает его в **Script Editor**.
- Script Editor: подсветка синтаксиса, номера строк, ошибки проверяются во время набора (строка с ошибкой
  подсвечена, клик по ошибке переходит к ней), автоотступ, Tab / Shift+Tab по выделению, **Ctrl+S** сохраняет.
  Кнопка `?` — краткая шпаргалка, кнопка со стрелкой открывает файл во внешнем редакторе.
- Подключение: **Add Component → Script**, затем перетащить `.zs` в слот Script (или кнопка папки — выбор из
  проекта / New Script…). Также: правый клик по `.zs` → *Add to Selected Entity*.
- Значения `prop` в Details сохраняются в сцене только если отличаются от значения по умолчанию (такие поля
  выделены жирным; правый клик → *Reset to Default*).
- **Play**: у всех скриптов вызывается `start()`, затем каждый кадр `update(dt)`. **Stop** возвращает сцену.
- **Горячая перезагрузка**: сохранение файла во время Play пересобирает скрипт; работающие экземпляры
  продолжают со своими значениями `var`/`prop` (новые поля получают начальные значения). Если новая версия
  не компилируется, продолжает работать старая, ошибка выводится в Output Log.
- Ошибка во время выполнения (`Scripts/Mover.zs:12: ...`) останавливает только этот экземпляр.

## Структура файла

Один файл = один `script Имя { ... }`. Внутри — поля и функции в любом порядке.

| Объявление | Смысл |
|---|---|
| `prop name = value` | редактируемое поле (число, bool, строка, `vec3(...)`); значение по умолчанию — литерал |
| `var name = expr` | поле экземпляра (состояние), инициализатор может использовать поля выше |
| `let name = expr` | то же, что `var` (для читаемости констант) |
| `fn name(a, b) { ... }` | функция; `start()` и `update(dt)` вызывает движок |

Аннотации перед `prop`: `@range(min, max)` — слайдер, `@tooltip("текст")`, `@color` — для `vec3` выбор цвета.
Типы можно указывать для документации: `prop speed: number = 1.0`, `fn f(x: number)` (не проверяются).

## Выражения и операторы

- Литералы: `1`, `2.5`, `1_000`, `1e-3`, `"строка\n"`, `true`, `false`, `nil`, списки `[1, 2, 3]`.
- Арифметика `+ - * / %` (`%` всегда неотрицателен при положительном делителе), сравнение `== != < <= > >=`,
  логика `and or not` (или `&& || !`). `a or b` возвращает первое «истинное» значение.
- Ложны только `nil`, `false` и `0`.
- `"a" + 1` — склейка строк (любое значение превращается в текст).
- Присваивание: `= += -= *= /=`, в том числе вглубь: `transform.position.y += 1`, `list[0].x = 5`.

## Операторы управления

```zs
if hp <= 0 { die() } elif hp < 20 { warn() } else { heal() }
while t < 1 { t += 0.1 }
for i in 0..10 { }          // 0, 1, ..., 9
for enemy in enemies { }    // элементы списка
break   continue   return value
```

Переменные `let`/`var` внутри функции видны до конца блока `{ }`. Поиск имени: локальная переменная → поле
скрипта → функция скрипта → глобальное имя движка. `self.name` явно обращается к полю/функции скрипта.

## Значения

| Тип | Пример | Члены и методы |
|---|---|---|
| number | `3.5` | — |
| string | `"hi"` | `len()`, `upper()`, `lower()`, `contains(s)`, `startsWith(s)`, `s[0]` |
| vec3 | `vec3(1, 2, 3)`, `vec3(5)` | `.x .y .z`, `length()`, `lengthSquared()`, `normalized()`, `dot(b)`, `cross(b)`, `distance(b)`, `lerp(b, t)`; `+ - * /` с vec3 и числами |
| quat | `quat.euler(0, 90, 0)` | `forward() right() up()`, `inverse()`, `euler()` (градусы), `rotate(v)`, `slerp(b, t)`; `q * v` поворачивает вектор, `q1 * q2` |
| list | `[1, "a"]` | `len()`, `push(v)`, `pop()`, `insert(i, v)`, `remove(i)`, `contains(v)`, `indexOf(v)`, `clear()`, `l[-1]` |

vec3 и quat копируются как значения; списки и строки — ссылки (`b = a; b.push(1)` меняет и `a`).

## Глобальные объекты движка

| Имя | Описание |
|---|---|
| `entity` (= `transform`) | своя сущность: `name`, `position`, `rotation` (quat; можно присвоить `vec3` углов в градусах), `scale`, `forward`/`right`/`up`, `alive`; `rotate(vec3 градусов)` / `rotate(ось, градусы)`, `translate(v)` (по своим осям; `translate(v, true)` — по мировым), `lookAt(точка)`, `destroy()` |
| `find("Имя")` | сущность по имени или `nil` (те же члены, что у `entity`); `entity.child("Имя")` — потомок по имени, `entity.parent` — родитель |
| `character` | Character Controller своей сущности: `move(vec3 скорость)` — горизонтальная скорость на этот кадр (м/с), `jump(скорость)` — прыжок, если стоит на земле; `grounded`, `velocity` |
| `input` | `key("W")` — удерживается, `pressed("Space")` / `released(...)` — в этом кадре, `mouse("left")`, `mousePressed("right")`; `move` — vec3 из WASD/стрелок/стика (x — вправо, z — вперёд, y — E/Q), `look` / `mouseDelta`, `mousePosition`, `wheel`; Input Actions проекта (Window > Input Actions): `action("Jump")` — удерживается, `actionPressed(...)` / `actionReleased(...)` — в этом кадре, `axis("MoveForward")` — значение оси. Имена клавиш: буквы, цифры, `Space`, `Enter`, `Escape`, `Shift`, `Ctrl`, `Alt`, `Tab`, `Up`, `F1`…`F12` |
| `time` | `dt`, `elapsed` (секунды с начала Play), `frame` |
| `audio` | `play("Sounds/x.wav"[, громкость])` → id, `playAt(файл, позиция[, громкость])` — 3D-звук в точке, `stop(id)`, `setVolume("Music" \| "SFX" \| "UI" \| "Voice" \| "Master", громкость)`; у сущности с Audio Source — `entity.playSound()` / `entity.stopSound()` |

Позиции и повороты — локальные (относительно родителя). Ось Y — вверх, +Z — вперёд, метры.

## Стандартная библиотека

`sin cos tan asin acos atan atan2 sqrt abs floor ceil round exp sign fract pow min max clamp lerp smoothstep`
(углы в радианах), `deg(rad)`, `rad(deg)`, `PI`, `random()` (0..1), `random(a, b)`, `randomInt(a, b)`,
`vec3(...)`, `quat.euler(pitch, yaw, roll)` (градусы), `quat.axisAngle(axis, градусы)`, `quat.lookAt(dir[, up])`,
`quat.identity`, `log(...)` / `print(...)` (в Output Log), `str(v)`, `num("1.5")`, `len(x)`, `type(v)`.

## Ограничения (версия 0.8.4)

- Нет замыканий и классов внутри файла; функции скрипта можно передавать как значения только внутри него.
- Бесконечный цикл останавливается после ~20 млн шагов за вызов, глубина рекурсии — 64.
- Коллизии персонажа пока по коробкам (AABB) мешей с флагом Collision; полноценная физика — M11.

## Персонаж и камера (шаблоны First Person / Third Person)

- **Character Controller**: позиция сущности — ступни; тело — коробка `2·radius × height`; ступеньки до
  `stepHeight` преодолеваются автоматически; гравитация, прыжок только с земли; при падении ниже точки старта
  на 100 м — возврат на старт. Меши самого персонажа (дочерние сущности) его не блокируют.
- **Camera**: в Play вид идёт из первой активной камеры. Клик по вьюпорту отдаёт игре мышь и клавиатуру
  (курсор захвачен), **Shift+F1** — отпустить мышь, **F8** — eject (камера редактора, игра продолжает идти),
  **Esc** — Stop.
- Готовые скрипты (`Content/Scripts` в шаблонах, `StarterContent/Scripts`): `FirstPersonController`,
  `ThirdPersonController`, `ThirdPersonCamera` — все параметры настраиваются в Details.
