// Animation Viewer and AnimGraph Editor (see AnimationEditor.h).

#include "AnimationEditor.h"
#include "Editor.h"
#include "Core/Containers/Algorithm.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Core/Time/Time.h"
#include "Engine/SceneRendering.h"
#include "Scene/Hierarchy.h"

namespace zx::editor {

ZX_DEFINE_LOG_CATEGORY_STATIC(LogAnimationEditor, Info);

using namespace render;

// --- Preview ----------------------------------------------------------------------------------------------------------------

AnimationPreview::AnimationPreview(rhi::Device& device, RenderAssets& renderAssets, asset::AssetManager& assets)
    : m_device(device), m_renderAssets(renderAssets), m_assets(assets)
{
}

AnimationPreview::~AnimationPreview() { m_device.Destroy(m_texture); }

bool AnimationPreview::Init()
{
    m_renderer.Reset(new ViewportRenderer(m_device, m_renderAssets));
    if (!m_renderer->Init(true, rhi::Format::R8G8B8A8_UNORM))
        return false;
    m_animation.Reset(new AnimationSystem(m_assets));
    m_renderer->SetAnimationSystem(m_animation.Get());
    ViewportSettings& s = m_renderer->Settings();
    s.renderScale = 1.0f;
    s.taa.enabled = false;
    scene::InstallHierarchyHooks(m_world);
    // A floor to stand on (shadows show the feet).
    const ecs::Entity floor = scene::CreateNode(m_world, "Floor", Transform(Vec3(0.0f, -0.01f, 0.0f), Quat(), Vec3(8.0f, 0.02f, 8.0f)));
    MeshRenderer mr;
    mr.mesh = builtin::Mesh(BuiltinShape::Cube);
    m_world.Add(floor, mr);
    rhi::TextureDesc td;
    td.width = Width;
    td.height = Height;
    td.format = rhi::Format::R8G8B8A8_UNORM;
    td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    td.debugName = "AnimationPreview";
    m_texture = m_device.CreateTexture(td);
    return m_texture != nullptr;
}

bool AnimationPreview::SetModel(asset::AssetId prefab)
{
    if (m_model == prefab && m_root)
        return true;
    const asset::PrefabAsset* p = m_assets.GetPrefab(prefab);
    if (!p)
        return false;
    if (m_root)
        scene::DestroyHierarchy(m_world, m_root);
    m_model = prefab;
    m_root = InstantiatePrefab(m_world, *p, {}, {}, &m_assets);
    m_animated = {};
    m_world.Query<Animator>().Each([&](ecs::Entity e, Animator& a) {
        a.previewInEditor = true;
        a.graph.Clear(); // the viewer plays clips directly
        m_animated = e;
    });
    scene::UpdateWorldTransforms(m_world);
    // Frame the model: its mesh bounds (bind pose) once they are resident.
    m_focus = Vec3(0.0f, 0.9f, 0.0f);
    distance = 3.4f;
    return true;
}

Span<const asset::AssetId> AnimationPreview::Clips() const
{
    const Animator* a = m_animated ? m_world.Get<const Animator>(m_animated) : nullptr;
    return a ? a->clips.View() : Span<const asset::AssetId>();
}

void AnimationPreview::Play(asset::AssetId clip)
{
    if (Animator* a = m_animated ? m_world.Get<Animator>(m_animated) : nullptr)
        a->clip = clip;
}

void AnimationPreview::Update(f32 dt, bool playing, f32 speed, bool loop, f32& time)
{
    Animator* a = m_animated ? m_world.Get<Animator>(m_animated) : nullptr;
    if (!a)
        return;
    a->loop = loop;
    a->speed = speed;
    a->blendTime = 0.0f; // switching clips in the viewer shows the new one at once
    if (!playing)
        m_animation->SetTime(m_animated, time);
    m_animation->Tick(m_world, playing ? dt : 0.0f, true);
    time = m_animation->Time(m_animated);
    if (const asset::AnimationClip* clip = m_assets.GetAnimation(a->clip); clip && clip->duration > 0.0f)
        time = loop ? time - clip->duration * Floor(time / clip->duration) : Min(time, clip->duration);
    // Frame on the mesh (first frame it is resident).
    if (const MeshRenderer* mr = m_world.Get<const MeshRenderer>(m_animated))
        if (const GpuMesh* mesh = m_renderAssets.GetMesh(mr->mesh)) {
            const Mat4& w = m_world.Get<const scene::WorldTransform>(m_animated)->matrix;
            AABB box;
            for (u32 i = 0; i < 8; ++i)
                box.Expand(w.TransformPoint(mesh->bounds.Corner(i)));
            const Vec3 focus = box.Center();
            if (LengthSq(focus - m_focus) > 1e-4f && box.IsValid()) {
                m_focus = focus;
                distance = Max(Length(box.Extents()) * 2.4f, 1.0f);
            }
        }
}

void AnimationPreview::AddPasses(RenderGraph& graph, Array<RGTexture>& sampled, f32 dt)
{
    if (!m_texture)
        return;
    scene::UpdateWorldTransforms(m_world);
    const Vec3 eye = m_focus + Vec3(Sin(yaw) * Cos(pitch), Sin(pitch), Cos(yaw) * Cos(pitch)) * distance;
    ViewportCamera cam;
    cam.view = Mat4::LookAt(eye, m_focus, Vec3::Up());
    cam.position = eye;
    cam.fovY = DegToRad(35.0f);
    cam.nearPlane = 0.05f;
    ViewportEnvironment env;
    env.sunElevation = DegToRad(40.0f);
    env.sunAzimuth = DegToRad(150.0f);
    env.autoExposure = false;
    const RGTexture target = graph.ImportTexture(m_texture, rhi::ResourceState::ShaderResource);
    m_renderer->AddPasses(graph, m_world, cam, env, target, UVec2(Width, Height), dt);
    sampled.PushBack(target);
}

namespace {

/// First open: the given size (logical units), at most 90 % of the window, centered.
void PlaceWindowFirstTime(Editor& ed, Vec2 size)
{
    const f32 dpi = ui::GetCurrentContext()->GetScale();
    const Vec2 display = Vec2(f32(ed.swapChain->GetSize().x), f32(ed.swapChain->GetSize().y)) / dpi;
    size = Vec2(Min(size.x, display.x * 0.9f), Min(size.y, display.y * 0.9f));
    ui::SetNextWindowSize(size, ui::Cond::FirstUse);
    ui::SetNextWindowPos((display - size) * 0.5f, ui::Cond::FirstUse);
}

Array<String> AnimationClipNames(Editor& ed)
{
    Array<String> names;
    if (!ed.assets)
        return names;
    Array<asset::AssetId> ids;
    ed.assets->GetAssetIds(ids);
    for (const asset::AssetId& id : ids)
        if (ed.assets->GetType(id) == asset::AssetType::Animation) {
            const String n = ed.assets->GetName(id);
            bool known = n.Empty();
            for (const String& m : names)
                known |= m.View().EqualsIgnoreCase(n);
            if (!known)
                names.PushBack(n);
        }
    Sort(names, [](const String& a, const String& b) { return a < b; });
    return names;
}

/// Combo over `names` editing `value` (free text stays possible through the field below it).
bool NameCombo(StringView id, String& value, Span<const String> names)
{
    Array<StringView> views;
    i32 current = -1;
    for (usize i = 0; i < names.Size(); ++i) {
        views.PushBack(names[i].View());
        if (names[i].View().EqualsIgnoreCase(value))
            current = i32(i);
    }
    if (views.Empty())
        return ui::InputText(id, value);
    if (current < 0) {
        views.PushBack(value.View());
        current = i32(views.Size() - 1);
    }
    if (ui::Combo(id, &current, views.View()) && current >= 0 && usize(current) < names.Size() && names[usize(current)] != value) {
        value = names[usize(current)];
        return true;
    }
    return false;
}

} // namespace

// --- Animation Viewer -------------------------------------------------------------------------------------------------------

void OpenAnimationViewer(Editor& ed, StringView modelPath)
{
    AnimationViewerState& st = ed.animViewer;
    if (!ed.assets)
        return;
    if (st.model != modelPath || !st.preview) {
        st.model = String(modelPath);
        st.prefab = ed.assets->LoadPath(modelPath);
        st.preview.Reset(new AnimationPreview(*ed.device, *ed.renderAssets, *ed.assets));
        if (!st.preview->Init())
            st.preview.Reset();
        st.clip = -1;
        st.time = 0.0f;
        st.playing = true;
    }
    st.open = true;
    ui::FocusDockedWindow(AnimationViewerTitle);
}

void RenderAnimationPreview(Editor& ed, RenderGraph& graph, Array<RGTexture>& sampled)
{
    AnimationViewerState& st = ed.animViewer;
    if (!st.open || !st.preview || !st.preview->SetModel(st.prefab))
        return;
    st.preview->AddPasses(graph, sampled, ed.dt);
}

void DrawAnimationViewer(Editor& ed)
{
    AnimationViewerState& st = ed.animViewer;
    if (!st.open)
        return;
    PlaceWindowFirstTime(ed, Vec2(960.0f, 620.0f));
    bool open = true;
    if (!ui::Begin(AnimationViewerTitle, &open, ui::WindowFlags::NoScrollbar)) {
        ui::End();
        st.open = open;
        return;
    }
    st.open = open;
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    ui::Text(String::Format(ZX_ICON_BOX "  {}", Path::FileName(st.model)));
    if (!st.preview || !st.preview->SetModel(st.prefab)) {
        ui::TextDisabled(st.preview ? "Loading the model..." : "The preview could not be created");
        ui::End();
        return;
    }
    const Span<const asset::AssetId> clips = st.preview->Clips();
    if (clips.Empty()) {
        ui::TextDisabled("This model has no animation clips (no skin, or no animations in the file)");
        ui::End();
        return;
    }
    if (st.clip < 0 || usize(st.clip) >= clips.Size()) {
        st.clip = 0;
        for (usize c = 0; c < clips.Size(); ++c)
            if (ed.assets->GetName(clips[c]).View().EqualsIgnoreCase("Idle"))
                st.clip = i32(c);
        st.preview->Play(clips[usize(st.clip)]);
        st.time = 0.0f;
    }
    const asset::AnimationClip* clip = ed.assets->GetAnimation(clips[usize(st.clip)]);
    const f32 duration = clip ? clip->duration : 0.0f;
    st.preview->Update(ed.dt, st.playing, st.speed, st.loop, st.time);

    // Layout: clip list | preview, timeline below.
    const Rect area = ui::GetWindowContentRect();
    const f32 listW = Round(200.0f * s), barH = Round(40.0f * s);
    const Vec2 start = ui::GetCursorPos();
    if (ui::BeginChild("##clips", Vec2(listW / s, (area.max.y - start.y - barH) / s), true)) {
        ui::SeparatorText("Clips");
        for (usize c = 0; c < clips.Size(); ++c) {
            const asset::AnimationClip* cc = ed.assets->GetAnimation(clips[c]);
            const String label = String::Format("{}##clip{}", ed.assets->GetName(clips[c]), c);
            if (ui::Selectable(label, i32(c) == st.clip)) {
                st.clip = i32(c);
                st.preview->Play(clips[c]);
                st.time = 0.0f;
                st.playing = true;
            }
            if (cc && ui::IsItemHovered())
                ui::SetTooltip(String::Format("{:.2f} s, {} tracks", cc->duration, cc->tracks.Size()));
        }
    }
    ui::EndChild();
    const Rect previewArea(Vec2(start.x + listW + Round(8.0f * s), start.y), Vec2(area.max.x, area.max.y - barH));
    const f32 aspect = f32(AnimationPreview::Width) / f32(AnimationPreview::Height);
    Vec2 size(previewArea.Width(), previewArea.Width() / aspect);
    if (size.y > previewArea.Height())
        size = Vec2(previewArea.Height() * aspect, previewArea.Height());
    const Rect image(previewArea.Center() - size * 0.5f, previewArea.Center() + size * 0.5f);
    ui::GetWindowDrawList().AddImage(st.preview->Srv(), image, Vec2(0, 0), Vec2(1, 1), Color(255, 255, 255), 6.0f * s);
    ui::SetCursorPos(image.min);
    ui::InvisibleButton("##anim_preview", image.Size() / s);
    if (ui::IsItemActive()) { // orbit
        st.preview->yaw -= ed.input.MouseDelta().x * 0.01f;
        st.preview->pitch = Clamp(st.preview->pitch + ed.input.MouseDelta().y * 0.01f, -0.6f, 1.3f);
    }
    if (ui::IsItemHovered() && ed.input.MouseWheel() != 0.0f)
        st.preview->distance = Clamp(st.preview->distance * Pow(0.88f, ed.input.MouseWheel()), 0.5f, 50.0f);
    if (ui::IsItemHovered())
        ui::SetTooltip("Drag to orbit, wheel to zoom");

    // Timeline.
    ui::SetCursorPos(Vec2(start.x, area.max.y - barH + Round(6.0f * s)));
    if (ui::IconButton(st.playing ? ZX_ICON_PAUSE : ZX_ICON_PLAY, st.playing ? "Pause" : "Play"))
        st.playing = !st.playing;
    ui::SameLine();
    ui::SetNextItemWidth(-Round(330.0f));
    f32 t = st.time;
    if (ui::SliderFloat("##time", &t, 0.0f, Max(duration, 1e-3f), "{:.2f} s")) {
        st.time = t;
        st.playing = false; // scrubbing pauses
    }
    ui::SameLine();
    ui::AlignTextToFramePadding();
    ui::TextDisabled(String::Format("/ {:.2f} s", duration));
    ui::SameLine();
    ui::SetNextItemWidth(110.0f);
    ui::DragFloat("##speed", &st.speed, 0.01f, 0.0f, 4.0f, "Speed {:.2f}x");
    ui::SameLine();
    ui::Checkbox("Loop", &st.loop);
    ui::End();
}

// --- AnimGraph Editor -------------------------------------------------------------------------------------------------------

namespace {

constexpr f32 NodeW = 170.0f, NodeH = 50.0f;
constexpr usize MaxUndo = 100;

struct Canvas
{
    Rect rect;
    Vec2 pan;
    f32 scale = 1.0f;
    Vec2 ToScreen(Vec2 p) const { return rect.min + (p + pan) * scale; }
    Vec2 ToGraph(Vec2 q) const { return (q - rect.min) / scale - pan; }
};

Vec2 NodePos(const AnimGraph& g, i32 state) { return state < 0 ? Vec2(g.anyX, g.anyY) : Vec2(g.states[usize(state)].x, g.states[usize(state)].y); }

/// Point where the segment from the node center towards `dir` leaves the node box.
Vec2 BoxExit(Vec2 center, Vec2 dir, Vec2 half)
{
    const f32 tx = Abs(dir.x) > 1e-6f ? half.x / Abs(dir.x) : 1e9f;
    const f32 ty = Abs(dir.y) > 1e-6f ? half.y / Abs(dir.y) : 1e9f;
    return center + dir * Min(tx, ty);
}

f32 SegmentDistance(Vec2 p, Vec2 a, Vec2 b)
{
    const Vec2 ab = b - a;
    const f32 t = Clamp(Dot(p - a, ab) / Max(Dot(ab, ab), 1e-6f), 0.0f, 1.0f);
    return Length(p - (a + ab * t));
}

/// Screen endpoints of a transition (two-way pairs are drawn side by side).
void TransitionEnds(const AnimGraph& g, const AnimGraph::Transition& t, const Canvas& c, Vec2& a, Vec2& b)
{
    const Vec2 half = Vec2(NodeW, NodeH) * 0.5f * c.scale;
    const Vec2 ca = c.ToScreen(NodePos(g, t.from) + Vec2(NodeW, NodeH) * 0.5f);
    const Vec2 cb = c.ToScreen(NodePos(g, t.to) + Vec2(NodeW, NodeH) * 0.5f);
    const Vec2 d = SafeNormalize(cb - ca, Vec2(1, 0));
    const Vec2 side = Vec2(-d.y, d.x) * (7.0f * c.scale);
    a = BoxExit(ca, d, half) + side;
    b = BoxExit(cb, -d, half) + side;
}

String Snapshot(const AnimGraph& g) { return WriteAnimGraph(g); }

void PushUndo(AnimGraphEditorState& st)
{
    st.undo.PushBack(Snapshot(st.graph));
    if (st.undo.Size() > MaxUndo)
        st.undo.RemoveAt(0);
    st.redo.Clear();
}

void Changed(AnimGraphEditorState& st) { st.dirty = true; }

void SaveGraph(Editor& ed, AnimGraphEditorState& st)
{
    if (!fs::WriteTextFile(st.path, WriteAnimGraph(st.graph).View())) {
        st.error = "Could not write the file";
        return;
    }
    st.dirty = false;
    st.error.Clear();
    if (ed.animation)
        ed.animation->ReloadGraph(Path::MakeRelative(st.path, ed.contentRoot).View());
}

void DeleteState(AnimGraphEditorState& st, i32 index)
{
    AnimGraph& g = st.graph;
    if (index < 0 || usize(index) >= g.states.Size() || g.states.Size() <= 1)
        return;
    g.transitions.RemoveIf([&](const AnimGraph::Transition& t) { return t.from == index || t.to == index; });
    for (AnimGraph::Transition& t : g.transitions) {
        if (t.from > index)
            --t.from;
        if (t.to > index)
            --t.to;
    }
    g.states.RemoveAt(usize(index));
    if (g.entry == index)
        g.entry = 0;
    else if (g.entry > index)
        --g.entry;
    st.selectedState = -1;
    st.selectedTransition = -1;
}

/// The state of the Animator (on the selected entity or below it) that plays this graph, while playing.
i32 LiveState(Editor& ed, const AnimGraphEditorState& st)
{
    if (!ed.IsPlaying() || !ed.animation || !ed.world.IsAlive(ed.selected))
        return -1;
    const String rel = Path::MakeRelative(st.path, ed.contentRoot);
    Array<ecs::Entity> queue;
    queue.PushBack(ed.selected);
    for (usize q = 0; q < queue.Size() && q < 256; ++q) {
        if (const Animator* a = ed.world.Get<const Animator>(queue[q]); a && a->graph.View().EqualsIgnoreCase(rel))
            return st.graph.FindState(ed.animation->CurrentState(queue[q]));
        queue.Append(scene::GetChildren(ed.world, queue[q]));
    }
    return -1;
}

void DrawNode(DrawList& dl, const Canvas& c, StringView title, StringView subtitle, Vec2 pos, Color accent, bool selected, bool live, bool entry)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const ui::Theme& th = ctx.GetStyle().colors;
    const f32 k = c.scale;
    const Rect r(c.ToScreen(pos), c.ToScreen(pos + Vec2(NodeW, NodeH)));
    if (!r.Intersects(c.rect))
        return;
    dl.AddShadow(r, Color(0, 0, 0, 120), 6.0f * k, 10.0f * k, Vec2(0.0f, 3.0f * k));
    if (live)
        dl.AddRect(r.Expanded(4.0f * k), WithAlpha(th.success, 0.35f), ui::CornerRadii(10.0f * k));
    dl.AddRect(r, Color(0x2A, 0x2A, 0x2E, 245), ui::CornerRadii(7.0f * k), selected ? 2.0f * k : 1.0f, selected ? th.accentHovered : Color(0x10, 0x10, 0x12));
    dl.AddRect(Rect(r.min, Vec2(r.min.x + 5.0f * k, r.max.y)), accent, ui::CornerRadii(7.0f * k, 0.0f, 0.0f, 7.0f * k));
    if (k < 0.45f)
        return;
    const ui::FontHandle semi = ctx.Fonts().FindFont("Inter SemiBold");
    const ui::FontHandle font = ctx.Fonts().FindFont("Inter");
    dl.AddTextInRect(semi, 13.0f * k, Rect(r.min + Vec2(14.0f * k, 6.0f * k), Vec2(r.max.x - 8.0f * k, r.min.y + 26.0f * k)), Color(255, 255, 255),
                     entry ? String::Format(ZX_ICON_LOG_IN "  {}", title).View() : title);
    dl.AddTextInRect(font, 11.0f * k, Rect(r.min + Vec2(14.0f * k, 26.0f * k), r.max - Vec2(8.0f * k, 4.0f * k)), th.textDisabled, subtitle);
}

void ParamsPanel(AnimGraphEditorState& st)
{
    AnimGraph& g = st.graph;
    ui::SeparatorText("Parameters");
    static const StringView Types[] = {"Float", "Bool", "Trigger"};
    i32 remove = -1;
    for (usize i = 0; i < g.params.Size(); ++i) {
        AnimGraph::Param& p = g.params[i];
        ui::PushID(i32(i));
        ui::SetNextItemWidth(110.0f);
        String name = p.name;
        if (ui::InputText("##name", name) && !name.Empty()) {
            PushUndo(st);
            // Conditions and blend spaces follow the rename.
            for (AnimGraph::Transition& t : g.transitions)
                for (AnimGraph::Condition& c : t.conditions)
                    if (c.param.View().EqualsIgnoreCase(p.name))
                        c.param = name;
            for (AnimGraph::State& s : g.states)
                if (s.blendParam.View().EqualsIgnoreCase(p.name))
                    s.blendParam = name;
            p.name = name;
            Changed(st);
        }
        ui::SameLine();
        i32 type = i32(p.type);
        ui::SetNextItemWidth(78.0f);
        if (ui::Combo("##type", &type, Span<const StringView>(Types, 3))) {
            PushUndo(st);
            p.type = AnimGraph::ParamType(type);
            Changed(st);
        }
        ui::SameLine();
        ui::SetNextItemWidth(56.0f);
        if (p.type == AnimGraph::ParamType::Float) {
            if (ui::DragFloat("##def", &p.defaultValue, 0.05f, -1000.0f, 1000.0f, "{:.2f}"))
                Changed(st);
        } else if (p.type == AnimGraph::ParamType::Bool) {
            bool b = p.defaultValue > 0.5f;
            if (ui::Checkbox("##def", &b)) {
                PushUndo(st);
                p.defaultValue = b ? 1.0f : 0.0f;
                Changed(st);
            }
        }
        ui::SameLine();
        if (ui::IconButton(ZX_ICON_TRASH_2, "Remove the parameter"))
            remove = i32(i);
        ui::PopID();
    }
    if (remove >= 0) {
        PushUndo(st);
        const String name = g.params[usize(remove)].name;
        g.params.RemoveAt(usize(remove));
        for (AnimGraph::Transition& t : g.transitions)
            t.conditions.RemoveIf([&](const AnimGraph::Condition& c) { return c.param.View().EqualsIgnoreCase(name); });
        Changed(st);
    }
    if (ui::Button(ZX_ICON_PLUS "  Parameter")) {
        PushUndo(st);
        String name = "Param";
        for (u32 n = 2; g.FindParam(name) >= 0; ++n)
            name = String::Format("Param{}", n);
        g.params.PushBack(AnimGraph::Param{name, AnimGraph::ParamType::Float, 0.0f});
        Changed(st);
    }
}

void StatePanel(Editor& ed, AnimGraphEditorState& st, AnimGraph::State& s)
{
    AnimGraph& g = st.graph;
    ui::SeparatorText("State");
    String name = s.name;
    if (ui::InputText("Name", name) && !name.Empty() && (g.FindState(name) < 0 || name.View().EqualsIgnoreCase(s.name))) {
        PushUndo(st);
        s.name = name;
        Changed(st);
    }
    const Array<String> clips = AnimationClipNames(ed);
    i32 mode = s.blend.Empty() ? 0 : 1;
    static const StringView Modes[] = {"Single Clip", "Blend Space 1D"};
    if (ui::Combo("Motion", &mode, Span<const StringView>(Modes, 2))) {
        PushUndo(st);
        if (mode == 1 && s.blend.Empty()) {
            s.blend.PushBack(AnimGraph::BlendSample{s.clip.Empty() ? String("Idle") : s.clip, 0.0f});
            s.blend.PushBack(AnimGraph::BlendSample{String("Walk"), 2.0f});
            if (s.blendParam.Empty())
                for (const AnimGraph::Param& p : g.params)
                    if (p.type == AnimGraph::ParamType::Float && s.blendParam.Empty())
                        s.blendParam = p.name;
        } else if (mode == 0 && !s.blend.Empty()) {
            s.clip = s.blend[0].clip;
            s.blend.Clear();
        }
        Changed(st);
    }
    if (s.blend.Empty()) {
        if (NameCombo("Clip", s.clip, clips.View())) {
            PushUndo(st);
            Changed(st);
        }
    } else {
        Array<String> floats;
        for (const AnimGraph::Param& p : g.params)
            if (p.type == AnimGraph::ParamType::Float)
                floats.PushBack(p.name);
        if (NameCombo("Parameter", s.blendParam, floats.View())) {
            PushUndo(st);
            Changed(st);
        }
        i32 remove = -1;
        for (usize i = 0; i < s.blend.Size(); ++i) {
            ui::PushID(i32(i) + 1000);
            ui::SetNextItemWidth(120.0f);
            if (NameCombo("##clip", s.blend[i].clip, clips.View())) {
                PushUndo(st);
                Changed(st);
            }
            ui::SameLine();
            ui::SetNextItemWidth(70.0f);
            if (ui::DragFloat("##value", &s.blend[i].value, 0.05f, -1000.0f, 1000.0f, "at {:.2f}")) {
                // Keep the samples sorted by value.
                Sort(s.blend, [](const AnimGraph::BlendSample& a, const AnimGraph::BlendSample& b) { return a.value < b.value; });
                Changed(st);
            }
            ui::SameLine();
            if (s.blend.Size() > 1 && ui::IconButton(ZX_ICON_TRASH_2, "Remove the sample"))
                remove = i32(i);
            ui::PopID();
        }
        if (remove >= 0) {
            PushUndo(st);
            s.blend.RemoveAt(usize(remove));
            Changed(st);
        }
        if (ui::Button(ZX_ICON_PLUS "  Sample")) {
            PushUndo(st);
            s.blend.PushBack(AnimGraph::BlendSample{clips.Empty() ? String("Run") : clips[0], s.blend.Back().value + 2.0f});
            Changed(st);
        }
    }
    if (ui::DragFloat("Speed", &s.speed, 0.01f, 0.0f, 10.0f, "{:.2f}x"))
        Changed(st);
    if (ui::Checkbox("Loop", &s.loop)) {
        PushUndo(st);
        Changed(st);
    }
    ui::SeparatorText("Events");
    i32 remove = -1;
    for (usize i = 0; i < s.events.Size(); ++i) {
        ui::PushID(i32(i) + 2000);
        ui::SetNextItemWidth(120.0f);
        if (ui::InputText("##event", s.events[i].name))
            Changed(st);
        ui::SameLine();
        ui::SetNextItemWidth(70.0f);
        if (ui::DragFloat("##time", &s.events[i].time, 0.005f, 0.0f, 1.0f, "at {:.2f}"))
            Changed(st);
        ui::SameLine();
        if (ui::IconButton(ZX_ICON_TRASH_2, "Remove the event"))
            remove = i32(i);
        ui::PopID();
    }
    if (remove >= 0) {
        PushUndo(st);
        s.events.RemoveAt(usize(remove));
        Changed(st);
    }
    if (ui::Button(ZX_ICON_PLUS "  Event")) {
        PushUndo(st);
        s.events.PushBack(AnimGraph::Event{String("Event"), 0.5f});
        Changed(st);
    }
    ui::TextDisabled("Events call onAnimEvent(name) in scripts when the\nstate's normalized time passes them");
}

void TransitionPanel(AnimGraphEditorState& st, AnimGraph::Transition& t)
{
    AnimGraph& g = st.graph;
    ui::SeparatorText("Transition");
    ui::Text(String::Format("{}  " ZX_ICON_ARROW_RIGHT "  {}", t.from < 0 ? StringView("Any State") : g.states[usize(t.from)].name.View(),
                            g.states[usize(t.to)].name));
    if (ui::DragFloat("Duration", &t.duration, 0.005f, 0.0f, 5.0f, "{:.2f} s"))
        Changed(st);
    bool exit = t.exitTime >= 0.0f;
    if (ui::Checkbox("Exit Time", &exit)) {
        PushUndo(st);
        t.exitTime = exit ? 0.9f : -1.0f;
        Changed(st);
    }
    if (exit) {
        ui::SameLine();
        ui::SetNextItemWidth(90.0f);
        if (ui::DragFloat("##exit", &t.exitTime, 0.005f, 0.0f, 1.0f, "{:.2f}"))
            Changed(st);
    }
    ui::SeparatorText("Conditions (all must hold)");
    static const StringView Ops[] = {">", "<", "is true", "is false", "triggered"};
    Array<StringView> paramNames;
    for (const AnimGraph::Param& p : g.params)
        paramNames.PushBack(p.name.View());
    i32 remove = -1;
    for (usize i = 0; i < t.conditions.Size(); ++i) {
        AnimGraph::Condition& c = t.conditions[i];
        ui::PushID(i32(i) + 3000);
        i32 param = g.FindParam(c.param);
        ui::SetNextItemWidth(100.0f);
        if (!paramNames.Empty() && ui::Combo("##param", &param, paramNames.View()) && param >= 0) {
            PushUndo(st);
            c.param = g.params[usize(param)].name;
            Changed(st);
        }
        ui::SameLine();
        i32 op = i32(c.op);
        ui::SetNextItemWidth(80.0f);
        if (ui::Combo("##op", &op, Span<const StringView>(Ops, 5))) {
            PushUndo(st);
            c.op = AnimGraph::Op(op);
            Changed(st);
        }
        if (c.op == AnimGraph::Op::Greater || c.op == AnimGraph::Op::Less) {
            ui::SameLine();
            ui::SetNextItemWidth(60.0f);
            if (ui::DragFloat("##value", &c.value, 0.05f, -1000.0f, 1000.0f, "{:.2f}"))
                Changed(st);
        }
        ui::SameLine();
        if (ui::IconButton(ZX_ICON_TRASH_2, "Remove the condition"))
            remove = i32(i);
        ui::PopID();
    }
    if (remove >= 0) {
        PushUndo(st);
        t.conditions.RemoveAt(usize(remove));
        Changed(st);
    }
    if (!g.params.Empty() && ui::Button(ZX_ICON_PLUS "  Condition")) {
        PushUndo(st);
        const AnimGraph::Param& p = g.params[0];
        t.conditions.PushBack(AnimGraph::Condition{p.name, p.type == AnimGraph::ParamType::Bool ? AnimGraph::Op::IsTrue
                                                           : p.type == AnimGraph::ParamType::Trigger ? AnimGraph::Op::Triggered
                                                                                                     : AnimGraph::Op::Greater,
                                                   0.0f});
        Changed(st);
    }
    if (t.conditions.Empty() && t.exitTime < 0.0f)
        ui::TextColored(ui::GetCurrentContext()->GetStyle().colors.warning, ZX_ICON_TRIANGLE_ALERT "  No condition and no exit time: fires at once");
}

} // namespace

