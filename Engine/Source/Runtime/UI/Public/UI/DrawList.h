#pragma once

// 2D draw list: resolution-independent primitives evaluated as signed distance fields in the pixel shader
// (rounded rectangles with borders/gradients, soft shadows, anti-aliased lines and triangles, glyphs, images).
// All coordinates are in render-target pixels, y down. Every primitive is one instance of a 6-vertex quad, so the
// whole UI is drawn with a single draw call (see UIRenderer and Shaders/UI/UI.hlsl).
//
//   DrawList& dl = ...;
//   dl.PushClipRect(panelRect);
//   dl.AddShadow(panelRect, Color(0, 0, 0, 120), 6.0f, 16.0f);
//   dl.AddRect(panelRect, theme.panel, CornerRadii(6.0f), 1.0f, theme.border);
//   dl.AddText(font, 14.0f, Vec2(x, y), Color(230, 230, 230), "Hello");
//   dl.PopClipRect();

#include "Core/Containers/Array.h"
#include "Core/Math/Color.h"
#include "Core/Math/Geometry.h"
#include "UI/Font.h"

namespace zx::ui {

enum class PrimitiveType : u32
{
    Rect = 0,
    Shadow = 1,
    Glyph = 2,
    Image = 3,
    Line = 4,
    Triangle = 5,
};

/// GPU layout of one primitive (64 bytes). Keep in sync with Shaders/UI/UI.hlsl.
struct GpuPrimitive
{
    Vec4 rect;        // Rect/Shadow/Glyph/Image: min.xy, max.xy | Line: p0, p1 | Triangle: p0, p1
    Vec4 data;        // Rect/Shadow: corner radii (tl, tr, br, bl) | Glyph: atlas texel rect | Image: uv rect
                      // Line: (thickness, 0, 0, 0) | Triangle: (p2.xy, edge AA mask, 0)
    u32 color0;       // fill (RGBA8, sRGB)
    u32 color1;       // gradient end color
    u32 borderColor;
    u32 header;       // type (4 bits) | gradient (2 bits at 4) | atlas page (4 bits at 8) | clip index (16 bits at 16)
    f32 borderWidth;
    f32 blur;         // Shadow: blur radius
    u32 texture;      // Image: bindless SRV index
    f32 extra;        // Image: corner radius | Rect/Shadow: hollow inset (> 0: the interior inset by this much is
                      // not rasterized - outlines, shadows under opaque shapes)
};
static_assert(sizeof(GpuPrimitive) == 64);

/// GPU layout of a clip region (32 bytes): pixels inside `rect` (and inside the rounded corners) are kept.
struct GpuClip
{
    Vec4 rect;
    f32 radius;
    f32 pad[3];
};
static_assert(sizeof(GpuClip) == 32);

struct CornerRadii
{
    f32 topLeft = 0.0f, topRight = 0.0f, bottomRight = 0.0f, bottomLeft = 0.0f;
    constexpr CornerRadii() = default;
    constexpr explicit CornerRadii(f32 r) : topLeft(r), topRight(r), bottomRight(r), bottomLeft(r) {}
    constexpr CornerRadii(f32 tl, f32 tr, f32 br, f32 bl) : topLeft(tl), topRight(tr), bottomRight(br), bottomLeft(bl) {}
    static constexpr CornerRadii Top(f32 r) { return {r, r, 0.0f, 0.0f}; }
    static constexpr CornerRadii Bottom(f32 r) { return {0.0f, 0.0f, r, r}; }
    static constexpr CornerRadii Left(f32 r) { return {r, 0.0f, 0.0f, r}; }
    static constexpr CornerRadii Right(f32 r) { return {0.0f, r, r, 0.0f}; }
};

enum class GradientDirection : u32
{
    None = 0,
    Vertical = 1,   // color0 at the top, color1 at the bottom
    Horizontal = 2, // color0 left, color1 right
};

enum class TextAlign : u8
{
    Left,
    Center,
    Right,
};

class DrawList
{
public:
    DrawList() = default;
    ZX_NON_COPYABLE(DrawList)

    /// Starts a new frame. `fonts` is used for text; `viewport` is the default clip.
    void Reset(FontSystem* fonts, Rect viewport);

    // --- Clipping ---
    /// Pushes a clip region (intersected with the current one unless `intersect` is false).
    void PushClipRect(Rect rect, f32 radius = 0.0f, bool intersect = true);
    void PopClipRect();
    Rect CurrentClipRect() const { return m_clipStack.Back().rect; }
    /// True if `r` is (at least partially) visible under the current clip.
    bool IsVisible(const Rect& r) const { return r.Intersects(m_clipStack.Back().rect); }

