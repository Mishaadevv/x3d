// Game UI: style sheets applied to the widgets, the animated Look (transitions, UI Animation presets, tweens) and
// gamepad / keyboard navigation.

#include "Core/Containers/Algorithm.h"
#include "Core/Containers/HashMap.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Log/Log.h"
#include "Core/String/Path.h"
#include "Core/Time/Time.h"
#include "Engine/GameUI.h"
#include "Platform/Input.h"
#include "Scene/Hierarchy.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogGameUI, Info);

namespace zx {

namespace {

// Widget components a style property can land in (an entity's widget first, its UI Element last).
enum Kind : u32
{
    KPanel,
    KText,
    KImage,
    KButton,
    KProgressBar,
    KSlider,
    KToggle,
    KInputField,
    KCanvas,
    KElement,
    KCount
};
const char* const KindNames[KCount] = {"Panel", "Text", "Image", "Button", "ProgressBar", "Slider", "Toggle", "InputField", "Canvas", "Element"};

const TypeInfo& KindType(u32 k)
{
    switch (k) {
    case KPanel: return TypeOf<UIPanel>();
    case KText: return TypeOf<UIText>();
    case KImage: return TypeOf<UIImage>();
    case KButton: return TypeOf<UIButton>();
    case KProgressBar: return TypeOf<UIProgressBar>();
    case KSlider: return TypeOf<UISlider>();
    case KToggle: return TypeOf<UIToggle>();
    case KInputField: return TypeOf<UIInputField>();
    case KCanvas: return TypeOf<UICanvas>();
    default: return TypeOf<UIElement>();
    }
}

void* KindPtr(ecs::World& world, ecs::Entity e, u32 k)
{
    switch (k) {
    case KPanel: return world.Get<UIPanel>(e);
    case KText: return world.Get<UIText>(e);
    case KImage: return world.Get<UIImage>(e);
    case KButton: return world.Get<UIButton>(e);
    case KProgressBar: return world.Get<UIProgressBar>(e);
    case KSlider: return world.Get<UISlider>(e);
    case KToggle: return world.Get<UIToggle>(e);
    case KInputField: return world.Get<UIInputField>(e);
    case KCanvas: return world.Get<UICanvas>(e);
    default: return world.Get<UIElement>(e);
    }
}

/// The interactive widget's Interactable (true for the rest).
bool Interactable(const ecs::World& world, ecs::Entity e)
{
    if (const UIButton* b = world.Get<const UIButton>(e))
        return b->interactable;
    if (const UISlider* s = world.Get<const UISlider>(e))
        return s->interactable;
    if (const UIToggle* t = world.Get<const UIToggle>(e))
        return t->interactable;
    if (const UIInputField* i = world.Get<const UIInputField>(e))
        return i->interactable;
    return true;
}

bool IsInteractive(const ecs::World& world, ecs::Entity e)
{
    return world.Has<UIButton>(e) || world.Has<UISlider>(e) || world.Has<UIToggle>(e) || world.Has<UIInputField>(e);
}

struct Resolved
{
    u32 kind = KCount;
    const FieldInfo* field = nullptr;
    i32 sub = -1; // component of a Vec2 (width, height, x, y)
};

/// Finds a (normalized) property in the widget component `kind`.
bool ResolveIn(u32 kind, StringView name, Resolved& out)
{
    String n(name);
    if (n == "background" || n == "backgroundcolor")
        n = "color";
    else if (n == "radius" || n == "borderradius")
        n = "cornerradius";
    else if (n == "fontweight")
        n = "weight";
    else if (n == "textalign")
        n = "align";
    else if (n == "textcolor" && kind == KText)
        n = "color";
    const TypeInfo& type = KindType(kind);
    if (kind == KElement && (n == "width" || n == "height" || n == "x" || n == "y")) {
        out.kind = kind;
        out.field = type.FindField(Name(n == "width" || n == "height" ? "size" : "translate"));
        out.sub = (n == "width" || n == "x") ? 0 : 1;
        return out.field != nullptr;
    }
    const FieldInfo* found = nullptr;
    type.ForEachField([&](const FieldInfo& f) {
        if (!found && !f.IsTransient() && NormalizeStyleName(f.name.View()) == n)
            found = &f;
    });
    if (!found)
        return false;
    out.kind = kind;
    out.field = found;
    out.sub = -1;
    return true;
}

/// The widget kinds `e` has, in the order properties are looked up.
u32 EntityKinds(const ecs::World& world, ecs::Entity e, u32* out)
{
    u32 n = 0;
    auto add = [&](bool has, u32 k) {
        if (has)
            out[n++] = k;
    };
    add(world.Has<UIButton>(e), KButton);
    add(world.Has<UISlider>(e), KSlider);
    add(world.Has<UIToggle>(e), KToggle);
    add(world.Has<UIInputField>(e), KInputField);
    add(world.Has<UIProgressBar>(e), KProgressBar);
    add(world.Has<UIText>(e), KText);
    add(world.Has<UIImage>(e), KImage);
    add(world.Has<UIPanel>(e), KPanel);
    add(world.Has<UICanvas>(e), KCanvas);
    add(world.Has<UIElement>(e), KElement);
    return n;
}

/// A reflected value in its own storage.
struct Boxed
{
    const TypeInfo* type = nullptr;
    alignas(16) u8 data[64];
    explicit Boxed(const TypeInfo& t) : type(&t)
    {
        ZX_ASSERT(t.size <= sizeof(data));
        t.construct(data);
    }
    ~Boxed() { type->destruct(data); }
    Boxed(const Boxed&) = delete;
    Boxed& operator=(const Boxed&) = delete;
};

/// A property's value parsed for one widget kind.
struct PropValue
{
    u8 state = 0; // 0 = not tried, 1 = not in this kind, 2 = ok, 3 = bad value
    Resolved where;
    UniquePtr<Boxed> value;
    f32 number = 0.0f; // sub-component values
};

struct PropCache
{
    PropValue kinds[KCount];
};

bool WriteValue(void* component, const Resolved& r, const PropValue& v)
{
    void* field = r.field->Ptr(component);
    if (r.sub >= 0) {
        static_cast<f32*>(field)[r.sub] = v.number;
        return true;
    }
    r.field->type->copyAssign(field, v.value->data);
    return true;
}

bool HasClass(StringView classes, StringView c)
{
    usize i = 0;
    while (i < classes.Size()) {
        while (i < classes.Size() && classes[i] == ' ')
            ++i;
        const usize start = i;
        while (i < classes.Size() && classes[i] != ' ')
            ++i;
        if (i > start && classes.SubStr(start, i - start).EqualsIgnoreCase(c))
            return true;
    }
    return false;
}

bool CompoundMatches(const ecs::World& world, ecs::Entity e, const UIStyleCompound& c)
{
    const UIElement* el = world.Get<const UIElement>(e);
    if (!el)
        return false;
    if (!c.type.Empty()) {
        bool ok = false;
        if (c.type.EqualsIgnoreCase("Element"))
            ok = true;
        else {
            u32 kinds[KCount];
            const u32 n = EntityKinds(world, e, kinds);
            for (u32 i = 0; i < n && !ok; ++i)
                ok = c.type.EqualsIgnoreCase(KindNames[kinds[i]]) ||
                     (kinds[i] == KProgressBar && c.type.EqualsIgnoreCase("Progress")) || (kinds[i] == KInputField && c.type.EqualsIgnoreCase("Input"));
        }
        if (!ok)
            return false;
    }
    if (!c.name.Empty()) {
        const scene::EntityName* n = world.Get<const scene::EntityName>(e);
        if (!n || n->value != c.name.View())
            return false;
    }
    for (const String& cls : c.classes)
        if (!HasClass(el->styleClass.View(), cls.View()))
            return false;
    if (HasAnyFlags(c.states, UIStyleState::Hover) && !el->hovered)
        return false;
    if (HasAnyFlags(c.states, UIStyleState::Pressed) && !el->pressed)
        return false;
    if (HasAnyFlags(c.states, UIStyleState::Focused) && !el->focused)
        return false;
    if (HasAnyFlags(c.states, UIStyleState::Disabled) && Interactable(world, e))
        return false;
    return true;
}

bool SelectorMatches(const ecs::World& world, ecs::Entity e, const UIStyleSelector& sel, Span<const ecs::Entity> ancestors)
{
    if (!CompoundMatches(world, e, sel.parts.Back()))
        return false;
    // Remaining parts right to left against the ancestors, nearest first.
    i64 part = i64(sel.parts.Size()) - 2;
    for (i64 a = i64(ancestors.Size()) - 1; a >= 0 && part >= 0; --a)
        if (CompoundMatches(world, ancestors[usize(a)], sel.parts[usize(part)]))
            --part;
    return part < 0;
}

} // namespace

struct GameUISystem::Override
{
    ecs::Entity entity;
    Resolved where;
    UniquePtr<Boxed> original;
    f32 number = 0.0f;
};

struct GameUISystem::StyleCache
{
    struct Sheet
    {
        String path; // as read
        UIStyleSheet sheet;
        Array<PropCache> props; // every property of every rule, in order
        Array<u32> firstProp;   // per rule
        i64 modified = 0;
        f64 checked = 0.0;
        bool reported = false;
    };
    HashMap<String, UniquePtr<Sheet>> sheets;
    HashMap<u64, bool> reportedProps; // (sheet, property) problems logged once
};

GameUISystem::GameUISystem() : m_styles(MakeUnique<StyleCache>()) {}
GameUISystem::~GameUISystem() = default;

f32 ApplyEasing(UIEasing easing, f32 t)
{
    t = Clamp(t, 0.0f, 1.0f);
    switch (easing) {
    case UIEasing::Linear: return t;
    case UIEasing::EaseIn: return t * t * t;
    case UIEasing::EaseOut: {
        const f32 u = 1.0f - t;
        return 1.0f - u * u * u;
    }
    case UIEasing::EaseInOut: return t < 0.5f ? 4.0f * t * t * t : 1.0f - Pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
    case UIEasing::Back: {
        const f32 c1 = 1.70158f, c3 = c1 + 1.0f, u = t - 1.0f;
        return 1.0f + c3 * u * u * u + c1 * u * u;
    }
    case UIEasing::Elastic:
        if (t <= 0.0f || t >= 1.0f)
            return t;
        return Pow(2.0f, -10.0f * t) * Sin((t * 10.0f - 0.75f) * TwoPi / 3.0f) + 1.0f;
    case UIEasing::Bounce: {
        const f32 n1 = 7.5625f, d1 = 2.75f;
        if (t < 1.0f / d1)
            return n1 * t * t;
        if (t < 2.0f / d1) {
            t -= 1.5f / d1;
            return n1 * t * t + 0.75f;
        }
        if (t < 2.5f / d1) {
            t -= 2.25f / d1;
            return n1 * t * t + 0.9375f;
        }
        t -= 2.625f / d1;
        return n1 * t * t + 0.984375f;
    }
    }
    return t;
}

void GameUISystem::RestoreOverrides(ecs::World& world)
{
    // Oldest last: a field overridden twice gets its very first value back.
    for (i64 i = i64(m_overrides.Size()) - 1; i >= 0; --i) {
        const Override& o = *m_overrides[usize(i)];
        if (!world.IsAlive(o.entity))
            continue;
        void* component = KindPtr(world, o.entity, o.where.kind);
        if (!component)
            continue;
        void* field = o.where.field->Ptr(component);
        if (o.where.sub >= 0)
            static_cast<f32*>(field)[o.where.sub] = o.number;
        else
            o.where.field->type->copyAssign(field, o.original->data);
    }
    m_overrides.Clear();
}

void GameUISystem::ApplyStyles(ecs::World& world, ecs::Entity canvas)
{
    const UICanvas* cv = world.Get<const UICanvas>(canvas);
    if (!cv || cv->style.Empty())
        return;
    // Load / hot-reload the sheet (checked twice a second).
    UniquePtr<StyleCache::Sheet>* slot = m_styles->sheets.Find(cv->style);
    if (!slot) {
        m_styles->sheets.InsertOrAssign(cv->style, MakeUnique<StyleCache::Sheet>());
        slot = m_styles->sheets.Find(cv->style);
        (*slot)->checked = -1.0;
    }
    StyleCache::Sheet& sh = **slot;
    const f64 now = Clock::NowSeconds();
    if (now - sh.checked > 0.5) {
        sh.checked = now;
        const String path = cv->style.View().StartsWith("/") || contentRoot.Empty() ? String(cv->style) : Path::Join(contentRoot, cv->style);
        platform::FileInfo info;
        const i64 modified = fs::GetInfo(path, info) ? info.modifiedTime : -1;
        if (modified != sh.modified) {
            sh.modified = modified;
            String text;
            sh.sheet = UIStyleSheet();
            if (modified >= 0 && fs::ReadTextFile(path, text))
                ParseUIStyle(text.View(), sh.sheet);
            else
                sh.sheet.errors.PushBack(String::Format("can't read {}", path));
            sh.props.Clear();
            sh.firstProp.Clear();
            for (const UIStyleRule& r : sh.sheet.rules) {
                sh.firstProp.PushBack(u32(sh.props.Size()));
                for (usize k = 0; k < r.properties.Size(); ++k)
                    sh.props.PushBack(PropCache{});
            }
            // Problems: logged once per load, kept for the editor.
            m_styleErrors.RemoveIf([&](const String& e) { return e.View().StartsWith(Path::FileName(cv->style)); });
            for (const String& e : sh.sheet.errors) {
                m_styleErrors.PushBack(String::Format("{} {}", Path::FileName(cv->style), e));
                ZX_LOG_WARN(LogGameUI, "{} {}", cv->style, e);
            }
            if (modified >= 0)
                ZX_LOG_INFO(LogGameUI, "Style {}: {} rules", cv->style, sh.sheet.rules.Size());
            m_styles->reportedProps.Clear();
        }
    }
    if (sh.sheet.rules.Empty())
        return;

    struct Match
    {
        u32 specificity;
        u32 rule;
        bool states;
    };
    Array<Match> matches;
    Array<ecs::Entity> ancestors;
    auto apply = [&](ecs::Entity e, const Match& m) {
        const UIStyleRule& rule = sh.sheet.rules[m.rule];
        u32 kinds[KCount];
        const u32 kindCount = EntityKinds(world, e, kinds);
        for (usize pi = 0; pi < rule.properties.Size(); ++pi) {
            const UIStyleProperty& prop = rule.properties[pi];
            PropCache& cache = sh.props[sh.firstProp[m.rule] + pi];
            bool written = false, bad = false;
            for (u32 ki = 0; ki < kindCount && !written; ++ki) {
                PropValue& pv = cache.kinds[kinds[ki]];
                if (pv.state == 0) {
                    pv.state = 1;
                    if (ResolveIn(kinds[ki], prop.name.View(), pv.where)) {
                        bool ok;
                        if (pv.where.sub >= 0) {
                            const TypeInfo& f32Type = TypeOf<f32>();
                            ok = ParseStyleValue(prop.value.View(), f32Type, &pv.number);
                        } else {
                            pv.value = MakeUnique<Boxed>(*pv.where.field->type);
                            ok = ParseStyleValue(prop.value.View(), *pv.where.field->type, pv.value->data);
                        }
                        pv.state = ok ? 2 : 3;
                    }
                }
                if (pv.state == 3)
                    bad = true;
                if (pv.state != 2)
                    continue;
                void* component = KindPtr(world, e, kinds[ki]);
                if (!component)
                    continue;
                if (m.states) {
                    // State rules are undone at the end of the frame (the widget keeps its own value).
                    UniquePtr<Override> o = MakeUnique<Override>();
                    o->entity = e;
                    o->where = pv.where;
                    void* field = pv.where.field->Ptr(component);
                    if (pv.where.sub >= 0)
                        o->number = static_cast<f32*>(field)[pv.where.sub];
                    else {
                        o->original = MakeUnique<Boxed>(*pv.where.field->type);
                        pv.where.field->type->copyAssign(o->original->data, field);
                    }
                    m_overrides.PushBack(move(o));
                }
                written = WriteValue(component, pv.where, pv);
            }
            if (!written) {
                const u64 key = (u64(m.rule) << 32) | u64(pi);
                if (!m_styles->reportedProps.Find(key)) {
                    m_styles->reportedProps.InsertOrAssign(key, true);
                    const String msg = String::Format("{} line {}: {} '{}: {}'", Path::FileName(cv->style), prop.line,
                                                      bad ? "bad value in" : "no such property for this widget in", prop.name, prop.value);
                    m_styleErrors.PushBack(msg);
                    ZX_LOG_WARN(LogGameUI, "{}", msg);
                }
            }
        }
    };
    Function<void(ecs::Entity)> visit = [&](ecs::Entity e) {
        if (!world.Has<UIElement>(e))
            return;
        matches.Clear();
        for (u32 ri = 0; ri < sh.sheet.rules.Size(); ++ri) {
            const UIStyleRule& rule = sh.sheet.rules[ri];
            u32 best = 0;
            bool found = false, states = false;
            for (const UIStyleSelector& sel : rule.selectors)
                if (SelectorMatches(world, e, sel, ancestors.View()) && (!found || sel.specificity > best)) {
                    best = sel.specificity;
                    states = sel.HasStates();
                    found = true;
                }
            if (found)
                matches.PushBack(Match{best, ri, states});
        }
        StableSort(matches, [](const Match& a, const Match& b) { return a.specificity < b.specificity; });
        for (const Match& m : matches)
            if (!m.states)
                apply(e, m);
        for (const Match& m : matches)
            if (m.states)
                apply(e, m);
        ancestors.PushBack(e);
        for (ecs::Entity c : scene::GetChildren(world, e))
            visit(c);
        ancestors.PopBack();
    };
    visit(canvas);
}

void GameUISystem::Update(ecs::World& world, f32 dt)
{
    m_playing = true;
    RestoreOverrides(world);
    if (!cursorRequested)
        m_navActive = false;
    m_navPressFlash = Max(m_navPressFlash - dt, 0.0f);
    // Transitions of the Look towards the styled target.
    world.Query<UIElement>().Each([&](ecs::Entity, UIElement& el) {
        const f32 k = el.transition > 0.0f ? 1.0f - Exp(-dt * 4.6f / el.transition) : 1.0f;
        el.shownOpacity = Lerp(el.shownOpacity, el.targetOpacity, k);
        el.shownScale = Lerp(el.shownScale, el.targetScale, k);
        el.shownTranslate = Lerp(el.shownTranslate, el.targetTranslate, k);
    });
    // UI Animation presets.
    world.Query<UIAnimation, UIElement>().Each([&](ecs::Entity, UIAnimation& a, UIElement& el) {
        if (a.playOnShow && el.visible && !a.wasVisible)
            a.time = 0.0f;
        a.wasVisible = el.visible;
        if (a.time < 0.0f)
            return;
        a.time += dt;
        const f32 duration = Max(a.duration, 0.01f);
        const f32 local = a.time - a.delay;
        const bool looping = a.loop || a.preset == UIAnimationPreset::Pulse || a.preset == UIAnimationPreset::Float || a.preset == UIAnimationPreset::Blink;
        f32 t = Clamp(local / duration, 0.0f, 1.0f);
        if (looping && local > 0.0f)
            t = Frac(local / duration);
        const f32 e = ApplyEasing(a.easing, t);
        f32 opacity = 1.0f, scale = 1.0f;
        Vec2 translate;
        const f32 cycle = Sin(TwoPi * Max(local, 0.0f) / duration);
        switch (a.preset) {
        case UIAnimationPreset::FadeIn: opacity = e; break;
        case UIAnimationPreset::FadeOut: opacity = 1.0f - e; break;
        case UIAnimationPreset::SlideInLeft: translate.x = -a.distance * (1.0f - e); opacity = Saturate(e * 2.0f); break;
        case UIAnimationPreset::SlideInRight: translate.x = a.distance * (1.0f - e); opacity = Saturate(e * 2.0f); break;
        case UIAnimationPreset::SlideInTop: translate.y = -a.distance * (1.0f - e); opacity = Saturate(e * 2.0f); break;
        case UIAnimationPreset::SlideInBottom: translate.y = a.distance * (1.0f - e); opacity = Saturate(e * 2.0f); break;
        case UIAnimationPreset::PopIn: scale = Lerp(0.6f, 1.0f, e); opacity = Saturate(t * 3.0f); break;
        case UIAnimationPreset::Pulse: scale = 1.0f + 0.06f * cycle; break;
        case UIAnimationPreset::Float: translate.y = a.distance * 0.1f * cycle; break;
        case UIAnimationPreset::Blink: opacity = 0.65f + 0.35f * Cos(TwoPi * Max(local, 0.0f) / duration); break;
        case UIAnimationPreset::Shake: translate.x = Sin(Max(local, 0.0f) * 45.0f) * a.distance * 0.1f * (1.0f - t); break;
        }
        el.animOpacity = opacity;
        el.animScale = scale;
        el.animTranslate = translate;
        if (!looping && local >= duration)
            a.time = -1.0f; // done: the last values stay (Fade Out stays invisible)
    });
    // Tweens.
    for (usize i = 0; i < m_tweens.Size();) {
        Tween& tw = m_tweens[i];
        void* component = world.IsAlive(tw.entity) ? KindPtr(world, tw.entity, tw.kind) : nullptr;
        if (!component) {
            m_tweens.RemoveAt(i);
            continue;
        }
        tw.elapsed += dt;
        if (tw.elapsed < tw.delay) {
            ++i;
            continue;
        }
        const f32 t = tw.duration > 0.0f ? Clamp((tw.elapsed - tw.delay) / tw.duration, 0.0f, 1.0f) : 1.0f;
        const f32 e = ApplyEasing(tw.easing, t);
        f32* field = static_cast<f32*>(tw.field->Ptr(component));
        if (tw.field->type->kind == TypeKind::Int32)
            *reinterpret_cast<i32*>(field) = i32(Round(Lerp(tw.from[0], tw.to[0], e)));
        else
            for (u32 c = 0; c < tw.count; ++c)
                field[tw.sub >= 0 ? tw.sub : i32(c)] = Lerp(tw.from[c], tw.to[c], e);
        if (t >= 1.0f)
            m_tweens.RemoveAt(i);
        else
            ++i;
    }
}

bool GameUISystem::Animate(ecs::World& world, ecs::Entity e, StringView property, Span<const f32> to, f32 seconds, UIEasing easing, f32 delay, String* error)
{
    if (!world.IsAlive(e) || to.Empty()) {
        if (error)
            *error = "no such entity";
        return false;
    }
    const String name = NormalizeStyleName(property);
    u32 kinds[KCount];
    const u32 n = EntityKinds(world, e, kinds);
    Resolved where;
    bool found = false;
    for (u32 i = 0; i < n && !found; ++i)
        found = ResolveIn(kinds[i], name.View(), where);
    if (!found) {
        if (error)
            *error = String::Format("this widget has no property '{}'", property);
        return false;
    }
    const TypeInfo& type = *where.field->type;
    Tween tw;
    tw.entity = e;
    tw.kind = where.kind;
    tw.field = where.field;
    tw.sub = where.sub;
    tw.duration = Max(seconds, 0.0f);
    tw.delay = Max(delay, 0.0f);
    tw.easing = easing;
    void* component = KindPtr(world, e, where.kind);
    const f32* current = static_cast<const f32*>(where.field->Ptr(component));
    if (where.sub >= 0 || type.kind == TypeKind::Float) {
        tw.count = 1;
        tw.from[0] = current[where.sub >= 0 ? where.sub : 0];
    } else if (type.kind == TypeKind::Int32) {
        tw.count = 1;
        tw.from[0] = f32(*reinterpret_cast<const i32*>(current));
    } else if (&type == &TypeOf<LinearColor>() || type.name == Name("Vec4")) {
        if (to.Size() < 4) {
            if (error)
                *error = String::Format("'{}' needs 4 numbers (r, g, b, a)", property);
            return false;
        }
        tw.count = 4;
        for (u32 c = 0; c < 4; ++c)
            tw.from[c] = current[c];
    } else if (type.name == Name("Vec2")) {
        tw.count = to.Size() >= 2 ? 2 : 1;
        for (u32 c = 0; c < 2; ++c)
            tw.from[c] = current[c];
        if (tw.count == 1) { // one number: both
            tw.count = 2;
            tw.to[1] = to[0];
        }
    } else {
        if (error)
            *error = String::Format("'{}' can't be animated (numbers and colors only)", property);
        return false;
    }
    for (u32 c = 0; c < Min<u32>(tw.count, u32(to.Size())); ++c)
        tw.to[c] = to[c];
    m_tweens.RemoveIf([&](const Tween& t) { return t.entity == e && t.field == tw.field && t.sub == tw.sub; });
    m_tweens.PushBack(tw);
    return true;
}

bool GameUISystem::PlayAnimation(ecs::World& world, ecs::Entity e)
{
    UIAnimation* a = world.IsAlive(e) ? world.Get<UIAnimation>(e) : nullptr;
    if (!a)
        return false;
    a->time = 0.0f;
    return true;
}

void GameUISystem::Navigate(ecs::World& world, const InputState& input, f32 dt)
{
    const GamepadState& pad = input.Gamepad(0);
    const bool padNow[6] = {pad.buttons[u32(GamepadButton::DPadUp)],   pad.buttons[u32(GamepadButton::DPadDown)],
                            pad.buttons[u32(GamepadButton::DPadLeft)], pad.buttons[u32(GamepadButton::DPadRight)],
                            pad.buttons[u32(GamepadButton::A)],        pad.buttons[u32(GamepadButton::B)]};
    bool padPressed[6];
    for (u32 i = 0; i < 6; ++i) {
        padPressed[i] = padNow[i] && !m_padPrev[i];
        m_padPrev[i] = padNow[i];
    }
    if (Dot(input.MouseDelta(), input.MouseDelta()) > 0.0f || input.WasMousePressed(MouseButton::Left))
        m_navActive = false; // the mouse took over
    Vec2 dir;
    if (input.WasKeyPressed(Key::Up) || padPressed[0])
        dir.y = -1.0f;
    else if (input.WasKeyPressed(Key::Down) || padPressed[1])
        dir.y = 1.0f;
    else if (input.WasKeyPressed(Key::Left) || padPressed[2])
        dir.x = -1.0f;
    else if (input.WasKeyPressed(Key::Right) || padPressed[3])
        dir.x = 1.0f;
    // Left stick with auto-repeat.
    const Vec2 stick(pad.leftStick.x, -pad.leftStick.y);
    if (Length(stick) > 0.55f) {
        m_navRepeat -= dt;
        if (!m_stickHeld || m_navRepeat <= 0.0f) {
            if (Abs(stick.x) > Abs(stick.y))
                dir = Vec2(Sign(stick.x), 0.0f);
            else
                dir = Vec2(0.0f, Sign(stick.y));
            m_navRepeat = m_stickHeld ? 0.14f : 0.4f;
            m_stickHeld = true;
        }
    } else {
        m_stickHeld = false;
    }
    const bool accept = input.WasKeyPressed(Key::Enter) || input.WasKeyPressed(Key::KeypadEnter) || input.WasKeyPressed(Key::Space) || padPressed[4];
    const bool back = input.WasKeyPressed(Key::Escape) || padPressed[5];
    const bool skip = m_skipNav || WantsKeyboard();
    m_skipNav = false;

    // Navigable widgets on screen, in drawing order.
    Array<ecs::Entity> list;
    Array<ecs::Entity> canvases;
    world.Query<const UICanvas>().Each([&](ecs::Entity e, const UICanvas& c) {
        if (!c.worldSpace && c.onScreen)
            canvases.PushBack(e);
    });
    StableSort(canvases, [&](ecs::Entity a, ecs::Entity b) { return world.Get<const UICanvas>(a)->order < world.Get<const UICanvas>(b)->order; });
    Function<void(ecs::Entity)> visit = [&](ecs::Entity e) {
        const UIElement* el = world.Get<const UIElement>(e);
        if (!el || !el->visible || el->drawOpacity <= 0.01f)
            return;
        if (el->navigable && IsInteractive(world, e) && Interactable(world, e) && el->rect.Width() > 0.0f)
            list.PushBack(e);
        for (ecs::Entity c : scene::GetChildren(world, e))
            visit(c);
    };
    for (ecs::Entity c : canvases)
        visit(c);
    bool valid = false;
    for (ecs::Entity e : list)
        valid = valid || e == m_navFocus;

    auto setFlags = [&]() {
        world.Query<UIElement>().Each([&](ecs::Entity e, UIElement& el) {
            el.focused = m_navActive && e == m_navFocus;
            if (el.focused) {
                el.hovered = true;
                el.pressed = el.pressed || m_navPressFlash > 0.0f;
            }
        });
    };
    if (skip) {
        setFlags();
        return;
    }
    if (back)
        m_events.PushBack(UIEvent{UIEvent::Type::Back, valid ? m_navFocus : ecs::Entity(), 0.0f, {}});
    if ((dir.x == 0.0f && dir.y == 0.0f) && !accept) {
        setFlags();
        return;
    }
    if (list.Empty()) {
        setFlags();
        return;
    }
    if (!valid || !m_navActive) {
        // The first press shows the focus (on the last focused widget, or the first one).
        if (!valid)
            m_navFocus = list[0];
        m_navActive = true;
        setFlags();
        return;
    }
    const UIElement& cur = *world.Get<const UIElement>(m_navFocus);
    if (dir.x != 0.0f || dir.y != 0.0f) {
        if (UISlider* sl = world.Get<UISlider>(m_navFocus); sl && dir.x != 0.0f) {
            const f32 v = Clamp(sl->value + dir.x * (sl->maxValue - sl->minValue) * 0.05f, Min(sl->minValue, sl->maxValue), Max(sl->minValue, sl->maxValue));
            if (v != sl->value) {
                sl->value = v;
                m_events.PushBack(UIEvent{UIEvent::Type::Changed, m_navFocus, v, {}});
            }
        } else {
            // The nearest widget in that direction (sideways distance counts double).
            const Vec2 from = cur.rect.Center();
            ecs::Entity best;
            f32 bestScore = 0.0f;
            for (ecs::Entity e : list) {
                if (e == m_navFocus)
                    continue;
                const Vec2 d = world.Get<const UIElement>(e)->rect.Center() - from;
                const f32 along = Dot(d, dir);
                if (along <= 1.0f)
                    continue;
                const f32 side = Abs(d.x * dir.y - d.y * dir.x);
                const f32 score = along + side * 2.0f;
                if (!best || score < bestScore) {
                    best = e;
                    bestScore = score;
                }
            }
            if (best)
                m_navFocus = best;
        }
    }
    if (accept) {
        if (world.Has<UIButton>(m_navFocus)) {
            m_events.PushBack(UIEvent{UIEvent::Type::Click, m_navFocus, 0.0f, {}});
            m_navPressFlash = 0.12f;
        } else if (UIToggle* tg = world.Get<UIToggle>(m_navFocus)) {
            tg->on = !tg->on;
            m_events.PushBack(UIEvent{UIEvent::Type::Changed, m_navFocus, tg->on ? 1.0f : 0.0f, {}});
        } else if (world.Has<UIInputField>(m_navFocus)) {
            m_focused = m_navFocus; // type into it
        }
    }
    setFlags();
}

} // namespace zx
