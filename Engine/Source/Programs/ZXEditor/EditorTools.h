#pragma once

// Editor tools beyond the transform gizmo: Brush (paint models onto surfaces), Measure (distances), Drop to Ground,
// and the Features window (engine subsystems and tools switched on/off per project, stored in the .zproj).

#include "Core/Containers/Function.h"
#include "Core/Math/Math.h"
#include "Core/Serialization/Json.h"
#include "UI/UI.h"

namespace zx::render {
class DebugDraw;
}

namespace zx::editor {

class Editor;

enum class EditorTool : u8
{
    None,    // transform gizmo
    Brush,   // B
    Measure, // M
};

/// One model the brush can place: a snapshot of an entity hierarchy (root first) captured from the scene.
struct BrushItem
{
    String name;
    Array<JsonValue> entities; // SerializeEntity of the hierarchy, root first (guids remapped on placement)
    f32 weight = 1.0f;         // relative chance among the enabled items
    bool enabled = true;
};

struct BrushSettings
{
    f32 radius = 3.0f;       // meters
    f32 density = 2.0f;      // instances per 10 m^2 per dab
    f32 spacing = 1.0f;      // minimum distance between painted instances (m)
    f32 scaleMin = 0.8f;
    f32 scaleMax = 1.25f;
    bool randomYaw = true;
    f32 maxTilt = 0.0f;      // random tilt (degrees)
    f32 alignToNormal = 0.0f; // 0 = upright, 1 = follow the surface
    f32 maxSlope = 45.0f;    // degrees; steeper surfaces are skipped
    f32 offset = 0.0f;       // along the surface normal (sink with negative values)
};

struct ToolState
{
    EditorTool active = EditorTool::None;
    BrushSettings brush;
    Array<BrushItem> items;
    bool painting = false;
    Vec3 lastDab;
    bool hovering = false;
    Vec3 hoverPoint;
    Vec3 hoverNormal = Vec3::Up();
    u32 placedThisStroke = 0;
    // Measure: up to two points.
    u32 measureCount = 0;
    Vec3 measureA, measureB;
    Random rng{0x5eed};
};

/// Activates a tool (clicking the active one again returns to the transform gizmo).
void SetActiveTool(Editor& ed, EditorTool tool);
/// Viewport input of the active tool. `project` maps world -> screen. Returns true when the tool used the mouse
/// (no selection / gizmo this frame).
bool UpdateViewportTool(Editor& ed, const Ray& mouseRay, bool hovered, const Function<bool(Vec3, Vec2&), 32>& project, ui::DrawList& dl,
                        f32 scale);
/// 3D overlay (brush circle, measure line) - called from the viewport's debug-draw pass.
void DrawToolOverlay(Editor& ed, render::DebugDraw& dd);
/// Settings window of the active tool (Brush items and options, Measure readout).
void DrawToolPanel(Editor& ed);
/// Window > Features.
void DrawFeaturesPanel(Editor& ed);
/// One brush stroke at a point (one undo step): paints, or erases with `erase`. Returns the painted-instance count after.
u32 BrushStrokeAt(Editor& ed, Vec3 point, Vec3 normal, bool erase);
/// Adds the selected entity hierarchy as a brush model. Returns false without a valid selection.
bool AddSelectedBrushItem(Editor& ed);
/// Moves the selection down (or up) until its bounds rest on the surface below (undoable). End key.
bool DropSelectedToGround(Editor& ed);

inline constexpr StringView FeaturesTitle = ZX_ICON_TOGGLE_RIGHT "  Features";
inline constexpr StringView ToolTitle = ZX_ICON_PAINTBRUSH "  Tool Settings";

struct FeatureInfo
{
    StringView id;
    StringView name;
    StringView category;
    StringView description;
};
Span<const FeatureInfo> GetFeatures();

} // namespace zx::editor
