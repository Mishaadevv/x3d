#include "UI/Svg.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Hash/Hash.h"
#include "Core/String/Parse.h"

namespace zx::ui {

namespace {

/// 2x3 affine transform: p' = (a*x + c*y + e, b*x + d*y + f).
struct Affine
{
    f32 a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Vec2 Apply(Vec2 p) const { return Vec2(a * p.x + c * p.y + e, b * p.x + d * p.y + f); }
    Affine operator*(const Affine& o) const // this after o
    {
        return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d, a * o.e + c * o.f + e, b * o.e + d * o.f + f};
    }
    f32 Scale() const { return Sqrt(Abs(a * d - b * c)); }
};

struct Style
{
    bool fill = true; // SVG default: black fill, no stroke
    bool stroke = false;
    f32 strokeWidth = 1.0f;
    Affine transform;
};

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ','; }

/// Number scanner for path data / point lists (handles "1-2", ".5.5", exponents).
struct NumberReader
{
    StringView s;
    usize i = 0;
    void Skip()
    {
        while (i < s.Size() && IsSpace(s[i]))
            ++i;
    }
    bool AtNumber()
    {
        Skip();
        return i < s.Size() && (s[i] == '-' || s[i] == '+' || s[i] == '.' || (s[i] >= '0' && s[i] <= '9'));
    }
    bool Read(f32& out)
    {
        Skip();
        const usize start = i;
        if (i < s.Size() && (s[i] == '-' || s[i] == '+'))
            ++i;
        bool dot = false, digits = false;
        while (i < s.Size()) {
            const char ch = s[i];
            if (ch >= '0' && ch <= '9') {
                digits = true;
                ++i;
            } else if (ch == '.' && !dot) {
                dot = true;
                ++i;
            } else {
                break;
            }
        }
        if (digits && i < s.Size() && (s[i] == 'e' || s[i] == 'E')) {
            usize j = i + 1;
            if (j < s.Size() && (s[j] == '-' || s[j] == '+'))
                ++j;
            if (j < s.Size() && s[j] >= '0' && s[j] <= '9') {
                i = j;
                while (i < s.Size() && s[i] >= '0' && s[i] <= '9')
                    ++i;
            }
        }
        if (!digits)
            return false;
        out = ParseFloat(s.SubStr(start, i - start)).ValueOr(0.0f);
        return true;
    }
    bool ReadFlag(bool& out) // arc flags may be written without separators ("0 01")
    {
        Skip();
        if (i < s.Size() && (s[i] == '0' || s[i] == '1')) {
            out = s[i] == '1';
            ++i;
            return true;
        }
        return false;
    }
};

struct PathBuilder
{
    SvgShape& shape;
    const Affine& xf;
    Vec2 current, start, lastControl;
    char lastCmd = 0;

