// Viewport panel: scene image, camera navigation, picking, transform gizmos (drawn in 2D), asset drag & drop.

#include "Editor.h"
#include "Engine/Raycast.h"
#include "Core/Math/Geometry.h"
#include "Core/String/Path.h"
#include "Engine/SceneRendering.h"

namespace zx::editor {

namespace {

const Color AxisColors[3] = {Color(0xE5, 0x48, 0x48), Color(0x6C, 0xC8, 0x40), Color(0x45, 0x7D, 0xF0)};
const Color HotColor(0xFF, 0xD2, 0x3C);

struct GizmoState
{
    i32 hot = -1;       // axis under the mouse: 0..2, 3 = uniform / screen
    i32 active = -1;    // axis being dragged
    Transform startLocal;
    Vec3 startWorld;
    f32 startParam = 0.0f;
    f32 startAngle = 0.0f;
    Vec2 startMouse;
};
GizmoState g_gizmo;

struct ViewProjector
{
    Mat4 viewProj;
    Mat4 invViewProj;
    Rect rect;
    Vec3 cameraPos;

    bool Project(Vec3 p, Vec2& out) const
    {
        const Vec4 c = viewProj * Vec4(p, 1.0f);
        if (c.w <= 1e-4f)
            return false;
        const Vec2 ndc(c.x / c.w, c.y / c.w);
        out = rect.min + Vec2((ndc.x * 0.5f + 0.5f) * rect.Width(), (0.5f - ndc.y * 0.5f) * rect.Height());
        return true;
    }
    Ray MouseRay(Vec2 mouse) const
    {
        const Vec2 ndc(((mouse.x - rect.min.x) / rect.Width()) * 2.0f - 1.0f, 1.0f - ((mouse.y - rect.min.y) / rect.Height()) * 2.0f);
        const Vec4 n = invViewProj * Vec4(ndc.x, ndc.y, 1.0f, 1.0f); // reversed-Z: near plane at depth 1
        const Vec3 nearPoint = n.XYZ() / n.w;
        return Ray(cameraPos, Normalize(nearPoint - cameraPos));
    }
};

f32 DistanceToSegment(Vec2 p, Vec2 a, Vec2 b)
{
    const Vec2 ab = b - a;
    const f32 t = Saturate(Dot(p - a, ab) / Max(Dot(ab, ab), 1e-6f));
    return Length(p - (a + ab * t));
}

/// Parameter along the line (origin + axis * t) closest to the ray.
bool ClosestParamOnAxis(const Ray& ray, Vec3 origin, Vec3 axis, f32& t)
{
    const Vec3 w0 = origin - ray.origin;
    const f32 b = Dot(axis, ray.direction);
    const f32 denom = 1.0f - b * b;
    if (denom < 1e-5f)
        return false;
    const f32 d = Dot(axis, w0), e = Dot(ray.direction, w0);
    t = (b * e - d) / denom;
    return true;
}

Mat4 ParentWorld(Editor& ed, ecs::Entity e)
{
    const ecs::Entity p = scene::GetParent(ed.world, e);
    return p ? scene::ComputeWorldMatrix(ed.world, p) : Mat4::Identity();
}

/// Ray vs everything pickable: mesh section bounds (in mesh space) and light icons.
ecs::Entity Pick(Editor& ed, const Ray& ray)
{
    ecs::Entity best;
    f32 bestT = F32Max;
    RaycastHit hit;
    if (RaycastMeshes(ed.world, *ed.renderAssets, ray, hit)) {
        best = hit.entity;
        bestT = hit.distance;
    }
    auto pickPoint = [&](ecs::Entity e, const scene::WorldTransform& wt) {
        f32 t;
        if (RayIntersectsSphere(ray, Sphere(wt.GetPosition(), 0.35f), t) && t < bestT) {
            bestT = t;
            best = e;
        }
    };
    ed.world.Query<const PointLight, const scene::WorldTransform>().Each(
        [&](ecs::Entity e, const PointLight&, const scene::WorldTransform& wt) { pickPoint(e, wt); });
    ed.world.Query<const SpotLight, const scene::WorldTransform>().Each(
        [&](ecs::Entity e, const SpotLight&, const scene::WorldTransform& wt) { pickPoint(e, wt); });
    // Prefer the topmost named node of an imported prefab (UE-style: clicking selects the actor, not a sub-mesh).
    return best;
}

void DrawLightIcons(Editor& ed, const ViewProjector& vp, DrawList& dl, f32 s)
{
    auto icon = [&](ecs::Entity e, const scene::WorldTransform& wt, Vec3 color, StringView glyph) {
        Vec2 p;
        if (!vp.Project(wt.GetPosition(), p) || !vp.rect.Contains(p))
            return;
        const bool sel = e == ed.selected;
        const f32 r = 11.0f * s;
        dl.AddCircle(p, r, Color(20, 20, 20, 170), 1.5f * s, sel ? HotColor : Color(255, 255, 255, 120));
        dl.AddCircle(p + Vec2(r * 0.55f, -r * 0.55f), 3.5f * s, Color::FromLinear(LinearColor(color.x, color.y, color.z, 1.0f)));
        ui::Context& ctx = *ui::GetCurrentContext();
        dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 12.0f * s, Rect(p - Vec2(r), p + Vec2(r)), Color(240, 240, 240), glyph,
                         ui::TextAlign::Center, false);
    };
    ed.world.Query<const PointLight, const scene::WorldTransform>().Each(
        [&](ecs::Entity e, const PointLight& l, const scene::WorldTransform& wt) { icon(e, wt, l.color, ZX_ICON_LIGHTBULB); });
    ed.world.Query<const SpotLight, const scene::WorldTransform>().Each(
        [&](ecs::Entity e, const SpotLight& l, const scene::WorldTransform& wt) { icon(e, wt, l.color, ZX_ICON_SPOTLIGHT); });
    ed.world.Query<const AudioSource, const scene::WorldTransform>().Each([&](ecs::Entity e, const AudioSource&, const scene::WorldTransform& wt) {
        if (!ed.world.Has<MeshRenderer>(e))
            icon(e, wt, Vec3(0.3f, 0.8f, 1.0f), ZX_ICON_VOLUME_2);
    });
    ed.world.Query<const ReverbZone, const scene::WorldTransform>().Each(
        [&](ecs::Entity e, const ReverbZone&, const scene::WorldTransform& wt) { icon(e, wt, Vec3(0.7f, 0.5f, 1.0f), ZX_ICON_AUDIO_LINES); });
    ed.world.Query<const Decal, const scene::WorldTransform>().Each(
        [&](ecs::Entity e, const Decal&, const scene::WorldTransform& wt) { icon(e, wt, Vec3(1.0f, 0.5f, 0.8f), ZX_ICON_STAMP); });
    ed.world.Query<const ReflectionProbe, const scene::WorldTransform>().Each(
        [&](ecs::Entity e, const ReflectionProbe&, const scene::WorldTransform& wt) { icon(e, wt, Vec3(0.45f, 0.85f, 1.0f), ZX_ICON_CIRCLE_DOT); });
    ed.world.Query<const PostProcessVolume, const scene::WorldTransform>().Each(
        [&](ecs::Entity e, const PostProcessVolume&, const scene::WorldTransform& wt) { icon(e, wt, Vec3(1.0f, 0.67f, 0.25f), ZX_ICON_APERTURE); });
}

/// Draws and drives the transform gizmo. Returns true if the gizmo owns the mouse this frame.
bool Gizmo(Editor& ed, const ViewProjector& vp, DrawList& dl, bool hoveredViewport, f32 s)
{
    if (!ed.world.IsAlive(ed.selected) || ed.tool == TransformTool::Select || !ed.world.Has<Transform>(ed.selected)) {
        g_gizmo.hot = g_gizmo.active = -1;
        return false;
    }
    const Mat4 world = scene::ComputeWorldMatrix(ed.world, ed.selected);
    const Vec3 origin = world.GetTranslation();
    Vec2 o2;
    if (!vp.Project(origin, o2)) {
        g_gizmo.hot = g_gizmo.active = -1;
        return false;
    }
    const f32 length = Distance(vp.cameraPos, origin) * 0.16f;
    Vec3 axes[3] = {Vec3::UnitX(), Vec3::UnitY(), Vec3::UnitZ()};
    if (ed.localSpace || ed.tool == TransformTool::Scale)
        for (u32 i = 0; i < 3; ++i)
            axes[i] = SafeNormalize(world.TransformVector(axes[i]), axes[i]);
    const Vec2 mouse = ui::GetMousePos();
    const Ray ray = vp.MouseRay(mouse);
    const f32 pickDist = 8.0f * s;

    // --- Hover ---
    if (g_gizmo.active < 0) {
        g_gizmo.hot = -1;
        if (hoveredViewport) {
            f32 best = pickDist;
            if (ed.tool == TransformTool::Rotate) {
                for (i32 a = 0; a < 3; ++a) {
                    const Vec3 u = axes[(a + 1) % 3], v = axes[(a + 2) % 3];
                    Vec2 prev;
                    bool hasPrev = false;
                    for (u32 k = 0; k <= 48; ++k) {
                        const f32 th = TwoPi * f32(k) / 48.0f;
                        Vec2 p;
                        if (!vp.Project(origin + (u * Cos(th) + v * Sin(th)) * length, p)) {
                            hasPrev = false;
                            continue;
                        }
                        if (hasPrev) {
                            const f32 d = DistanceToSegment(mouse, prev, p);
                            if (d < best) {
                                best = d;
                                g_gizmo.hot = a;
                            }
                        }
                        prev = p;
                        hasPrev = true;
                    }
                }
            } else {
                for (i32 a = 0; a < 3; ++a) {
                    Vec2 e;
                    if (vp.Project(origin + axes[a] * length, e)) {
                        const f32 d = DistanceToSegment(mouse, o2, e);
                        if (d < best) {
                            best = d;
                            g_gizmo.hot = a;
                        }
                    }
                }
                if (Length(mouse - o2) < 9.0f * s)
                    g_gizmo.hot = 3;
            }
        }
    }

    // --- Drag ---
    const ui::Context& ctx = *ui::GetCurrentContext();
    ZX_UNUSED(ctx);
    const bool pressed = ed.input.WasMousePressed(MouseButton::Left);
    const bool down = ed.input.IsMouseDown(MouseButton::Left);
    Transform* local = ed.world.Get<Transform>(ed.selected);
    if (g_gizmo.active < 0 && g_gizmo.hot >= 0 && pressed && hoveredViewport) {
        g_gizmo.active = g_gizmo.hot;
        ed.BeginContinuousEdit(ed.tool == TransformTool::Move ? "Move" : ed.tool == TransformTool::Rotate ? "Rotate" : "Scale", ed.selected);
        local = ed.world.Get<Transform>(ed.selected); // the guid may have been added (archetype move)
        g_gizmo.startLocal = *local;
        g_gizmo.startWorld = origin;
        g_gizmo.startMouse = mouse;
        g_gizmo.startAngle = Atan2(mouse.y - o2.y, mouse.x - o2.x);
        if (g_gizmo.active < 3)
            ClosestParamOnAxis(ray, origin, axes[g_gizmo.active], g_gizmo.startParam);
    }
    if (g_gizmo.active >= 0) {
        if (!down) {
            g_gizmo.active = -1;
        } else {
            const i32 a = g_gizmo.active;
            const Mat4 parent = ParentWorld(ed, ed.selected);
            Transform t = g_gizmo.startLocal;
            if (ed.tool == TransformTool::Move) {
                Vec3 delta;
                if (a < 3) {
                    f32 param;
                    if (ClosestParamOnAxis(ray, g_gizmo.startWorld, axes[a], param)) {
                        f32 d = param - g_gizmo.startParam;
                        if (ed.snapEnabled)
                            d = Round(d / ed.snapStep) * ed.snapStep;
                        delta = axes[a] * d;
                    }
                } else {
                    // Screen-plane move: intersect with the plane facing the camera.
                    const Vec3 n = Normalize(vp.cameraPos - g_gizmo.startWorld);
                    f32 tp;
                    if (RayIntersectsPlane(ray, Plane::FromPointNormal(g_gizmo.startWorld, n), tp)) {
                        const Ray r0 = vp.MouseRay(g_gizmo.startMouse);
                        f32 t0;
                        if (RayIntersectsPlane(r0, Plane::FromPointNormal(g_gizmo.startWorld, n), t0))
                            delta = ray.At(tp) - r0.At(t0);
                    }
                }
                t.position = InverseAffine(parent).TransformPointAffine(g_gizmo.startWorld + delta);
            } else if (ed.tool == TransformTool::Rotate && a < 3) {
                f32 angle = Atan2(mouse.y - o2.y, mouse.x - o2.x) - g_gizmo.startAngle;
                // Screen y points down; flip so dragging follows the ring as seen from the camera.
                if (Dot(axes[a], vp.cameraPos - g_gizmo.startWorld) > 0.0f)
                    angle = -angle;
                if (ed.snapEnabled)
                    angle = Round(angle / DegToRad(15.0f)) * DegToRad(15.0f);
                Vec3 pt, ps;
                Quat parentRot;
                Decompose(parent, pt, parentRot, ps);
                const Quat worldRot = parentRot * g_gizmo.startLocal.rotation;
                t.rotation = Normalize(Inverse(parentRot) * (Quat::FromAxisAngle(axes[a], angle) * worldRot));
            } else if (ed.tool == TransformTool::Scale) {
                if (a < 3) {
                    f32 param;
                    if (ClosestParamOnAxis(ray, g_gizmo.startWorld, axes[a], param)) {
                        f32 f = Max(1.0f + (param - g_gizmo.startParam) / Max(length, 1e-3f), 0.01f);
                        if (ed.snapEnabled)
                            f = Max(Round(f / 0.1f) * 0.1f, 0.1f);
                        t.scale[a] = g_gizmo.startLocal.scale[a] * f;
                    }
                } else {
                    f32 f = Max(1.0f + (mouse.x - g_gizmo.startMouse.x) / (120.0f * s), 0.01f);
                    if (ed.snapEnabled)
                        f = Max(Round(f / 0.1f) * 0.1f, 0.1f);
                    t.scale = g_gizmo.startLocal.scale * f;
                }
            }
            *local = t;
        }
    }

    // --- Draw ---
    const f32 thick = 2.5f * s;
    const i32 lit = g_gizmo.active >= 0 ? g_gizmo.active : g_gizmo.hot;
    if (ed.tool == TransformTool::Rotate) {
        for (i32 a = 0; a < 3; ++a) {
            const Vec3 u = axes[(a + 1) % 3], v = axes[(a + 2) % 3];
            Vec2 prev;
            bool hasPrev = false;
            const Vec3 toCam = Normalize(vp.cameraPos - origin);
            for (u32 k = 0; k <= 64; ++k) {
                const f32 th = TwoPi * f32(k) / 64.0f;
                const Vec3 dir = u * Cos(th) + v * Sin(th);
                Vec2 p;
                if (!vp.Project(origin + dir * length, p)) {
                    hasPrev = false;
                    continue;
                }
                if (hasPrev) {
                    // Back half of the ring is dimmed.
                    const bool front = Dot(dir, toCam) > -0.05f;
                    const Color c = lit == a ? HotColor : AxisColors[a];
                    dl.AddLine(prev, p, front ? c : WithAlpha(c, 0.25f), front ? thick : 1.5f * s);
                }
                prev = p;
                hasPrev = true;
            }
        }
        dl.AddCircleOutline(o2, 3.0f * s, Color(255, 255, 255, 200), 1.5f * s);
    } else {
        for (i32 a = 0; a < 3; ++a) {
            Vec2 e;
            if (!vp.Project(origin + axes[a] * length, e))
                continue;
            const Color c = lit == a ? HotColor : AxisColors[a];
            const Vec2 dir = SafeNormalize(e - o2, Vec2(1, 0));
            const Vec2 n(-dir.y, dir.x);
            if (ed.tool == TransformTool::Move) {
                const f32 head = 12.0f * s;
                dl.AddLine(o2 + dir * 6.0f * s, e - dir * head * 0.8f, c, thick);
                dl.AddTriangle(e + dir * head * 0.2f, e - dir * head + n * head * 0.38f, e - dir * head - n * head * 0.38f, c);
            } else {
                const f32 box = 5.5f * s;
                dl.AddLine(o2 + dir * 6.0f * s, e, c, thick);
                dl.AddRect(Rect(e - Vec2(box), e + Vec2(box)), c, ui::CornerRadii(1.5f * s));
            }
        }
        const Color centerColor = lit == 3 ? HotColor : Color(240, 240, 240, 220);
        if (ed.tool == TransformTool::Scale)
            dl.AddRect(Rect(o2 - Vec2(6.0f * s), o2 + Vec2(6.0f * s)), centerColor, ui::CornerRadii(1.5f * s));
        else
            dl.AddCircle(o2, 5.0f * s, Color(0, 0, 0, 0), 2.0f * s, centerColor);
    }
    return g_gizmo.hot >= 0 || g_gizmo.active >= 0;
}

void ViewportToolbar(Editor& ed, const Rect& r, f32 s)
{
    ViewportSettings& vs = ed.viewport->Settings();
    ui::SetCursorPos(r.min + Vec2(8.0f * s, 6.0f * s));
    // The last entry is a post-processing view (focus check: near blur red, far blur blue, in focus green).
    static const StringView ViewModes[] = {"Lit", "Base Color", "Normals", "Roughness / Metallic", "LOD", "Shadow Cascades",
                                           "Light Clusters", "Ambient Occlusion", "Local Shadows", "Depth of Field Focus"};
    const u32 current = vs.post.viewMode == 5 ? 9u : Min(vs.scene.debugMode, 8u);
    const String litLabel = String::Format(ZX_ICON_SUN "  {}  " ZX_ICON_CHEVRON_DOWN "##viewmode", ViewModes[current]);
    if (ui::Button(litLabel, Vec2::Zero(), ui::ButtonVariant::Default))
        ui::OpenPopup("##viewmodes");
    if (ui::BeginPopup("##viewmodes")) {
        for (u32 i = 0; i < 10; ++i) {
            if (i == 9)
                ui::Separator();
            if (ui::MenuItem(ViewModes[i], {}, current == i)) {
                vs.scene.debugMode = i == 9 ? 0u : i;
                vs.post.viewMode = i == 9 ? 5u : 0u;
            }
        }
        ui::EndPopup();
    }
    ui::SameLine(0.0f, 4.0f);
    if (ui::Button(ZX_ICON_EYE "  Show  " ZX_ICON_CHEVRON_DOWN "##show"))
        ui::OpenPopup("##showflags");
    if (ui::BeginPopup("##showflags")) {
        ui::MenuItem("Grid", {}, &ed.showGrid);
        ui::MenuItem("Sky & Fog", {}, &vs.useSky);
        ui::MenuItem("Sun Shadows", {}, &vs.scene.shadows.enabled);
        ui::MenuItem("Local Shadows", {}, &vs.scene.shadows.localEnabled);
        ui::MenuItem("Ambient Occlusion", {}, &vs.scene.ao.enabled);
        if (ui::MenuItem("Temporal AA", {}, &vs.taa.enabled))
            ed.viewport->ResetHistory();
        ui::MenuItem("Bloom", {}, &vs.post.bloom);
        ui::MenuItem("Decals", {}, &ed.viewport->features.decals);
        if (ui::MenuItem("Temporal Upscaling (ZTSR)", {}, &vs.taa.upscale))
            ed.viewport->ResetHistory();
        ui::MenuItem("Dynamic Resolution", {}, &vs.dynamicResolution.enabled);
        ui::MenuItem("Reflection Probes", {}, &vs.reflections.probes);
        ui::MenuItem("Screen Space Reflections", {}, &vs.reflections.ssr);
        ui::MenuItem("Post Process Volumes", {}, &ed.showPostVolumes);
        ui::MenuItem("Depth of Field", {}, &ed.showDepthOfField);
        ui::MenuItem("Motion Blur (playing)", {}, &ed.showMotionBlur);
        ui::Separator();
        ui::MenuItem("Night", {}, &ed.environment.night);
        ui::MenuItem("Auto Exposure", {}, &ed.environment.autoExposure);
        ui::EndPopup();
    }
    ui::SameLine(0.0f, 4.0f);
    if (ui::Button(ZX_ICON_SUNRISE "  Sun##sun"))
        ui::OpenPopup("##sun");
    if (ui::BeginPopup("##sun")) {
        if (ed.viewport->LastHdri().IsActive())
            ui::TextDisabled(ed.viewport->HdriSunFound() ? "HDRI Sky active: its sun lights the scene\n(turn it with Rotation in Details)"
                                                         : "HDRI Sky active: these apply with Sun = Scene Sun");
        f32 elevation = RadToDeg(ed.environment.sunElevation), azimuth = RadToDeg(ed.environment.sunAzimuth);
        ui::SetNextItemWidth(160.0f);
        if (ui::SliderFloat("Elevation", &elevation, -5.0f, 89.0f, "{:.0f}°"))
            ed.environment.sunElevation = DegToRad(elevation);
        ui::SetNextItemWidth(160.0f);
        if (ui::SliderFloat("Azimuth", &azimuth, 0.0f, 360.0f, "{:.0f}°"))
            ed.environment.sunAzimuth = DegToRad(azimuth);
        ui::EndPopup();
    }
    // Right: camera speed + frame stats.
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 scale = ed.viewport->LastRenderScale();
    const String resolution = scale < 0.999f || scale > 1.001f
                                  ? String::Format("   {:.0f}%{}", scale * 100.0f, ed.viewport->IsUpscaling() ? " ZTSR" : "")
                                  : String();
    const String stats = String::Format("{:.0f} FPS   {:.1f} ms{}", 1.0f / Max(ed.timer.SmoothedDeltaSeconds(), 1e-4f), ed.gpuFrameMs, resolution);
    const ui::FontHandle font = ui::GetCurrentContext()->Fonts().FindFont("Inter");
    const f32 w = ui::GetCurrentContext()->Fonts().MeasureWidth(font, 12.0f * s, stats);
    const Rect sr(Vec2(r.max.x - w - 18.0f * s, r.min.y + 6.0f * s), Vec2(r.max.x - 8.0f * s, r.min.y + 6.0f * s + ui::GetFrameHeight()));
    ui::GetWindowDrawList().AddRect(sr, Color(0, 0, 0, 120), ui::CornerRadii(4.0f * s));
    ui::GetWindowDrawList().AddTextInRect(font, 12.0f * s, sr, ctx.GetStyle().colors.textDim, stats, ui::TextAlign::Center, false);
    ui::SetCursorPos(Vec2(sr.min.x - 120.0f * s - 6.0f * s, sr.min.y));
    ui::SetNextItemWidth(120.0f);
    ui::DragFloat("##speed", &ed.camera.moveSpeed, 0.05f, 0.1f, 100.0f, ZX_ICON_GAUGE " {:.1f} m/s");
    ui::ItemTooltip("Camera speed (mouse wheel while flying)");
}

} // namespace