void OpenAnimGraphEditor(Editor& ed, StringView graphPath)
{
    AnimGraphEditorState& st = ed.animGraphEditor;
    String text, error;
    AnimGraph g;
    if (!fs::ReadTextFile(graphPath, text) || !ParseAnimGraph(text.View(), g, &error)) {
        ZX_LOG_ERROR(LogAnimationEditor, "Can't open {}: {}", Path::FileName(graphPath), error.Empty() ? StringView("unreadable") : error.View());
        return;
    }
    // Graphs without a layout: a row.
    bool laidOut = false;
    for (const AnimGraph::State& s : g.states)
        laidOut |= s.x != 0.0f || s.y != 0.0f;
    if (!laidOut)
        for (usize i = 0; i < g.states.Size(); ++i) {
            g.states[i].x = f32(i % 4) * 260.0f;
            g.states[i].y = f32(i / 4) * 140.0f;
        }
    st = AnimGraphEditorState{};
    st.path = String(graphPath);
    st.graph = move(g);
    st.open = true;
    ui::FocusDockedWindow(AnimGraphEditorTitle);
}

void CreateAnimGraph(Editor& ed, StringView directory)
{
    String path = Path::Join(directory, "NewAnimGraph.zanimgraph");
    for (u32 n = 2; platform::FileExists(path); ++n)
        path = Path::Join(directory, String::Format("NewAnimGraph{}.zanimgraph", n));
    if (fs::WriteTextFile(path, WriteAnimGraph(MakeLocomotionGraph()).View()))
        OpenAnimGraphEditor(ed, path);
}