    void Begin(Vec2 p)
    {
        shape.contours.AddDefaulted();
        shape.closed.PushBack(false);
        shape.contours.Back().PushBack(xf.Apply(p));
        current = start = p;
    }
    void Line(Vec2 p)
    {
        if (shape.contours.Empty())
            Begin(current);
        shape.contours.Back().PushBack(xf.Apply(p));
        current = p;
    }
    void Cubic(Vec2 c0, Vec2 c1, Vec2 p)
    {
        const f32 len = Length(c0 - current) + Length(c1 - c0) + Length(p - c1);
        const u32 n = Clamp(u32(len * 0.6f), 6u, 32u);
        const Vec2 p0 = current;
        for (u32 k = 1; k <= n; ++k) {
            const f32 t = f32(k) / f32(n), u = 1.0f - t;
            Line(p0 * (u * u * u) + c0 * (3.0f * u * u * t) + c1 * (3.0f * u * t * t) + p * (t * t * t));
        }
        lastControl = c1;
    }
    void Quad(Vec2 c, Vec2 p)
    {
        const f32 len = Length(c - current) + Length(p - c);
        const u32 n = Clamp(u32(len * 0.6f), 4u, 24u);
        const Vec2 p0 = current;
        for (u32 k = 1; k <= n; ++k) {
            const f32 t = f32(k) / f32(n), u = 1.0f - t;
            Line(p0 * (u * u) + c * (2.0f * u * t) + p * (t * t));
        }
        lastControl = c;
    }
    /// Elliptical arc (SVG endpoint parameterization, spec F.6.5).
    void Arc(f32 rx, f32 ry, f32 angleDeg, bool large, bool sweep, Vec2 p)
    {
        const Vec2 p0 = current;
        if (p0 == p)
            return;
        rx = Abs(rx);
        ry = Abs(ry);
        if (rx < 1e-6f || ry < 1e-6f) {
            Line(p);
            return;
        }
        const f32 phi = DegToRad(angleDeg), cs = Cos(phi), sn = Sin(phi);
        const Vec2 d = (p0 - p) * 0.5f;
        const Vec2 q(cs * d.x + sn * d.y, -sn * d.x + cs * d.y);
        const f32 lambda = (q.x * q.x) / (rx * rx) + (q.y * q.y) / (ry * ry);
        if (lambda > 1.0f) {
            rx *= Sqrt(lambda);
            ry *= Sqrt(lambda);
        }
        const f32 num = rx * rx * ry * ry - rx * rx * q.y * q.y - ry * ry * q.x * q.x;
        const f32 den = rx * rx * q.y * q.y + ry * ry * q.x * q.x;
        f32 k = den > 0.0f ? Sqrt(Max(num / den, 0.0f)) : 0.0f;
        if (large == sweep)
            k = -k;
        const Vec2 cq(k * rx * q.y / ry, -k * ry * q.x / rx);
        const Vec2 center(cs * cq.x - sn * cq.y + (p0.x + p.x) * 0.5f, sn * cq.x + cs * cq.y + (p0.y + p.y) * 0.5f);
        auto angle = [](Vec2 u, Vec2 v) {
            const f32 a = Atan2(u.x * v.y - u.y * v.x, u.x * v.x + u.y * v.y);
            return a;
        };
        const Vec2 v1((q.x - cq.x) / rx, (q.y - cq.y) / ry), v2((-q.x - cq.x) / rx, (-q.y - cq.y) / ry);
        const f32 theta = angle(Vec2(1, 0), v1);
        f32 delta = angle(v1, v2);
        if (!sweep && delta > 0.0f)
            delta -= TwoPi;
        else if (sweep && delta < 0.0f)
            delta += TwoPi;
        const u32 n = Clamp(u32(Abs(delta) / (Pi / 12.0f)) + 1, 2u, 48u);
        for (u32 i = 1; i <= n; ++i) {
            const f32 t = theta + delta * f32(i) / f32(n);
            const Vec2 e(rx * Cos(t), ry * Sin(t));
            Line(i == n ? p : Vec2(cs * e.x - sn * e.y + center.x, sn * e.x + cs * e.y + center.y));
        }
        lastControl = p;
    }
    void Close()
    {
        if (!shape.contours.Empty()) {
            shape.closed.Back() = true;
            current = start;
        }
    }
};