void DrawViewportPanel(Editor& ed)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const bool visible = ui::Begin(ViewportTitle, &ed.showViewport, ui::WindowFlags::NoScrollbar);
    if (!visible) {
        ed.viewportRect = Rect();
        ed.viewportHovered = false;
        ui::End();
        return;
    }
    Rect r = ui::GetWindowContentRect();
    r = Rect(Round(r.min), Round(r.max));
    ed.viewportRect = r;
    ed.EnsureViewportTexture(UVec2(u32(Max(r.Width(), 16.0f)), u32(Max(r.Height(), 16.0f))));
    DrawList& dl = ui::GetWindowDrawList();
    dl.AddImage(ed.viewportTexture->GetSrvIndex(), r);

    // Full-viewport invisible item: hover, clicks, drop target.
    ui::SetCursorPos(r.min);
    ui::InvisibleButton("##viewport_surface", r.Size() / s);
    bool surfaceHovered = ui::IsItemHovered() || ui::IsItemActive();
    // While the game camera is shown a click gives the game the input; no fly camera, picking or gizmo on it.
    if (ed.UsingGameCamera()) {
        if (!ed.gameInputCaptured && surfaceHovered && ed.input.WasMousePressed(MouseButton::Left))
            ed.SetGameInputCaptured(true);
        surfaceHovered = false; // no fly camera, picking or gizmo clicks on the game view
    }
    if (const void* payload = ui::AcceptDragDropPayload("asset")) {
        const StringView path(static_cast<const char*>(payload));
        const ViewProjector pv{ed.viewport->ViewProjection(), Inverse(ed.viewport->ViewProjection()), r, ed.camera.position};
        const Ray ray = pv.MouseRay(ui::GetMousePos());
        if (Path::HasExtension(path, ".zmat") || Path::HasExtension(path, ".zmati")) {
            // Materials dropped on a mesh replace its materials.
            const ecs::Entity target = Pick(ed, ray);
            MaterialLibrary::Material* m = ed.materials ? ed.materials->FindByPath(path) : nullptr;
            const Guid guid = m ? m->guid : (ed.materials ? ed.materials->LoadFile(path) : Guid());
            if (target && guid.IsValid()) {
                ed.AssignMaterial(target, guid);
                ed.Select(target);
            }
        } else if ((Path::HasExtension(path, ".hdr") || Path::HasExtension(path, ".exr")) && !ed.IsPlaying()) {
            ed.UseHdriSky(Path::MakeRelative(path, ed.contentRoot)); // panoramas become the sky
        } else {
            f32 t;
            Vec3 pos = ed.camera.position + ed.camera.Forward() * 5.0f;
            if (RayIntersectsPlane(ray, Plane::FromPointNormal(Vec3::Zero(), Vec3::Up()), t) && t > 0.0f && t < 200.0f)
                pos = ray.At(t);
            ed.ImportAsset(path, pos);
        }
    }

    const ViewProjector vp{ed.viewport->ViewProjection(), Inverse(ed.viewport->ViewProjection()), r, ed.camera.position};
    dl.PushClipRect(r);
    if (ed.IsPlaying()) {
        // Play-in-Editor: colored frame + badge (green = playing, amber = paused).
        const ui::Theme& theme = ui::GetCurrentContext()->GetStyle().colors;
        const bool paused = ed.playState == PlayState::Paused;
        const Color tint = paused ? theme.warning : theme.success;
        dl.AddRect(r, Color(0, 0, 0, 0), ui::CornerRadii(), Round(2.0f * s), tint);
        const StringView badge = paused ? ZX_ICON_PAUSE "  Paused  -  F6 resume, F10 step, Esc stop"
                                 : ed.gameInputCaptured ? ZX_ICON_PLAY "  Playing  -  Esc stop, Shift+F1 release mouse, F8 eject"
                                 : ed.UsingGameCamera() ? ZX_ICON_PLAY "  Playing  -  click to control, Esc stop, F8 eject"
                                                        : ZX_ICON_PLAY "  Playing  -  Esc stop, F8 game camera";
        const ui::FontHandle font = ui::GetCurrentContext()->Fonts().FindFont("Inter SemiBold");
        const f32 w = ui::GetCurrentContext()->Fonts().MeasureWidth(font, 12.0f * s, badge) + 24.0f * s;
        const Rect br(Vec2(Round(r.Center().x - w * 0.5f), r.max.y - Round(34.0f * s)), Vec2(Round(r.Center().x + w * 0.5f), r.max.y - Round(10.0f * s)));
        dl.AddRect(br, WithAlpha(Color(0x14, 0x14, 0x14), 0.85f), ui::CornerRadii(12.0f * s), 1.0f, tint);
        dl.AddTextInRect(font, 12.0f * s, br, tint, badge, ui::TextAlign::Center, false);
    }
    DrawLightIcons(ed, vp, dl, s);
    // Brush / Measure take the left mouse button while active; otherwise the gizmo and click-to-select work.
    const bool toolBusy = UpdateViewportTool(ed, vp.MouseRay(ui::GetMousePos()), surfaceHovered && !ed.flying,
                                             [&vp](Vec3 p, Vec2& out) { return vp.Project(p, out); }, dl, s);
    const bool gizmoBusy = toolBusy || Gizmo(ed, vp, dl, surfaceHovered && !ed.flying, s);
    dl.PopClipRect();

    // Camera: RMB fly (WASD/QE), wheel dolly while hovered. While the game camera is shown a click gives the game the input.
    ed.viewportHovered = surfaceHovered;
    if (!ed.flying && surfaceHovered && ed.input.WasMousePressed(MouseButton::Right))
        ed.flying = true;
    if (ed.flying || surfaceHovered) {
        const bool active = ed.camera.Update(ed.input, ed.dt);
        if (ed.flying && !active)
            ed.flying = false;
    }
    // Click to select (when the gizmo didn't take the click).
    static Vec2 s_pressPos;
    if (surfaceHovered && ed.input.WasMousePressed(MouseButton::Left))
        s_pressPos = ui::GetMousePos();
    if (surfaceHovered && ed.input.WasMouseReleased(MouseButton::Left) && !gizmoBusy &&
        Distance(ui::GetMousePos(), s_pressPos) < 4.0f * s)
        ed.Select(Pick(ed, vp.MouseRay(ui::GetMousePos())));

    ViewportToolbar(ed, r, s);
    if (ed.flying) {
        const ui::FontHandle font = ctx.Fonts().FindFont("Inter");
        dl.AddTextInRect(font, 12.0f * s, Rect(Vec2(r.min.x, r.max.y - 26.0f * s), r.max), Color(230, 230, 230, 200),
                         "Flying: WASD move, Q/E down/up, Shift fast, wheel = speed", ui::TextAlign::Center, false);
    }
    ui::End();
}

} // namespace zx::editor