    // --- Shapes ---
    void AddRect(const Rect& rect, Color fill, CornerRadii radii = {}, f32 borderWidth = 0.0f, Color border = {});
    void AddRectGradient(const Rect& rect, Color from, Color to, GradientDirection direction, CornerRadii radii = {},
                         f32 borderWidth = 0.0f, Color border = {});
    /// Border only.
    void AddRectOutline(const Rect& rect, Color color, f32 thickness = 1.0f, CornerRadii radii = {});
    /// Inserts a filled rect below the primitives starting at `index` (e.g. a table row background drawn after its cells).
    void InsertRectAt(u32 index, const Rect& rect, Color fill, CornerRadii radii = {});
    /// Soft shadow of a rounded rect. `hollow`: skip the area under the (opaque) shape itself - much cheaper.
    void AddShadow(const Rect& rect, Color color, f32 radius, f32 blur, Vec2 offset = Vec2::Zero(), bool hollow = true);
    void AddCircle(Vec2 center, f32 radius, Color fill, f32 borderWidth = 0.0f, Color border = {});
    void AddCircleOutline(Vec2 center, f32 radius, Color color, f32 thickness = 1.0f);
    void AddLine(Vec2 p0, Vec2 p1, Color color, f32 thickness = 1.0f);
    void AddPolyline(Span<const Vec2> points, Color color, f32 thickness = 1.0f, bool closed = false);
    /// Cubic Bezier curve (node links, curves).
    void AddBezier(Vec2 p0, Vec2 c0, Vec2 c1, Vec2 p1, Color color, f32 thickness = 1.0f, u32 segments = 0);
    /// Filled triangle; `aaEdges` bit i anti-aliases edge (p_i, p_(i+1)).
    void AddTriangle(Vec2 a, Vec2 b, Vec2 c, Color color, u32 aaEdges = 7);
    /// Filled convex polygon (fan triangulation, interior edges not anti-aliased).
    void AddConvexPolygon(Span<const Vec2> points, Color color);

    // --- Images & text ---
    /// Draws a bindless texture (SRV index); the texture must be in the shader-resource state.
    void AddImage(u32 textureSrv, const Rect& rect, Vec2 uv0 = Vec2(0, 0), Vec2 uv1 = Vec2(1, 1), Color tint = Color(255, 255, 255),
                  f32 radius = 0.0f);
    /// Draws a monochrome SVG icon fitted into `rect` (square pixel size = the smaller side), tinted with `color`.
    void AddSvg(const struct SvgImage& image, const Rect& rect, Color color);
    /// Draws text with its first line box at `pos` (top-left). Lines break on '\n' and, if wrapWidth > 0, at word
    /// boundaries. Returns the size of the text block.
    Vec2 AddText(FontHandle font, f32 size, Vec2 pos, Color color, StringView text, f32 wrapWidth = 0.0f);
    /// Single line aligned horizontally inside `rect` and centered vertically; truncated with "…" if too long.
    void AddTextInRect(FontHandle font, f32 size, const Rect& rect, Color color, StringView text,
                       TextAlign align = TextAlign::Left, bool ellipsis = true);

    // --- Output ---
    Span<const GpuPrimitive> Primitives() const { return m_prims.View(); }
    Span<const GpuClip> Clips() const { return m_clips.View(); }
    u32 PrimitiveCount() const { return u32(m_prims.Size()); }
    FontSystem* Fonts() const { return m_fonts; }

private:
    struct ClipState
    {
        Rect rect;
        u32 index;
    };
    GpuPrimitive& Emit(PrimitiveType type, const Rect& bounds);
    void DrawLine(FontHandle font, f32 size, f32 x, f32 baseline, Color color, StringView line);

    FontSystem* m_fonts = nullptr;
    Array<GpuPrimitive> m_prims;
    Array<GpuClip> m_clips;
    Array<ClipState> m_clipStack;
    Array<ShapedGlyph> m_shaped; // scratch
    GpuPrimitive m_discard;      // target for culled primitives
};

/// Packs a color for GpuPrimitive.
inline u32 PackColor(Color c) { return c.ToPackedABGR(); }
/// Color with alpha multiplied by `alpha` in [0,1].
inline Color WithAlpha(Color c, f32 alpha) { return Color(c.r, c.g, c.b, u8(Clamp(f32(c.a) * alpha, 0.0f, 255.0f) + 0.5f)); }
/// Linear interpolation of sRGB-encoded colors (perceptual enough for UI hover/press blends).
Color LerpColor(Color a, Color b, f32 t);

} // namespace zx::ui
