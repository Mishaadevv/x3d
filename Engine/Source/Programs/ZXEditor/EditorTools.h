#pragma once

// Editor tools beyond the transform gizmo: Brush (paint models onto surfaces), Terrain (sculpt and paint terrains),
// Measure (distances), Drop to Ground,
// and the Features window (engine subsystems and tools switched on/off per project, stored in the .zproj).

#include "Core/Containers/Function.h"
#include "Core/Math/Math.h"
#include "Core/Serialization/Json.h"
#include "Scene/ECS/World.h"
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
    Terrain, // T: sculpt / paint Terrain components
    UIDesigner, // U: build game UI (UI Canvases) with the mouse
};

enum class TerrainBrushMode : u8
{
    Raise,
    Lower,
    Smooth,
    Flatten,   // towards the height where the stroke started
    SetHeight, // towards Target Height (Alt+click picks it from the ground)
    Noise,     // rough it up
    Terrace,   // steps of Terrace Height (rice fields, mesas)
    Erode,     // material slides down steep slopes (thermal erosion): natural screes and valleys
    Ramp,      // drag from start to end: a straight ramp / road as wide as the brush
    PaintGrass,
    PaintRock,
    PaintDirt,
    PaintSand,
    ErasePaint, // back to the material's automatic grass / rock / snow
    Count
};
inline bool IsTerrainPaintMode(TerrainBrushMode m) { return m >= TerrainBrushMode::PaintGrass; }

enum class TerrainBrushShape : u8
{
    Circle,
    Square,
};

struct TerrainBrushSettings
{
    TerrainBrushMode mode = TerrainBrushMode::Raise;
    TerrainBrushShape shape = TerrainBrushShape::Circle;
    f32 radius = 8.0f;    // m
    f32 strength = 0.5f;  // 0..1
    f32 falloff = 0.85f;  // 0 = hard edge, 1 = soft all the way to the center
    f32 targetHeight = 0.0f;  // Set Height / Flatten All (m, the terrain's local height)
    f32 noiseScale = 0.33f;   // Noise: lump size as a fraction of the radius
    f32 terraceHeight = 2.0f; // m per step
    f32 terraceSharpness = 0.8f; // 0 = soft steps, 1 = vertical walls
    f32 erodeAngle = 35.0f;   // degrees: steeper slopes crumble
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
    TerrainBrushSettings terrain;
    Array<BrushItem> items;
    bool painting = false;
    Vec3 lastDab;
    bool hovering = false;
    Vec3 hoverPoint;
    Vec3 hoverNormal = Vec3::Up();
    u32 placedThisStroke = 0;
    // Terrain tool: the terrain under the mouse and the stroke in progress.
    ecs::Entity hoverTerrain;
    ecs::Entity sculptTerrain;
    bool sculpting = false;
    f32 flattenHeight = 0.0f; // local height of the stroke's start
    bool rampDragging = false;
    bool focusPanel = false;  // bring the Tool Settings tab to front once it exists
    Vec3 rampStart;           // Ramp: where the drag began (world)
    f32 rampStartHeight = 0.0f; // local
    // UI Designer.
    ecs::Entity uiHover;
    ecs::Entity uiDragged;
    bool uiDragging = false;
    bool uiMoved = false;
    i32 uiHandle = -1; // 0..7 resize handles, 8 = the body
    i32 uiInsert = -1; // reorder: index among the laid-out siblings
    Vec2 uiPressMouse;
    Rect uiStartRect;
    Vec2 uiStartOffset, uiStartSize;
    u32 uiPreview = 0; // preview screen shape (0 = the viewport)
    bool uiSnap = true;
    f32 uiGrid = 4.0f; // UI units
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
/// UI Designer: the screen rect the game UI is laid out in (a preview shape inside the viewport, or the viewport).
Rect UIDesignerScreen(Editor& ed, const Rect& viewport);
/// UI Designer: picking, moving, resizing and its overlay (after the game UI is drawn). True = it used the mouse.
bool UpdateUIDesigner(Editor& ed, const Rect& viewport, const Rect& screen, bool hovered, ui::DrawList& dl, f32 s);
void DrawUIDesignerPanel(Editor& ed);
/// Window > Features.
void DrawFeaturesPanel(Editor& ed);
/// One brush stroke at a point (one undo step): paints, or erases with `erase`. Returns the painted-instance count after.
u32 BrushStrokeAt(Editor& ed, Vec3 point, Vec3 normal, bool erase);
/// Adds the selected entity hierarchy as a brush model. Returns false without a valid selection.
bool AddSelectedBrushItem(Editor& ed);
/// One frame of a terrain stroke at world `point` (the editor's live terrain data; Commit writes the component).
void TerrainDab(Editor& ed, ecs::Entity terrain, Vec3 point, f32 dt);
/// Ramp from `from` to `to` (world) as wide as the brush (one undo step).
void TerrainRamp(Editor& ed, ecs::Entity terrain, Vec3 from, Vec3 to);
/// Whole-terrain operations (one undo step each).
enum class TerrainOp : u8
{
    FlattenAll, // every sample to Target Height
    SmoothAll,
    ResetSculpt,
    ClearPaint,
    PaintBySlope, // rock on steep slopes, dirt on mid slopes
};
void TerrainOperation(Editor& ed, ecs::Entity terrain, TerrainOp op);
/// Heightmaps: PNG (8 / 16-bit, grey = height from 0 to `heightRange` m) or RAW 16-bit (.r16 / .raw, square,
/// little-endian), resampled to the terrain's resolution; export writes RAW 16-bit + its height range in the log.
bool ImportTerrainHeightmap(Editor& ed, ecs::Entity terrain, StringView path, f32 heightRange);
bool ExportTerrainHeightmap(Editor& ed, ecs::Entity terrain, StringView path);
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