void ParsePath(StringView d, PathBuilder& b)
{
    NumberReader r{d};
    char cmd = 0;
    while (true) {
        r.Skip();
        if (r.i >= d.Size())
            break;
        const char ch = d[r.i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            cmd = ch;
            ++r.i;
        } else if (!cmd) {
            break;
        }
        const bool rel = cmd >= 'a' && cmd <= 'z';
        const Vec2 base = rel ? b.current : Vec2::Zero();
        f32 v[7];
        auto read = [&](u32 count) {
            for (u32 k = 0; k < count; ++k)
                if (!r.Read(v[k]))
                    return false;
            return true;
        };
        const char lower = char(cmd | 0x20);
        const bool smoothOk = b.lastCmd == 'c' || b.lastCmd == 's' || b.lastCmd == 'q' || b.lastCmd == 't';
        switch (lower) {
        case 'm':
            if (!read(2))
                return;
            b.Begin(base + Vec2(v[0], v[1]));
            cmd = rel ? 'l' : 'L'; // following pairs are line-tos
            break;
        case 'l':
            if (!read(2))
                return;
            b.Line(base + Vec2(v[0], v[1]));
            break;
        case 'h':
            if (!read(1))
                return;
            b.Line(Vec2(rel ? b.current.x + v[0] : v[0], b.current.y));
            break;
        case 'v':
            if (!read(1))
                return;
            b.Line(Vec2(b.current.x, rel ? b.current.y + v[0] : v[0]));
            break;
        case 'c':
            if (!read(6))
                return;
            b.Cubic(base + Vec2(v[0], v[1]), base + Vec2(v[2], v[3]), base + Vec2(v[4], v[5]));
            break;
        case 's': {
            if (!read(4))
                return;
            const Vec2 c0 = smoothOk && (b.lastCmd == 'c' || b.lastCmd == 's') ? b.current * 2.0f - b.lastControl : b.current;
            b.Cubic(c0, base + Vec2(v[0], v[1]), base + Vec2(v[2], v[3]));
            break;
        }
        case 'q':
            if (!read(4))
                return;
            b.Quad(base + Vec2(v[0], v[1]), base + Vec2(v[2], v[3]));
            break;
        case 't': {
            if (!read(2))
                return;
            const Vec2 c = smoothOk && (b.lastCmd == 'q' || b.lastCmd == 't') ? b.current * 2.0f - b.lastControl : b.current;
            b.Quad(c, base + Vec2(v[0], v[1]));
            break;
        }
        case 'a': {
            bool large = false, sweep = false;
            if (!read(3) || !r.ReadFlag(large) || !r.ReadFlag(sweep) || !r.Read(v[3]) || !r.Read(v[4]))
                return;
            b.Arc(v[0], v[1], v[2], large, sweep, base + Vec2(v[3], v[4]));
            break;
        }
        case 'z':
            b.Close();
            break;
        default:
            return; // unknown command: stop
        }
        b.lastCmd = lower;
        if (lower == 'z')
            cmd = 0;
    }
}

Affine ParseTransform(StringView t)
{
    Affine m;
    usize i = 0;
    while (i < t.Size()) {
        const usize open = t.SubStr(i).Find('(');
        if (open == StringView::NPos)
            break;
        const StringView name = t.SubStr(i, open).Trim();
        const usize close = t.SubStr(i + open).Find(')');
        if (close == StringView::NPos)
            break;
        NumberReader r{t.SubStr(i + open + 1, close - 1)};
        f32 a[6] = {};
        u32 n = 0;
        while (n < 6 && r.Read(a[n]))
            ++n;
        Affine x;
        if (name == "translate")
            x.e = a[0], x.f = n > 1 ? a[1] : 0.0f;
        else if (name == "scale")
            x.a = a[0], x.d = n > 1 ? a[1] : a[0];
        else if (name == "rotate") {
            const f32 rad = DegToRad(a[0]), cs = Cos(rad), sn = Sin(rad);
            Affine rot{cs, sn, -sn, cs, 0, 0};
            if (n >= 3) {
                Affine to{1, 0, 0, 1, a[1], a[2]}, back{1, 0, 0, 1, -a[1], -a[2]};
                x = to * rot * back;
            } else {
                x = rot;
            }
        } else if (name == "matrix" && n == 6)
            x = Affine{a[0], a[1], a[2], a[3], a[4], a[5]};
        m = m * x;
        i += open + close + 1;
    }
    return m;
}

/// Attribute value of `name` inside an element's attribute text (or empty).
StringView Attr(StringView attrs, StringView name)
{
    usize i = 0;
    while (i < attrs.Size()) {
        while (i < attrs.Size() && IsSpace(attrs[i]))
            ++i;
        const usize nameStart = i;
        while (i < attrs.Size() && attrs[i] != '=' && !IsSpace(attrs[i]))
            ++i;
        const StringView key = attrs.SubStr(nameStart, i - nameStart);
        while (i < attrs.Size() && (IsSpace(attrs[i]) || attrs[i] == '='))
            ++i;
        if (i >= attrs.Size() || (attrs[i] != '"' && attrs[i] != '\''))
            break;
        const char q = attrs[i++];
        const usize valueStart = i;
        while (i < attrs.Size() && attrs[i] != q)
            ++i;
        const StringView value = attrs.SubStr(valueStart, i - valueStart);
        ++i;
        if (key == name)
            return value;
    }
    return {};
}