void DrawAnimGraphEditor(Editor& ed)
{
    AnimGraphEditorState& st = ed.animGraphEditor;
    if (!st.open)
        return;
    PlaceWindowFirstTime(ed, Vec2(1100.0f, 640.0f));
    bool open = true;
    if (!ui::Begin(AnimGraphEditorTitle, &open, ui::WindowFlags::NoScrollbar)) {
        ui::End();
        st.open = open;
        return;
    }
    st.open = open;
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& th = ctx.GetStyle().colors;
    AnimGraph& g = st.graph;

    // --- Toolbar ---
    if (ui::Button(ZX_ICON_SAVE "  Save", Vec2::Zero(), st.dirty ? ui::ButtonVariant::Primary : ui::ButtonVariant::Default))
        SaveGraph(ed, st);
    ui::SameLine();
    const bool canUndo = !st.undo.Empty(), canRedo = !st.redo.Empty();
    auto undo = [&] {
        st.redo.PushBack(Snapshot(g));
        String error;
        ParseAnimGraph(st.undo.Back().View(), g, &error);
        st.undo.PopBack();
        st.selectedState = st.selectedTransition = -1;
        Changed(st);
    };
    auto redo = [&] {
        st.undo.PushBack(Snapshot(g));
        String error;
        ParseAnimGraph(st.redo.Back().View(), g, &error);
        st.redo.PopBack();
        st.selectedState = st.selectedTransition = -1;
        Changed(st);
    };
    if (ui::IconButton(ZX_ICON_UNDO_2, "Undo (Ctrl+Z)") && canUndo)
        undo();
    ui::SameLine();
    if (ui::IconButton(ZX_ICON_REDO_2, "Redo (Ctrl+Y)") && canRedo)
        redo();
    ui::SameLine();
    ui::AlignTextToFramePadding();
    ui::Text(String::Format("{}{}", Path::FileName(st.path), st.dirty ? "*" : ""));
    ui::SameLine(0.0f, 16.0f);
    if (!st.error.Empty())
        ui::TextColored(th.error, st.error);
    else if (st.linkFrom != -3)
        ui::TextColored(th.warning, ZX_ICON_GIT_BRANCH "  Click the target state (Esc cancels)");
    else
        ui::TextDisabled("Right-click: add states / transitions. Select an entity with this graph and press Play to see it run.");

    const Rect content = ui::GetWindowContentRect();
    const f32 sideW = Round(330.0f * s);
    const f32 top = ui::GetCursorPos().y + Round(4.0f * s);
    const Rect canvasRect(Vec2(content.min.x, top), Vec2(content.max.x - sideW - Round(6.0f * s), content.max.y));
    Canvas c{canvasRect, st.pan, st.zoom * s};
    DrawList& dl = ui::GetWindowDrawList();
    ui::SetCursorPos(canvasRect.min);
    ui::InvisibleButton("##graph_canvas", canvasRect.Size() / s);
    const bool hovered = ui::IsItemHovered() || ui::IsItemActive();
    const Vec2 mouse = ui::GetMousePos();
    dl.PushClipRect(canvasRect);
    dl.AddRect(canvasRect, Color(0x1C, 0x1C, 0x1E));
    {
        const f32 step = 32.0f * c.scale;
        if (step > 8.0f) {
            const Vec2 o = c.ToScreen(Vec2::Zero());
            for (f32 x = Fmod(o.x - canvasRect.min.x, step) + canvasRect.min.x; x < canvasRect.max.x; x += step)
                dl.AddLine(Vec2(x, canvasRect.min.y), Vec2(x, canvasRect.max.y), Color(0x26, 0x26, 0x29), 1.0f);
            for (f32 y = Fmod(o.y - canvasRect.min.y, step) + canvasRect.min.y; y < canvasRect.max.y; y += step)
                dl.AddLine(Vec2(canvasRect.min.x, y), Vec2(canvasRect.max.x, y), Color(0x26, 0x26, 0x29), 1.0f);
        }
    }

    // Hit testing: nodes (Any State = -1), then transitions.
    i32 hotNode = -3, hotTransition = -1;
    for (i32 i = i32(g.states.Size()) - 1; i >= -1 && hovered; --i) {
        const Vec2 p = NodePos(g, i);
        if (Rect(c.ToScreen(p), c.ToScreen(p + Vec2(NodeW, NodeH))).Contains(mouse)) {
            hotNode = i;
            break;
        }
    }
    if (hotNode == -3 && hovered)
        for (usize i = 0; i < g.transitions.Size(); ++i) {
            Vec2 a, b;
            TransitionEnds(g, g.transitions[i], c, a, b);
            if (SegmentDistance(mouse, a, b) < 6.0f * s) {
                hotTransition = i32(i);
                break;
            }
        }

    // --- Mouse ---
    static Vec2 rightPress;
    if (hovered && ed.input.WasMousePressed(MouseButton::Left)) {
        if (st.linkFrom != -3) {
            if (hotNode >= 0 && hotNode != st.linkFrom) {
                PushUndo(st);
                AnimGraph::Transition t;
                t.from = st.linkFrom;
                t.to = hotNode;
                g.transitions.PushBack(t);
                st.selectedTransition = i32(g.transitions.Size() - 1);
                st.selectedState = -1;
                Changed(st);
            }
            st.linkFrom = -3;
        } else if (hotNode != -3) {
            st.selectedState = hotNode < 0 ? -2 : hotNode;
            st.selectedTransition = -1;
            st.drag = AnimGraphEditorState::Drag::Node;
            st.dragOffset = c.ToGraph(mouse) - NodePos(g, hotNode);
            st.beforeEdit = Snapshot(g);
        } else if (hotTransition >= 0) {
            st.selectedTransition = hotTransition;
            st.selectedState = -1;
        } else {
            st.selectedState = st.selectedTransition = -1;
        }
    }
    if (hovered && (ed.input.WasMousePressed(MouseButton::Right) || ed.input.WasMousePressed(MouseButton::Middle))) {
        rightPress = mouse;
        st.drag = AnimGraphEditorState::Drag::Pan;
    }
    if (st.drag == AnimGraphEditorState::Drag::Node && ed.input.IsMouseDown(MouseButton::Left) && st.selectedState != -1) {
        const Vec2 p = c.ToGraph(mouse) - st.dragOffset;
        const Vec2 snapped(Round(p.x / 10.0f) * 10.0f, Round(p.y / 10.0f) * 10.0f);
        if (st.selectedState == -2) {
            g.anyX = snapped.x;
            g.anyY = snapped.y;
        } else {
            g.states[usize(st.selectedState)].x = snapped.x;
            g.states[usize(st.selectedState)].y = snapped.y;
        }
    }
    if (st.drag == AnimGraphEditorState::Drag::Pan && (ed.input.IsMouseDown(MouseButton::Right) || ed.input.IsMouseDown(MouseButton::Middle)))
        st.pan += ed.input.MouseDelta() / c.scale;
    if (st.drag != AnimGraphEditorState::Drag::None && !ed.input.IsMouseDown(MouseButton::Left) && !ed.input.IsMouseDown(MouseButton::Right) &&
        !ed.input.IsMouseDown(MouseButton::Middle)) {
        if (st.drag == AnimGraphEditorState::Drag::Node && st.beforeEdit != Snapshot(g)) {
            st.undo.PushBack(st.beforeEdit);
            st.redo.Clear();
            Changed(st);
        }
        if (st.drag == AnimGraphEditorState::Drag::Pan && Length(mouse - rightPress) < 4.0f * s) {
            // A click (no drag): context menu.
            if (hotNode != -3) {
                st.selectedState = hotNode < 0 ? -2 : hotNode;
                st.selectedTransition = -1;
                ui::OpenPopup("##state_menu");
            } else {
                st.dragOffset = c.ToGraph(mouse); // where a new state goes
                ui::OpenPopup("##canvas_menu");
            }
        }
        st.drag = AnimGraphEditorState::Drag::None;
    }
    if (hovered && ed.input.MouseWheel() != 0.0f) {
        const Vec2 before = c.ToGraph(mouse);
        st.zoom = Clamp(st.zoom * Pow(1.15f, ed.input.MouseWheel()), 0.3f, 2.0f);
        c.scale = st.zoom * s;
        st.pan += c.ToGraph(mouse) - before;
        c.pan = st.pan;
    }
    c.pan = st.pan;

    // --- Drawing: transitions under the nodes ---
    for (usize i = 0; i < g.transitions.Size(); ++i) {
        Vec2 a, b;
        TransitionEnds(g, g.transitions[i], c, a, b);
        const bool sel = i32(i) == st.selectedTransition, hot = i32(i) == hotTransition;
        const Color col = sel ? th.accentHovered : hot ? Color(0xC8, 0xC8, 0xCC) : Color(0x8A, 0x8A, 0x90);
        dl.AddLine(a, b, col, (sel ? 2.5f : 1.6f) * s);
        const Vec2 d = SafeNormalize(b - a, Vec2(1, 0)), n(-d.y, d.x);
        const Vec2 mid = (a + b) * 0.5f;
        const f32 h = 7.0f * c.scale;
        dl.AddTriangle(mid + d * h, mid - d * h * 0.6f + n * h * 0.7f, mid - d * h * 0.6f - n * h * 0.7f, col);
    }
    if (st.linkFrom != -3) {
        const Vec2 from = c.ToScreen(NodePos(g, st.linkFrom) + Vec2(NodeW, NodeH) * 0.5f);
        dl.AddLine(from, mouse, th.warning, 2.0f * s);
    }
    const i32 live = LiveState(ed, st);
    DrawNode(dl, c, "Any State", "transitions from every state", Vec2(g.anyX, g.anyY), Color(0x2F, 0x6A, 0xB0), st.selectedState == -2, false, false);
    for (usize i = 0; i < g.states.Size(); ++i) {
        const AnimGraph::State& sd = g.states[i];
        const String sub = sd.blend.Empty() ? String::Format(ZX_ICON_CLAPPERBOARD "  {}{}", sd.clip, sd.loop ? "" : " (once)")
                                            : String::Format("Blend by {} ({} clips)", sd.blendParam, sd.blend.Size());
        DrawNode(dl, c, sd.name.View(), sub.View(), Vec2(sd.x, sd.y), i32(i) == g.entry ? Color(0x3C, 0x86, 0x4C) : Color(0xAE, 0x7A, 0x2A),
                 st.selectedState == i32(i), live == i32(i), i32(i) == g.entry);
    }
    dl.PopClipRect();

    // --- Menus ---
    if (ui::BeginPopup("##canvas_menu")) {
        if (ui::MenuItem(ZX_ICON_PLUS "  Add State")) {
            PushUndo(st);
            AnimGraph::State ns;
            ns.name = "State";
            for (u32 n = 2; g.FindState(ns.name) >= 0; ++n)
                ns.name = String::Format("State{}", n);
            ns.clip = "Idle";
            ns.x = Round(st.dragOffset.x - NodeW * 0.5f);
            ns.y = Round(st.dragOffset.y - NodeH * 0.5f);
            g.states.PushBack(ns);
            st.selectedState = i32(g.states.Size() - 1);
            st.selectedTransition = -1;
            Changed(st);
        }
        ui::EndPopup();
    }
    if (ui::BeginPopup("##state_menu")) {
        const i32 state = st.selectedState == -2 ? -1 : st.selectedState;
        if (ui::MenuItem(ZX_ICON_GIT_BRANCH "  Make Transition"))
            st.linkFrom = state;
        if (state >= 0 && ui::MenuItem(ZX_ICON_LOG_IN "  Set as Entry State", {}, false, state != g.entry)) {
            PushUndo(st);
            g.entry = state;
            Changed(st);
        }
        if (state >= 0 && ui::MenuItem(ZX_ICON_TRASH_2 "  Delete", "Del", false, g.states.Size() > 1)) {
            PushUndo(st);
            DeleteState(st, state);
            Changed(st);
        }
        ui::EndPopup();
    }

    // --- Keys ---
    if (ui::IsWindowHovered()) {
        if (ui::IsShortcutPressed(KeyModifiers::Control, Key::S))
            SaveGraph(ed, st);
        if (ui::IsShortcutPressed(KeyModifiers::Control, Key::Z) && canUndo)
            undo();
        if (ui::IsShortcutPressed(KeyModifiers::Control, Key::Y) && canRedo)
            redo();
        if (ui::IsKeyPressed(Key::Escape))
            st.linkFrom = -3;
        if (ui::IsKeyPressed(Key::Delete)) {
            if (st.selectedState >= 0 && g.states.Size() > 1) {
                PushUndo(st);
                DeleteState(st, st.selectedState);
                Changed(st);
            } else if (st.selectedTransition >= 0 && usize(st.selectedTransition) < g.transitions.Size()) {
                PushUndo(st);
                g.transitions.RemoveAt(usize(st.selectedTransition));
                st.selectedTransition = -1;
                Changed(st);
            }
        }
    }

    // --- Side panel ---
    const Rect sideRect(Vec2(canvasRect.max.x + Round(6.0f * s), top), content.max);
    ui::SetCursorPos(sideRect.min);
    if (ui::BeginChild("##graph_side", sideRect.Size() / s, false)) {
        ParamsPanel(st);
        ui::Spacing();
        if (st.selectedState >= 0 && usize(st.selectedState) < g.states.Size())
            StatePanel(ed, st, g.states[usize(st.selectedState)]);
        else if (st.selectedTransition >= 0 && usize(st.selectedTransition) < g.transitions.Size())
            TransitionPanel(st, g.transitions[usize(st.selectedTransition)]);
        else
            ui::TextDisabled("Select a state or a transition to edit it.");
    }
    ui::EndChild();
    ui::End();
}

} // namespace zx::editor