/// Inline style="fill:none; stroke-width:2" declarations.
StringView StyleProp(StringView style, StringView name)
{
    StringView rest = style;
    while (!rest.Empty()) {
        StringView decl;
        rest.Split(';', decl, rest);
        StringView key, value;
        if (decl.Split(':', key, value) && key.Trim() == name)
            return value.Trim();
    }
    return {};
}

void ApplyStyle(StringView attrs, Style& s)
{
    const StringView inlineStyle = Attr(attrs, "style");
    auto get = [&](StringView name) {
        const StringView v = StyleProp(inlineStyle, name);
        return v.Empty() ? Attr(attrs, name) : v;
    };
    if (const StringView fill = get("fill"); !fill.Empty())
        s.fill = fill != "none" && fill != "transparent";
    if (const StringView stroke = get("stroke"); !stroke.Empty())
        s.stroke = stroke != "none" && stroke != "transparent";
    if (const StringView w = get("stroke-width"); !w.Empty())
        s.strokeWidth = ParseFloat(w).ValueOr(s.strokeWidth);
    if (const StringView t = Attr(attrs, "transform"); !t.Empty())
        s.transform = s.transform * ParseTransform(t);
}

f32 AttrF(StringView attrs, StringView name, f32 fallback = 0.0f)
{
    const StringView v = Attr(attrs, name);
    return v.Empty() ? fallback : ParseFloat(v).ValueOr(fallback);
}

void PointList(StringView text, PathBuilder& b, bool close)
{
    NumberReader r{text};
    f32 x, y;
    bool first = true;
    while (r.Read(x) && r.Read(y)) {
        if (first)
            b.Begin(Vec2(x, y));
        else
            b.Line(Vec2(x, y));
        first = false;
    }
    if (close)
        b.Close();
}

// --- Rasterization -------------------------------------------------------------------------------------------------------

f32 SignedArea(Span<const Vec2> poly)
{
    f32 a = 0.0f;
    for (usize i = 0; i < poly.Size(); ++i) {
        const Vec2 p = poly[i], q = poly[(i + 1) % poly.Size()];
        a += p.x * q.y - q.x * p.y;
    }
    return a * 0.5f;
}

/// Adds a polygon with a fixed winding so overlapping stroke pieces add up (and never cancel).
void AddPolygon(PathRasterizer& r, Span<const Vec2> poly)
{
    if (poly.Size() < 3)
        return;
    const bool reverse = SignedArea(poly) < 0.0f;
    r.MoveTo(poly[reverse ? poly.Size() - 1 : 0]);
    for (usize k = 1; k < poly.Size(); ++k)
        r.LineTo(poly[reverse ? poly.Size() - 1 - k : k]);
    r.Close();
}

void AddDisc(PathRasterizer& r, Vec2 c, f32 radius)
{
    Vec2 pts[16];
    for (u32 i = 0; i < 16; ++i) {
        const f32 a = TwoPi * f32(i) / 16.0f;
        pts[i] = c + Vec2(Cos(a), Sin(a)) * radius;
    }
    AddPolygon(r, Span<const Vec2>(pts, 16));
}

} // namespace

Result<SvgImage> ParseSvg(StringView text)
{
    SvgImage img;
    img.id = HashXX64(text);
    Array<Style> stack;
    stack.PushBack(Style{});
    bool sawSvg = false;
    usize i = 0;
    while (i < text.Size()) {
        const usize lt = text.SubStr(i).Find('<');
        if (lt == StringView::NPos)
            break;
        i += lt + 1;
        if (i >= text.Size())
            break;
        if (text.SubStr(i).StartsWith("!--")) {
            const usize end = text.SubStr(i).Find("-->");
            i = end == StringView::NPos ? text.Size() : i + end + 3;
            continue;
        }
        if (text[i] == '?' || text[i] == '!') {
            const usize end = text.SubStr(i).Find('>');
            i = end == StringView::NPos ? text.Size() : i + end + 1;
            continue;
        }
        const usize gt = text.SubStr(i).Find('>');
        if (gt == StringView::NPos)
            break;
        StringView tag = text.SubStr(i, gt);
        i += gt + 1;
        if (tag.StartsWith("/")) {
            const StringView name = tag.SubStr(1).Trim();
            if ((name == "g" || name == "svg" || name == "a") && stack.Size() > 1)
                stack.PopBack();
            continue;
        }
        const bool selfClosing = tag.EndsWith("/");
        if (selfClosing)
            tag = tag.SubStr(0, tag.Size() - 1);
        usize nameEnd = 0;
        while (nameEnd < tag.Size() && !IsSpace(tag[nameEnd]))
            ++nameEnd;
        const StringView name = tag.SubStr(0, nameEnd);
        const StringView attrs = tag.SubStr(nameEnd);
        Style style = stack.Back();
        ApplyStyle(attrs, style);

        if (name == "svg") {
            sawSvg = true;
            NumberReader r{Attr(attrs, "viewBox")};
            f32 vb[4];
            if (r.Read(vb[0]) && r.Read(vb[1]) && r.Read(vb[2]) && r.Read(vb[3])) {
                img.origin = Vec2(vb[0], vb[1]);
                img.size = Vec2(vb[2], vb[3]);
            } else {
                img.size = Vec2(AttrF(attrs, "width", 24.0f), AttrF(attrs, "height", 24.0f));
            }
        }
        if (name == "svg" || name == "g" || name == "a") {
            if (!selfClosing)
                stack.PushBack(style);
            continue;
        }
        if (!style.fill && !style.stroke)
            continue;
        SvgShape shape;
        shape.fill = style.fill;
        shape.strokeWidth = style.stroke ? style.strokeWidth * style.transform.Scale() : 0.0f;
        PathBuilder b{shape, style.transform};
        if (name == "path") {
            ParsePath(Attr(attrs, "d"), b);
        } else if (name == "circle" || name == "ellipse") {
            const Vec2 c(AttrF(attrs, "cx"), AttrF(attrs, "cy"));
            const f32 rx = name == "circle" ? AttrF(attrs, "r") : AttrF(attrs, "rx"), ry = name == "circle" ? rx : AttrF(attrs, "ry");
            if (rx <= 0.0f || ry <= 0.0f)
                continue;
            const u32 n = Clamp(u32(Max(rx, ry) * 4.0f), 24u, 96u);
            for (u32 k = 0; k <= n; ++k) {
                const f32 a = TwoPi * f32(k) / f32(n);
                const Vec2 p = c + Vec2(Cos(a) * rx, Sin(a) * ry);
                if (k == 0)
                    b.Begin(p);
                else if (k < n)
                    b.Line(p);
            }
            b.Close();
        } else if (name == "rect") {
            const f32 x = AttrF(attrs, "x"), y = AttrF(attrs, "y"), w = AttrF(attrs, "width"), h = AttrF(attrs, "height");
            f32 rx = AttrF(attrs, "rx", -1.0f), ry = AttrF(attrs, "ry", -1.0f);
            if (rx < 0.0f)
                rx = Max(ry, 0.0f);
            if (ry < 0.0f)
                ry = rx;
            rx = Min(rx, w * 0.5f);
            ry = Min(ry, h * 0.5f);
            if (w <= 0.0f || h <= 0.0f)
                continue;
            if (rx <= 0.0f) {
                b.Begin(Vec2(x, y));
                b.Line(Vec2(x + w, y));
                b.Line(Vec2(x + w, y + h));
                b.Line(Vec2(x, y + h));
            } else {
                b.Begin(Vec2(x + rx, y));
                b.Line(Vec2(x + w - rx, y));
                b.Arc(rx, ry, 0, false, true, Vec2(x + w, y + ry));
                b.Line(Vec2(x + w, y + h - ry));
                b.Arc(rx, ry, 0, false, true, Vec2(x + w - rx, y + h));
                b.Line(Vec2(x + rx, y + h));
                b.Arc(rx, ry, 0, false, true, Vec2(x, y + h - ry));
                b.Line(Vec2(x, y + ry));
                b.Arc(rx, ry, 0, false, true, Vec2(x + rx, y));
            }
            b.Close();
        } else if (name == "line") {
            b.Begin(Vec2(AttrF(attrs, "x1"), AttrF(attrs, "y1")));
            b.Line(Vec2(AttrF(attrs, "x2"), AttrF(attrs, "y2")));
            shape.fill = false;
        } else if (name == "polyline" || name == "polygon") {
            PointList(Attr(attrs, "points"), b, name == "polygon");
        } else {
            continue; // unsupported element (text, image, gradients...)
        }
        if (!shape.contours.Empty())
            img.shapes.PushBack(move(shape));
    }
    if (!sawSvg)
        return Error::Make(ErrorCode::InvalidData, "Not an SVG document");
    return img;
}

Result<SvgImage> LoadSvg(StringView path)
{
    String text;
    if (!fs::ReadTextFile(path, text))
        return Error::Make(ErrorCode::NotFound, "Can't read {}", path);
    return ParseSvg(text.View());
}

bool RasterizeSvg(const SvgImage& image, u32 pixelSize, GlyphBitmap& out)
{
    if (pixelSize == 0 || image.size.x <= 0.0f || image.size.y <= 0.0f)
        return false;
    const f32 scale = f32(pixelSize) / Max(image.size.x, image.size.y);
    const Vec2 offset = (Vec2(f32(pixelSize)) - image.size * scale) * 0.5f;
    auto toPixels = [&](Vec2 p) { return (p - image.origin) * scale + offset; };

    PathRasterizer fill(pixelSize, pixelSize), stroke(pixelSize, pixelSize);
    bool anyFill = false, anyStroke = false;
    Array<Vec2> pts;
    for (const SvgShape& s : image.shapes) {
        for (usize c = 0; c < s.contours.Size(); ++c) {
            pts.Clear();
            for (Vec2 p : s.contours[c])
                pts.PushBack(toPixels(p));
            if (pts.Size() < 2)
                continue;
            if (s.fill && pts.Size() >= 3) {
                fill.MoveTo(pts[0]);
                for (usize k = 1; k < pts.Size(); ++k)
                    fill.LineTo(pts[k]);
                fill.Close();
                anyFill = true;
            }
            if (s.strokeWidth > 0.0f) {
                // Stroke = union of segment quads + round joins/caps (all with the same winding).
                const f32 h = Max(s.strokeWidth * scale * 0.5f, 0.35f);
                const usize segments = s.closed[c] ? pts.Size() : pts.Size() - 1;
                for (usize k = 0; k < segments; ++k) {
                    const Vec2 a = pts[k], b = pts[(k + 1) % pts.Size()];
                    const Vec2 d = b - a;
                    const f32 len = Length(d);
                    if (len < 1e-4f)
                        continue;
                    const Vec2 n = Vec2(-d.y, d.x) / len * h;
                    const Vec2 quad[4] = {a + n, b + n, b - n, a - n};
                    AddPolygon(stroke, Span<const Vec2>(quad, 4));
                }
                for (usize k = 0; k < pts.Size(); ++k)
                    AddDisc(stroke, pts[k], h);
                anyStroke = true;
            }
        }
    }
    out.width = out.height = pixelSize;
    out.left = out.top = 0;
    out.pixels.ResizeZeroed(usize(pixelSize) * pixelSize);
    Array<u8> tmp;
    tmp.ResizeZeroed(usize(pixelSize) * pixelSize);
    if (anyFill)
        fill.Resolve(out.pixels.Data(), pixelSize);
    if (anyStroke) {
        stroke.Resolve(tmp.Data(), pixelSize);
        for (usize i = 0; i < tmp.Size(); ++i)
            out.pixels[i] = Max(out.pixels[i], tmp[i]);
    }
    return anyFill || anyStroke;
}

} // namespace zx::ui
