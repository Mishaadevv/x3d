// FBX importer (own implementation): binary FBX 7.x (32-bit node records up to 7.4, 64-bit from 7.5, zlib-compressed
// arrays). Meshes (polygons with normals / UV sets / colors / per-polygon materials), Phong / Lambert materials with
// textures (files next to the model or embedded), the node hierarchy with pivots and pre / post rotations, skins
// (clusters -> one skeleton per armature) and animation stacks resampled into bone tracks.
//
// Everything is first converted into a right-handed Y-up space in meters (the file's axis system and unit scale),
// then mirrored into engine space like glTF; node transforms are converted by conjugation (C * M * C^-1), so pivots,
// rotation orders and axis systems all go through the same exact path.

#include "Asset/SceneImport.h"
#include "Core/Compression/Compression.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Containers/HashMap.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Math/Packing.h"
#include "Core/String/Path.h"

#include <cmath>
#include <cstring>

namespace zx::asset {

namespace {

constexpr char BinaryMagic[] = "Kaydara FBX Binary  "; // followed by "\0\x1a\0" and the u32 version
constexpr usize HeaderSize = 27;
constexpr f64 KTimeSecond = 46186158000.0;
constexpr u32 MaxDepth = 64;
constexpr f64 MaxClipSeconds = 3600.0;

template <typename T>
T ReadLE(const u8* p)
{
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

// --- Binary document ---------------------------------------------------------------------------------------------

struct Property
{
    char type = 0;
    i64 i = 0;     // Y C I L
    f64 d = 0.0;   // F D
    ByteSpan data; // S R: the bytes; arrays: the payload as stored
    u32 count = 0; // arrays: element count
    u32 encoding = 0;

    f64 Number() const { return type == 'F' || type == 'D' ? d : f64(i); }
    StringView Str() const { return type == 'S' || type == 'R' ? StringView(reinterpret_cast<const char*>(data.Data()), data.Size()) : StringView(); }
};

struct Node
{
    StringView name;
    u32 firstProp = 0;
    u32 propCount = 0;
    Array<u32> children;
};

class Document
{
public:
    Result<void> Parse(ByteSpan bytes)
    {
        m_bytes = bytes;
        if (bytes.Size() < HeaderSize || std::memcmp(bytes.Data(), BinaryMagic, 20) != 0) {
            if (bytes.Size() >= 5 && (StringView(reinterpret_cast<const char*>(bytes.Data()), Min(bytes.Size(), usize(256))).Contains("FBX")))
                return Error::Make(ErrorCode::Unsupported, "FBX: ASCII files are not supported (re-export as binary FBX)");
            return Error::Make(ErrorCode::InvalidData, "FBX: not a binary FBX file");
        }
        version = ReadLE<u32>(bytes.Data() + 23);
        if (version < 7000 || version >= 8000)
            return Error::Make(ErrorCode::Unsupported, "FBX: version {} is not supported (7.x only)", version);
        usize pos = HeaderSize;
        while (pos < bytes.Size()) {
            i64 node = -1;
            ZX_TRY(ParseNode(pos, bytes.Size(), 0, node));
            if (node < 0)
                break;
            roots.PushBack(u32(node));
        }
        return {};
    }

    const Node* Root(StringView name) const
    {
        for (u32 r : roots)
            if (nodes[r].name == name)
                return &nodes[r];
        return nullptr;
    }
    const Node* Child(const Node* n, StringView name) const
    {
        if (n)
            for (u32 c : n->children)
                if (nodes[c].name == name)
                    return &nodes[c];
        return nullptr;
    }
    const Property* Prop(const Node* n, u32 i) const { return n && i < n->propCount ? &props[n->firstProp + i] : nullptr; }
    StringView PropStr(const Node* n, u32 i) const
    {
        const Property* p = Prop(n, i);
        return p ? p->Str() : StringView();
    }
    /// Child `name`'s first property as a string ("MappingInformationType" and the like).
    StringView ChildStr(const Node* n, StringView name) const { return PropStr(Child(n, name), 0); }

    /// Numeric array (or scalar) property converted to T; decompresses zlib arrays.
    template <typename T>
    bool ReadArray(const Property* p, Array<T>& out) const
    {
        out.Clear();
        if (!p)
            return false;
        u32 elem = 0;
        switch (p->type) {
        case 'f': case 'i': elem = 4; break;
        case 'd': case 'l': elem = 8; break;
        case 'b': elem = 1; break;
        default:
            out.PushBack(T(p->Number()));
            return true;
        }
        const usize bytes = usize(p->count) * elem;
        Array<u8> inflated;
        const u8* src = p->data.Data();
        if (p->encoding == 1) {
            if (bytes > p->data.Size() * 1040 + 1024) // more than DEFLATE can expand to: corrupt count
                return false;
            if (!compression::ZlibDecompress(p->data, inflated, bytes) || inflated.Size() < bytes)
                return false;
            src = inflated.Data();
        } else if (p->encoding != 0 || p->data.Size() < bytes) {
            return false;
        }
        out.ResizeUninitialized(p->count);
        for (u32 k = 0; k < p->count; ++k) {
            const u8* e = src + usize(k) * elem;
            switch (p->type) {
            case 'f': out[k] = T(ReadLE<f32>(e)); break;
            case 'd': out[k] = T(ReadLE<f64>(e)); break;
            case 'i': out[k] = T(ReadLE<i32>(e)); break;
            case 'l': out[k] = T(ReadLE<i64>(e)); break;
            default: out[k] = T(e[0] != 0); break;
            }
        }
        return true;
    }
    template <typename T>
    bool ReadChildArray(const Node* n, StringView name, Array<T>& out) const
    {
        const Node* c = Child(n, name);
        return c && ReadArray(Prop(c, 0), out);
    }

    u32 version = 0;
    Array<Node> nodes;
    Array<Property> props;
    Array<u32> roots;

private:
    Result<void> ParseNode(usize& pos, usize parentEnd, u32 depth, i64& outIndex)
    {
        outIndex = -1;
        const bool wide = version >= 7500;
        const usize header = wide ? 25 : 13;
        if (pos + header > m_bytes.Size()) {
            if (depth == 0)
                return {}; // footer / padding after the last top-level node
            return Error::Make(ErrorCode::InvalidData, "FBX: truncated node");
        }
        const u8* p = m_bytes.Data() + pos;
        const u64 end = wide ? ReadLE<u64>(p) : ReadLE<u32>(p);
        const u64 numProps = wide ? ReadLE<u64>(p + 8) : ReadLE<u32>(p + 4);
        const u64 propLength = wide ? ReadLE<u64>(p + 16) : ReadLE<u32>(p + 8);
        const u8 nameLength = p[header - 1];
        pos += header;
        if (end == 0)
            return {}; // null record: end of a child list (or of the top level)
        if (depth > MaxDepth || end > parentEnd || end < pos + nameLength + propLength)
            return Error::Make(ErrorCode::InvalidData, "FBX: corrupt node record at {}", pos - header);
        Node node;
        node.name = StringView(reinterpret_cast<const char*>(m_bytes.Data() + pos), nameLength);
        pos += nameLength;
        const usize propEnd = pos + usize(propLength);
        node.firstProp = u32(props.Size());
        for (u64 i = 0; i < numProps; ++i)
            ZX_TRY(ParseProperty(pos, propEnd));
        node.propCount = u32(props.Size()) - node.firstProp;
        pos = propEnd;
        const u32 index = u32(nodes.Size());
        nodes.PushBack(move(node));
        while (pos < end) {
            i64 child = -1;
            ZX_TRY(ParseNode(pos, usize(end), depth + 1, child));
            if (child < 0)
                break;
            nodes[index].children.PushBack(u32(child));
        }
        pos = usize(end);
        outIndex = index;
        return {};
    }

    Result<void> ParseProperty(usize& pos, usize end)
    {
        if (pos >= end)
            return Error::Make(ErrorCode::InvalidData, "FBX: truncated property list");
        Property prop;
        prop.type = char(m_bytes[pos++]);
        const u8* p = m_bytes.Data() + pos;
        auto need = [&](usize n) { return pos + n <= end; };
        switch (prop.type) {
        case 'Y': if (!need(2)) break; prop.i = ReadLE<i16>(p); pos += 2; props.PushBack(prop); return {};
        case 'C': if (!need(1)) break; prop.i = p[0]; pos += 1; props.PushBack(prop); return {};
        case 'I': if (!need(4)) break; prop.i = ReadLE<i32>(p); pos += 4; props.PushBack(prop); return {};
        case 'L': if (!need(8)) break; prop.i = ReadLE<i64>(p); pos += 8; props.PushBack(prop); return {};
        case 'F': if (!need(4)) break; prop.d = ReadLE<f32>(p); pos += 4; props.PushBack(prop); return {};
        case 'D': if (!need(8)) break; prop.d = ReadLE<f64>(p); pos += 8; props.PushBack(prop); return {};
        case 'S':
        case 'R': {
            if (!need(4))
                break;
            const u32 length = ReadLE<u32>(p);
            if (!need(4 + usize(length)))
                break;
            prop.data = ByteSpan(p + 4, length);
            pos += 4 + usize(length);
            props.PushBack(prop);
            return {};
        }
        case 'f': case 'd': case 'l': case 'i': case 'b': {
            if (!need(12))
                break;
            prop.count = ReadLE<u32>(p);
            prop.encoding = ReadLE<u32>(p + 4);
            const u32 length = ReadLE<u32>(p + 8);
            if (!need(12 + usize(length)))
                break;
            prop.data = ByteSpan(p + 12, length);
            pos += 12 + usize(length);
            props.PushBack(prop);
            return {};
        }
        default:
            return Error::Make(ErrorCode::InvalidData, "FBX: unknown property type '{}'", prop.type);
        }
        return Error::Make(ErrorCode::InvalidData, "FBX: truncated property");
    }

    ByteSpan m_bytes;
};

// --- Double-precision math (FBX files mix 100x scales with centimeters) ------------------------------------------

struct DVec3
{
    f64 x = 0, y = 0, z = 0;
};

/// Column-major 4x4, column vectors (like Mat4).
struct DMat
{
    f64 m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}; // m[column][row]

    static DMat Translation(DVec3 t)
    {
        DMat r;
        r.m[3][0] = t.x;
        r.m[3][1] = t.y;
        r.m[3][2] = t.z;
        return r;
    }
    static DMat Scale(DVec3 s)
    {
        DMat r;
        r.m[0][0] = s.x;
        r.m[1][1] = s.y;
        r.m[2][2] = s.z;
        return r;
    }
    /// Right-handed rotation about one axis (0 = X, 1 = Y, 2 = Z), degrees.
    static DMat Rotation(u32 axis, f64 degrees)
    {
        const f64 a = degrees * 3.14159265358979323846 / 180.0, c = std::cos(a), s = std::sin(a);
        DMat r;
        const u32 i = (axis + 1) % 3, j = (axis + 2) % 3;
        r.m[i][i] = c;
        r.m[i][j] = s;
        r.m[j][i] = -s;
        r.m[j][j] = c;
        return r;
    }
    /// FBX Euler rotation: order 0 = XYZ (X applied first) ... 5 = ZYX; 6 (spheric) is treated as XYZ.
    static DMat Euler(DVec3 deg, i64 order)
    {
        static constexpr u8 Orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 2, 0}, {1, 0, 2}, {2, 0, 1}, {2, 1, 0}};
        const u8* o = Orders[order >= 0 && order < 6 ? order : 0];
        const f64 v[3] = {deg.x, deg.y, deg.z};
        DMat r;
        for (u32 k = 0; k < 3; ++k)
            if (v[o[k]] != 0.0)
                r = Rotation(o[k], v[o[k]]) * r;
        return r;
    }

    DMat operator*(const DMat& o) const
    {
        DMat r;
        for (u32 c = 0; c < 4; ++c)
            for (u32 row = 0; row < 4; ++row)
                r.m[c][row] = m[0][row] * o.m[c][0] + m[1][row] * o.m[c][1] + m[2][row] * o.m[c][2] + m[3][row] * o.m[c][3];
        return r;
    }

    DMat Inverse() const
    {
        // Gauss-Jordan with partial pivoting on a row-major copy.
        f64 a[4][8];
        for (u32 r = 0; r < 4; ++r)
            for (u32 c = 0; c < 4; ++c) {
                a[r][c] = m[c][r];
                a[r][c + 4] = r == c ? 1.0 : 0.0;
            }
        for (u32 c = 0; c < 4; ++c) {
            u32 pivot = c;
            for (u32 r = c + 1; r < 4; ++r)
                if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
                    pivot = r;
            if (std::fabs(a[pivot][c]) < 1e-300)
                return DMat();
            if (pivot != c)
                for (u32 k = 0; k < 8; ++k)
                    std::swap(a[c][k], a[pivot][k]);
            const f64 inv = 1.0 / a[c][c];
            for (u32 k = 0; k < 8; ++k)
                a[c][k] *= inv;
            for (u32 r = 0; r < 4; ++r)
                if (r != c && a[r][c] != 0.0) {
                    const f64 f = a[r][c];
                    for (u32 k = 0; k < 8; ++k)
                        a[r][k] -= f * a[c][k];
                }
        }
        DMat r;
        for (u32 row = 0; row < 4; ++row)
            for (u32 c = 0; c < 4; ++c)
                r.m[c][row] = a[row][c + 4];
        return r;
    }

    Mat4 ToMat4() const
    {
        Mat4 r;
        for (u32 c = 0; c < 4; ++c)
            r.columns[c] = Vec4(f32(m[c][0]), f32(m[c][1]), f32(m[c][2]), f32(m[c][3]));
        return r;
    }
    static DMat FromArray(Span<const f64> v) // 16 values, column-major (FBX matrices)
    {
        DMat r;
        if (v.Size() >= 16)
            for (u32 c = 0; c < 4; ++c)
                for (u32 row = 0; row < 4; ++row)
                    r.m[c][row] = v[c * 4 + row];
        return r;
    }
};

// --- Scene graph -------------------------------------------------------------------------------------------------

struct Link
{
    u32 object;
    StringView property; // OP connections
};

struct Object
{
    i64 id = 0;
    const Node* node = nullptr;
    StringView kind;     // node name: Model, Geometry, Material, Deformer, ...
    StringView name;     // "Name" of "Name\0\x01Class"
    StringView subclass; // Mesh, LimbNode, Skin, Cluster, ...
    Array<Link> children;
    Array<Link> parents;
};

struct Curve
{
    Array<f64> times; // seconds
    Array<f32> values;
    Array<u8> constant; // per key: hold until the next key

    f64 Evaluate(f64 t) const
    {
        if (values.Empty())
            return 0.0;
        if (t <= times[0] || values.Size() == 1)
            return values[0];
        if (t >= times.Back())
            return values.Back();
        usize lo = 0, hi = times.Size() - 1;
        while (hi - lo > 1) {
            const usize mid = (lo + hi) / 2;
            (times[mid] <= t ? lo : hi) = mid;
        }
        if (constant[lo])
            return values[lo];
        const f64 span = times[hi] - times[lo];
        const f64 a = span > 0.0 ? (t - times[lo]) / span : 0.0;
        return f64(values[lo]) + (f64(values[hi]) - f64(values[lo])) * a;
    }
};

/// The animated channels of one model in one stack: curves per component of T / R / S (nullptr = static value).
struct ModelChannels
{
    u32 model = 0;
    const Curve* curves[3][3] = {}; // [T/R/S][x/y/z]
    DVec3 defaults[3];              // curve node defaults
    bool has[3] = {};
};

struct Skin
{
    u32 geometry = 0;
    struct ClusterData
    {
        u32 bone = 0; // object index of the bone model
        Array<i32> indices;
        Array<f64> weights;
        DMat transform; // mesh global at bind time (engine space)
        DMat transformLink; // bone global at bind time
        bool hasLink = false;
    };
    Array<ClusterData> clusters;
};

class FbxParser
{
public:
    FbxParser(const Document& doc, StringView baseDir, StringView sourceName, const ImportOptions& options)
        : m_doc(doc), m_baseDir(baseDir), m_sourceName(sourceName), m_options(options)
    {
    }

    Result<ImportedScene> Run()
    {
        ReadGlobalSettings();
        ReadObjects();
        ReadConnections();
        ImportNodes();
        ImportMaterials();
        ImportSkeletons();
        ZX_TRY(ImportMeshes());
        ImportAnimations();
        if (m_scene.meshes.Empty() && m_scene.skeletons.Empty())
            return Error::Make(ErrorCode::InvalidData, "FBX: '{}' contains no meshes", m_sourceName);
        return move(m_scene);
    }

    /// Resolved files of the external (not embedded) textures.
    Array<String> TextureFiles()
    {
        ReadObjects();
        ReadConnections();
        Array<String> files;
        for (u32 i = 0; i < m_objects.Size(); ++i)
            if (m_objects[i].kind == "Texture" && ImportTexture(i, true, false) >= 0) {
                const ImportedTexture& t = m_scene.textures.Back();
                if (!t.path.Empty() && fs::Exists(t.path) && files.Find(t.path) == NPos)
                    files.PushBack(t.path);
            }
        return files;
    }

private:
    // --- Properties70 ------------------------------------------------------------------------------------------

    const Node* FindP(const Node* objectNode, StringView name) const
    {
        const Node* table = m_doc.Child(objectNode, "Properties70");
        if (!table)
            table = m_doc.Child(objectNode, "Properties60");
        if (table)
            for (u32 c : table->children) {
                const Node& p = m_doc.nodes[c];
                if (p.propCount > 0 && m_doc.props[p.firstProp].Str() == name)
                    return &p;
            }
        return nullptr;
    }
    /// Value `index` of P record `name` (values start after name, type, label, flags).
    f64 PNumber(const Node* objectNode, StringView name, f64 fallback, u32 index = 0) const
    {
        const Node* p = FindP(objectNode, name);
        const Property* v = p ? m_doc.Prop(p, 4 + index) : nullptr;
        return v && v->type != 'S' ? v->Number() : fallback;
    }
    DVec3 PVec(const Node* objectNode, StringView name, DVec3 fallback) const
    {
        const Node* p = FindP(objectNode, name);
        if (!p || p->propCount < 7)
            return fallback;
        return DVec3{m_doc.props[p->firstProp + 4].Number(), m_doc.props[p->firstProp + 5].Number(), m_doc.props[p->firstProp + 6].Number()};
    }
    StringView PString(const Node* objectNode, StringView name) const
    {
        const Node* p = FindP(objectNode, name);
        return p ? m_doc.PropStr(p, 4) : StringView();
    }

    // --- Global settings: axis system and units ------------------------------------------------------------------

    void ReadGlobalSettings()
    {
        const Node* gs = m_doc.Root("GlobalSettings");
        const i64 up = i64(PNumber(gs, "UpAxis", 1)), upSign = i64(PNumber(gs, "UpAxisSign", 1));
        const i64 front = i64(PNumber(gs, "FrontAxis", 2)), frontSign = i64(PNumber(gs, "FrontAxisSign", 1));
        const i64 coord = i64(PNumber(gs, "CoordAxis", 0)), coordSign = i64(PNumber(gs, "CoordAxisSign", 1));
        const f64 unit = PNumber(gs, "UnitScaleFactor", 1.0); // centimeters per file unit
        m_frameRate = FrameRate(i64(PNumber(gs, "TimeMode", 0)), PNumber(gs, "CustomFrameRate", 30.0));
        // Rows of the axis change: x' = coord axis, y' = up axis, z' = front axis (right-handed, Y-up, +Z front as glTF).
        DMat axes = DMat::Scale(DVec3{0, 0, 0});
        auto setRow = [&](u32 row, i64 axis, i64 sign) {
            if (axis < 0 || axis > 2)
                return false;
            axes.m[axis][row] = sign < 0 ? -1.0 : 1.0;
            return true;
        };
        if (up == front || up == coord || front == coord || !setRow(0, coord, coordSign) || !setRow(1, up, upSign) || !setRow(2, front, frontSign))
            axes = DMat(); // inconsistent settings: keep the data as is
        axes.m[3][3] = 1.0;
        const f64 meters = (unit > 0.0 ? unit : 1.0) / 100.0 * f64(m_options.scale);
        // Right-handed -> engine (left-handed): mirror X, as for glTF.
        m_convert = DMat::Scale(DVec3{-meters, meters, meters}) * axes;
        m_convertInverse = m_convert.Inverse();
        const Node* creator = m_doc.Root("Creator");
        m_blender = m_doc.PropStr(creator, 0).StartsWith("Blender");
    }

    static f64 FrameRate(i64 mode, f64 custom)
    {
        // FbxTime::EMode: default, 120, 100, 60, 50, 48, 30, 30 drop, NTSC drop, NTSC full, PAL, 24, 1000, film, custom, ...
        static constexpr f64 Rates[] = {0, 120, 100, 60, 50, 48, 30, 30, 29.97, 29.97, 25, 24, 1000, 23.976, 0, 96, 72, 59.94, 119.88};
        if (mode == 14)
            return custom > 0.0 ? custom : 30.0;
        return mode > 0 && mode < i64(sizeof(Rates) / sizeof(Rates[0])) && Rates[mode] > 0.0 ? Rates[mode] : 30.0;
    }

    /// File-space matrix -> engine space (keeps the meaning "child space -> parent space" across the change of basis).
    DMat Convert(const DMat& m) const { return m_convert * m * m_convertInverse; }

    // --- Objects and connections ---------------------------------------------------------------------------------

    void ReadObjects()
    {
        const Node* objects = m_doc.Root("Objects");
        if (!objects)
            return;
        for (u32 c : objects->children) {
            const Node& n = m_doc.nodes[c];
            const Property* id = m_doc.Prop(&n, 0);
            if (!id || (id->type != 'L' && id->type != 'I'))
                continue;
            Object& o = m_objects.AddDefaulted();
            o.id = id->i;
            o.node = &n;
            o.kind = n.name;
            StringView full = m_doc.PropStr(&n, 1);
            const usize sep = full.Find(StringView("\0\x01", 2));
            o.name = sep != StringView::NPos ? full.Left(sep) : full;
            if (o.name.Contains("::")) // FBX 6 style "Model::Name"
                o.name = o.name.SubStr(o.name.Find("::") + 2);
            o.subclass = m_doc.PropStr(&n, 2);
            m_byId.InsertOrAssign(o.id, u32(m_objects.Size() - 1));
        }
    }

    void ReadConnections()
    {
        const Node* connections = m_doc.Root("Connections");
        if (!connections)
            return;
        for (u32 c : connections->children) {
            const Node& n = m_doc.nodes[c];
            if (n.name != "C" || n.propCount < 3)
                continue;
            const u32* child = m_byId.Find(m_doc.props[n.firstProp + 1].i);
            const u32* parent = m_byId.Find(m_doc.props[n.firstProp + 2].i);
            if (!child || !parent)
                continue; // parent 0 = scene root
            const StringView property = n.propCount > 3 ? m_doc.props[n.firstProp + 3].Str() : StringView();
            m_objects[*parent].children.PushBack(Link{*child, property});
            m_objects[*child].parents.PushBack(Link{*parent, property});
        }
    }

    i32 ParentOfKind(u32 object, StringView kind) const
    {
        for (const Link& l : m_objects[object].parents)
            if (m_objects[l.object].kind == kind)
                return i32(l.object);
        return -1;
    }
    i32 ChildOfKind(u32 object, StringView kind, StringView subclass = {}) const
    {
        for (const Link& l : m_objects[object].children)
            if (m_objects[l.object].kind == kind && (subclass.Empty() || m_objects[l.object].subclass == subclass))
                return i32(l.object);
        return -1;
    }

    // --- Nodes ----------------------------------------------------------------------------------------------------

    /// FBX node local matrix (file space): T * Roff * Rp * Rpre * R * Rpost^-1 * Rp^-1 * Soff * Sp * S * Sp^-1.
    DMat LocalMatrix(const Node* n, const DVec3* t = nullptr, const DVec3* r = nullptr, const DVec3* s = nullptr) const
    {
        const DVec3 zero{}, one{1, 1, 1};
        const DVec3 translation = t ? *t : PVec(n, "Lcl Translation", zero);
        const DVec3 rotation = r ? *r : PVec(n, "Lcl Rotation", zero);
        const DVec3 scaling = s ? *s : PVec(n, "Lcl Scaling", one);
        const i64 order = i64(PNumber(n, "RotationOrder", 0));
        const DVec3 rpv = PVec(n, "RotationPivot", zero), spv = PVec(n, "ScalingPivot", zero);
        const DMat rp = DMat::Translation(rpv), sp = DMat::Translation(spv);
        const DMat rpInv = DMat::Translation(DVec3{-rpv.x, -rpv.y, -rpv.z});
        const DMat spInv = DMat::Translation(DVec3{-spv.x, -spv.y, -spv.z});
        const DMat pre = DMat::Euler(PVec(n, "PreRotation", zero), 0);
        const DMat post = DMat::Euler(PVec(n, "PostRotation", zero), 0).Inverse();
        return DMat::Translation(translation) * DMat::Translation(PVec(n, "RotationOffset", zero)) * rp * pre *
               DMat::Euler(rotation, order) * post * rpInv * DMat::Translation(PVec(n, "ScalingOffset", zero)) * sp *
               DMat::Scale(scaling) * spInv;
    }

    DMat GeometricMatrix(const Node* n) const
    {
        const DVec3 zero{}, one{1, 1, 1};
        return DMat::Translation(PVec(n, "GeometricTranslation", zero)) * DMat::Euler(PVec(n, "GeometricRotation", zero), 0) *
               DMat::Scale(PVec(n, "GeometricScaling", one));
    }

    void ImportNodes()
    {
        m_nodeOf.Resize(m_objects.Size(), -1);
        for (u32 i = 0; i < m_objects.Size(); ++i)
            if (m_objects[i].kind == "Model") {
                m_nodeOf[i] = i32(m_scene.nodes.Size());
                m_modelOf.PushBack(i);
                ImportedNode& node = m_scene.nodes.AddDefaulted();
                node.name = String(m_objects[i].name);
            }
        m_nodeLocal.Resize(m_scene.nodes.Size());
        for (u32 n = 0; n < m_modelOf.Size(); ++n) {
            const u32 model = m_modelOf[n];
            ImportedNode& node = m_scene.nodes[n];
            m_nodeLocal[n] = Convert(LocalMatrix(m_objects[model].node));
            node.local = Transform::FromMatrix(m_nodeLocal[n].ToMat4());
            const i32 parent = ParentOfKind(model, "Model");
            node.parent = parent >= 0 ? m_nodeOf[usize(parent)] : -1;
        }
        for (u32 n = 0; n < m_scene.nodes.Size(); ++n) {
            if (m_scene.nodes[n].parent >= 0)
                m_scene.nodes[usize(m_scene.nodes[n].parent)].children.PushBack(i32(n));
            else
                m_scene.rootNodes.PushBack(i32(n));
        }
    }

    DMat NodeGlobal(i32 node) const
    {
        DMat m;
        for (i32 n = node; n >= 0; n = m_scene.nodes[usize(n)].parent)
            m = m_nodeLocal[usize(n)] * m;
        return m;
    }

    // --- Materials and textures ---------------------------------------------------------------------------------

    String ResolveTextureFile(StringView fileName) const
    {
        String file(fileName);
        file.ReplaceChar('\\', '/');
        if (file.Empty())
            return {};
        const bool absolute = file.Size() > 1 && (file[1] == ':' || file[0] == '/');
        if (!absolute && fs::Exists(Path::Join(m_baseDir, file)))
            return Path::Normalize(Path::Join(m_baseDir, file));
        if (absolute && fs::Exists(file))
            return file;
        // Moved files: look for the name next to the model and in the usual texture folders.
        const StringView name = Path::FileName(file);
        const String stem(Path::Stem(m_sourceName));
        const String parent(Path::Parent(m_baseDir));
        const String dirs[] = {String(m_baseDir), Path::Join(m_baseDir, "Textures"), Path::Join(m_baseDir, "textures"),
                               Path::Join(m_baseDir, stem + ".fbm"), Path::Join(parent, "Textures"), Path::Join(parent, "textures"),
                               parent};
        for (const String& d : dirs)
            if (!d.Empty() && fs::Exists(Path::Join(d, name)))
                return Path::Normalize(Path::Join(d, name));
        return Path::Join(m_baseDir, name); // missing: reported when the texture loads
    }

    static String MimeOf(StringView file)
    {
        if (Path::HasExtension(file, "jpg") || Path::HasExtension(file, "jpeg"))
            return "image/jpeg";
        if (Path::HasExtension(file, "tga"))
            return "image/x-tga";
        if (Path::HasExtension(file, "bmp"))
            return "image/bmp";
        return "image/png";
    }

    i32 ImportTexture(u32 textureObject, bool srgb, bool normalMap)
    {
        if (const i32* existing = m_textureOf.Find(textureObject)) {
            m_scene.textures[usize(*existing)].srgb = srgb;
            m_scene.textures[usize(*existing)].normalMap |= normalMap;
            return *existing;
        }
        const Object& tex = m_objects[textureObject];
        ImportedTexture t;
        t.name = String(tex.name);
        t.srgb = srgb;
        t.normalMap = normalMap;
        StringView fileName = m_doc.ChildStr(tex.node, "RelativeFilename");
        if (fileName.Empty())
            fileName = m_doc.ChildStr(tex.node, "FileName");
        // Embedded image: the Video's Content.
        if (const i32 video = ChildOfKind(textureObject, "Video"); video >= 0) {
            const Node* vn = m_objects[usize(video)].node;
            const Property* content = m_doc.Prop(m_doc.Child(vn, "Content"), 0);
            StringView videoFile = m_doc.ChildStr(vn, "RelativeFilename");
            if (videoFile.Empty())
                videoFile = m_doc.ChildStr(vn, "Filename");
            if (content && content->type == 'R' && content->data.Size() > 0) {
                t.data = Array<u8>(content->data);
                t.mimeType = MimeOf(videoFile.Empty() ? fileName : videoFile);
            } else if (fileName.Empty()) {
                fileName = videoFile;
            }
        }
        if (t.data.Empty()) {
            t.path = ResolveTextureFile(fileName);
            if (t.path.Empty())
                return -1;
        }
        const i32 index = i32(m_scene.textures.Size());
        m_scene.textures.PushBack(move(t));
        m_textureOf.InsertOrAssign(textureObject, index);
        return index;
    }

    /// Texture connected to `material` through `property` (directly, or the first texture of a LayeredTexture).
    i32 MaterialTexture(u32 material, StringView property) const
    {
        for (const Link& l : m_objects[material].children) {
            if (l.property != property)
                continue;
            const Object& o = m_objects[l.object];
            if (o.kind == "Texture")
                return i32(l.object);
            if (o.kind == "LayeredTexture")
                if (const i32 t = ChildOfKind(l.object, "Texture"); t >= 0)
                    return t;
        }
        return -1;
    }

    void ImportMaterials()
    {
        m_materialOf.Resize(m_objects.Size(), -1);
        for (u32 i = 0; i < m_objects.Size(); ++i) {
            if (m_objects[i].kind != "Material")
                continue;
            const Node* n = m_objects[i].node;
            ImportedMaterial m;
            m.name = String(m_objects[i].name);
            const DVec3 diffuse = PVec(n, "DiffuseColor", PVec(n, "Diffuse", DVec3{0.8, 0.8, 0.8}));
            const f64 opacity = PNumber(n, "Opacity", 1.0);
            m.baseColor = Vec4(f32(diffuse.x), f32(diffuse.y), f32(diffuse.z), f32(Clamp(opacity, 0.0, 1.0)));
            const DVec3 emissive = PVec(n, "EmissiveColor", DVec3{});
            const f64 emissiveFactor = PNumber(n, "EmissiveFactor", 1.0);
            m.emissive = Vec3(f32(emissive.x * emissiveFactor), f32(emissive.y * emissiveFactor), f32(emissive.z * emissiveFactor));
            const f64 shininess = PNumber(n, "ShininessExponent", PNumber(n, "Shininess", 20.0));
            if (m_blender) {
                // Blender writes shininess = ((1 - roughness) * 10)^2 and metallic as ReflectionFactor.
                m.roughness = f32(Clamp(1.0 - std::sqrt(Max(shininess, 0.0)) / 10.0, 0.0, 1.0));
                m.metallic = f32(Clamp(PNumber(n, "ReflectionFactor", 0.0), 0.0, 1.0));
            } else {
                m.roughness = f32(Clamp(std::sqrt(2.0 / (Max(shininess, 0.0) + 2.0)), 0.05, 1.0)); // Blinn-Phong exponent
                m.metallic = 0.0f;
            }
            if (const i32 t = MaterialTexture(i, "DiffuseColor"); t >= 0)
                m.baseColorTexture.texture = ImportTexture(u32(t), true, false);
            i32 normal = MaterialTexture(i, "NormalMap");
            if (normal < 0) // bump slots often hold normal maps
                if (const i32 bump = MaterialTexture(i, "Bump"); bump >= 0 && String(m_objects[usize(bump)].name).ToLower().View().Contains("normal"))
                    normal = bump;
            if (normal >= 0)
                m.normalTexture.texture = ImportTexture(u32(normal), false, true);
            if (const i32 t = MaterialTexture(i, "EmissiveColor"); t >= 0) {
                m.emissiveTexture.texture = ImportTexture(u32(t), true, false);
                if (emissive.x == 0.0 && emissive.y == 0.0 && emissive.z == 0.0)
                    m.emissive = Vec3(1.0f, 1.0f, 1.0f);
            }
            if (MaterialTexture(i, "TransparentColor") >= 0 || MaterialTexture(i, "TransparencyFactor") >= 0) {
                m.alphaMode = AlphaMode::Mask; // alpha comes from the base color texture
                m.doubleSided = true;
            } else if (m.baseColor.w < 0.999f) {
                m.alphaMode = AlphaMode::Blend;
            }
            m_materialOf[i] = i32(m_scene.materials.Size());
            m_scene.materials.PushBack(move(m));
        }
    }

    u32 DefaultMaterial()
    {
        if (m_defaultMaterial < 0) {
            m_defaultMaterial = i32(m_scene.materials.Size());
            ImportedMaterial& def = m_scene.materials.AddDefaulted();
            def.name = "Default";
            def.metallic = 0.0f;
            def.roughness = 0.5f;
        }
        return u32(m_defaultMaterial);
    }

    // --- Skeletons ----------------------------------------------------------------------------------------------

    bool IsLimb(u32 model) const
    {
        const StringView s = m_objects[model].subclass;
        return s == "LimbNode" || s == "Root" || s == "Limb";
    }

    void ImportSkeletons()
    {
        // Skins and their clusters.
        for (u32 i = 0; i < m_objects.Size(); ++i) {
            if (m_objects[i].kind != "Deformer" || m_objects[i].subclass != "Skin")
                continue;
            const i32 geometry = ParentOfKind(i, "Geometry");
            if (geometry < 0)
                continue;
            Skin skin;
            skin.geometry = u32(geometry);
            for (const Link& l : m_objects[i].children) {
                const Object& c = m_objects[l.object];
                if (c.kind != "Deformer" || c.subclass != "Cluster")
                    continue;
                const i32 bone = ChildOfKind(l.object, "Model");
                if (bone < 0)
                    continue;
                Skin::ClusterData& cd = skin.clusters.AddDefaulted();
                cd.bone = u32(bone);
                m_doc.ReadChildArray(c.node, "Indexes", cd.indices);
                m_doc.ReadChildArray(c.node, "Weights", cd.weights);
                // Stored Transform = TransformLink^-1 * mesh global at bind time (what the SDK's GetTransformMatrix
                // multiplies back); TransformLink = bone global at bind time.
                Array<f64> m;
                if (m_doc.ReadChildArray(c.node, "TransformLink", m) && m.Size() >= 16) {
                    cd.transformLink = Convert(DMat::FromArray(m.View()));
                    cd.hasLink = true;
                }
                if (m_doc.ReadChildArray(c.node, "Transform", m) && m.Size() >= 16)
                    cd.transform = cd.transformLink * Convert(DMat::FromArray(m.View()));
            }
            if (!skin.clusters.Empty())
                m_skins.PushBack(move(skin));
        }
        // One skeleton per armature: the top-most limb above the bound bones; every limb below it is a bone.
        m_skeletonOfRoot.Clear();
        m_boneOf.Resize(m_objects.Size(), BoneRef{-1, -1});
        for (const Skin& skin : m_skins)
            for (const Skin::ClusterData& cd : skin.clusters) {
                u32 root = cd.bone;
                for (i32 p = ParentOfKind(root, "Model"); p >= 0 && IsLimb(u32(p)); p = ParentOfKind(u32(p), "Model"))
                    root = u32(p);
                if (!m_skeletonOfRoot.Contains(root))
                    BuildSkeleton(root);
            }
        // Inverse bind matrices from the clusters (bone global at bind time); unbound bones use their rest pose.
        for (usize s = 0; s < m_scene.skeletons.Size(); ++s) {
            Skeleton& sk = m_scene.skeletons[s];
            SkeletonInfo& info = m_skeletonInfo[s];
            for (const Skin& skin : m_skins) {
                for (const Skin::ClusterData& cd : skin.clusters)
                    if (m_boneOf[cd.bone].skeleton == i32(s) && cd.hasLink && !info.bound[usize(m_boneOf[cd.bone].bone)]) {
                        info.bindGlobal[usize(m_boneOf[cd.bone].bone)] = cd.transformLink;
                        info.bound[usize(m_boneOf[cd.bone].bone)] = 1;
                    }
            }
            sk.inverseBind.Resize(sk.BoneCount());
            for (u32 b = 0; b < sk.BoneCount(); ++b)
                info.inverseBindSource[b] = info.bindGlobal[b].Inverse();
        }
    }

    void BuildSkeleton(u32 root)
    {
        const i32 s = i32(m_scene.skeletons.Size());
        m_skeletonOfRoot.InsertOrAssign(root, s);
        Skeleton& sk = m_scene.skeletons.AddDefaulted();
        SkeletonInfo& info = m_skeletonInfo.AddDefaulted();
        sk.name = String(m_objects[root].name);
        info.rootParentNode = ParentOfKind(root, "Model") >= 0 ? m_nodeOf[usize(ParentOfKind(root, "Model"))] : -1;
        // Depth-first: parents precede children.
        Array<Pair<u32, i32>> stack; // (model, parent bone)
        stack.PushBack({root, -1});
        while (!stack.Empty()) {
            const Pair<u32, i32> item = stack.PopBackValue();
            const u32 model = item.first;
            if (m_boneOf[model].skeleton >= 0)
                continue;
            const i32 bone = i32(sk.BoneCount());
            m_boneOf[model] = BoneRef{s, bone};
            const i32 node = m_nodeOf[model];
            sk.boneNames.PushBack(String(m_objects[model].name));
            sk.parents.PushBack(item.second);
            sk.bindPose.PushBack(m_scene.nodes[usize(node)].local);
            info.boneModels.PushBack(model);
            info.bindGlobal.PushBack(NodeGlobal(node));
            info.bound.PushBack(0);
            info.inverseBindSource.AddDefaulted();
            m_scene.nodes[usize(node)].isBone = true;
            const Array<Link>& children = m_objects[model].children;
            for (usize c = children.Size(); c-- > 0;) {
                const u32 child = children[c].object;
                if (m_objects[child].kind == "Model" && (IsLimb(child) || IsBoundBone(child)))
                    stack.PushBack({child, bone});
            }
        }
    }

    bool IsBoundBone(u32 model) const
    {
        for (const Skin& skin : m_skins)
            for (const Skin::ClusterData& cd : skin.clusters)
                if (cd.bone == model)
                    return true;
        return false;
    }

    // --- Meshes -------------------------------------------------------------------------------------------------

    /// Index into a layer element's direct array for one polygon corner (-1: not available).
    struct LayerMapping
    {
        StringView mapping;
        bool indexed = false;
        Array<i32> index;

        i32 Element(u32 corner, u32 controlPoint, u32 polygon) const
        {
            u32 i;
            if (mapping == "ByPolygonVertex")
                i = corner;
            else if (mapping == "ByVertice" || mapping == "ByVertex" || mapping == "ByControlPoint")
                i = controlPoint;
            else if (mapping == "ByPolygon")
                i = polygon;
            else if (mapping == "AllSame")
                i = 0;
            else
                return -1;
            if (!indexed)
                return i32(i);
            return i < index.Size() ? index[i] : -1;
        }
    };

    LayerMapping ReadMapping(const Node* element, StringView indexName) const
    {
        LayerMapping m;
        m.mapping = m_doc.ChildStr(element, "MappingInformationType");
        const StringView ref = m_doc.ChildStr(element, "ReferenceInformationType");
        m.indexed = ref == "IndexToDirect" || ref == "Index";
        if (m.indexed && !m_doc.ReadChildArray(element, indexName, m.index))
            m.indexed = false;
        return m;
    }

    /// Layer elements named `name` ordered by their set index (UV0, UV1...).
    Array<const Node*> LayerElements(const Node* geometry, StringView name) const
    {
        Array<const Node*> out;
        for (u32 c : geometry->children)
            if (m_doc.nodes[c].name == name)
                out.PushBack(&m_doc.nodes[c]);
        StableSort(out, [&](const Node* a, const Node* b) {
            const Property* pa = m_doc.Prop(a, 0);
            const Property* pb = m_doc.Prop(b, 0);
            return (pa ? pa->i : 0) < (pb ? pb->i : 0);
        });
        return out;
    }

    /// Builds the mesh of one Geometry in file geometry space (counter-clockwise polygons, fan triangulated), one
    /// section per material slot; `slotMaterials` maps slots to scene materials.
    Result<MeshData> BuildMesh(u32 geometryObject, Span<const u32> slotMaterials, const Skin* skin, i32 skeleton)
    {
        const Node* g = m_objects[geometryObject].node;
        Array<f64> positions;
        Array<i32> polygonIndex;
        if (!m_doc.ReadChildArray(g, "Vertices", positions) || !m_doc.ReadChildArray(g, "PolygonVertexIndex", polygonIndex))
            return Error::Make(ErrorCode::InvalidData, "FBX: geometry '{}' has no vertices", m_objects[geometryObject].name);
        const u32 controlPoints = u32(positions.Size() / 3);

        Array<const Node*> normalElements = LayerElements(g, "LayerElementNormal");
        Array<const Node*> uvElements = LayerElements(g, "LayerElementUV");
        Array<const Node*> colorElements = LayerElements(g, "LayerElementColor");
        Array<const Node*> materialElements = LayerElements(g, "LayerElementMaterial");
        Array<f64> normals, uv0, uv1, colors;
        LayerMapping normalMap, uv0Map, uv1Map, colorMap, materialMap;
        Array<i32> materialSlots;
        const bool hasNormals = !normalElements.Empty() && m_doc.ReadChildArray(normalElements[0], "Normals", normals);
        if (hasNormals)
            normalMap = ReadMapping(normalElements[0], "NormalsIndex");
        const bool hasUv0 = !uvElements.Empty() && m_doc.ReadChildArray(uvElements[0], "UV", uv0);
        if (hasUv0)
            uv0Map = ReadMapping(uvElements[0], "UVIndex");
        const bool hasUv1 = uvElements.Size() > 1 && m_doc.ReadChildArray(uvElements[1], "UV", uv1);
        if (hasUv1)
            uv1Map = ReadMapping(uvElements[1], "UVIndex");
        const bool hasColors = !colorElements.Empty() && m_doc.ReadChildArray(colorElements[0], "Colors", colors);
        if (hasColors)
            colorMap = ReadMapping(colorElements[0], "ColorIndex");
        if (!materialElements.Empty() && m_doc.ReadChildArray(materialElements[0], "Materials", materialSlots)) {
            materialMap.mapping = m_doc.ChildStr(materialElements[0], "MappingInformationType");
            materialMap.indexed = false; // the array already holds slot numbers
        }

        // Skin weights per control point: up to 4 strongest influences.
        struct Influence
        {
            u32 bone[4] = {};
            f32 weight[4] = {};
        };
        Array<Influence> influences;
        if (skin && skeleton >= 0) {
            influences.Resize(controlPoints);
            for (const Skin::ClusterData& cd : skin->clusters) {
                const BoneRef ref = m_boneOf[cd.bone];
                if (ref.skeleton != skeleton)
                    continue;
                for (usize k = 0; k < cd.indices.Size() && k < cd.weights.Size(); ++k) {
                    const i32 cp = cd.indices[k];
                    const f32 w = f32(cd.weights[k]);
                    if (cp < 0 || u32(cp) >= controlPoints || !(w > 0.0f))
                        continue;
                    Influence& inf = influences[usize(cp)];
                    u32 weakest = 0;
                    for (u32 j = 1; j < 4; ++j)
                        if (inf.weight[j] < inf.weight[weakest])
                            weakest = j;
                    if (w > inf.weight[weakest]) {
                        inf.weight[weakest] = w;
                        inf.bone[weakest] = u32(ref.bone);
                    }
                }
            }
        }

        // Corners grouped by material slot.
        struct Group
        {
            u32 slot;
            Array<MeshVertex> corners;
        };
        Array<Group> groups;
        auto groupOf = [&](u32 slot) -> Array<MeshVertex>& {
            for (Group& gr : groups)
                if (gr.slot == slot)
                    return gr.corners;
            groups.PushBack(Group{slot, {}});
            return groups.Back().corners;
        };
        auto vec3At = [](const Array<f64>& a, i32 e, u32 stride) {
            return e >= 0 && usize(e) * stride + 2 < a.Size() ? Vec3(f32(a[usize(e) * stride]), f32(a[usize(e) * stride + 1]), f32(a[usize(e) * stride + 2]))
                                                              : Vec3(0.0f, 0.0f, 0.0f);
        };
        auto uvAt = [](const Array<f64>& a, i32 e) {
            return e >= 0 && usize(e) * 2 + 1 < a.Size() ? Vec2(f32(a[usize(e) * 2]), 1.0f - f32(a[usize(e) * 2 + 1])) // origin bottom-left
                                                         : Vec2(0.0f, 0.0f);
        };
        SmallArray<MeshVertex, 16> polygon;
        u32 polygonNumber = 0;
        for (usize corner = 0, start = 0; corner < polygonIndex.Size(); ++corner) {
            const bool last = polygonIndex[corner] < 0;
            if (!last)
                continue;
            polygon.Clear();
            for (usize c = start; c <= corner; ++c) {
                const i32 raw = polygonIndex[c];
                const u32 cp = u32(raw < 0 ? ~raw : raw);
                if (cp >= controlPoints)
                    return Error::Make(ErrorCode::InvalidData, "FBX: geometry '{}' indexes a missing vertex", m_objects[geometryObject].name);
                MeshVertex v;
                v.position = Vec3(f32(positions[cp * 3]), f32(positions[cp * 3 + 1]), f32(positions[cp * 3 + 2]));
                if (hasNormals)
                    v.normal = SafeNormalize(vec3At(normals, normalMap.Element(u32(c), cp, polygonNumber), 3), Vec3::Up());
                if (hasUv0)
                    v.uv0 = uvAt(uv0, uv0Map.Element(u32(c), cp, polygonNumber));
                if (hasUv1)
                    v.uv1 = uvAt(uv1, uv1Map.Element(u32(c), cp, polygonNumber));
                if (hasColors) {
                    const i32 e = colorMap.Element(u32(c), cp, polygonNumber);
                    if (e >= 0 && usize(e) * 4 + 3 < colors.Size())
                        v.color = PackUnorm4x8(Vec4(f32(colors[usize(e) * 4]), f32(colors[usize(e) * 4 + 1]), f32(colors[usize(e) * 4 + 2]),
                                                    f32(colors[usize(e) * 4 + 3])));
                }
                if (!influences.Empty()) {
                    const Influence& inf = influences[cp];
                    anim::PackSkin(inf.bone, inf.weight, v.joints, v.weights);
                }
                polygon.PushBack(v);
            }
            u32 slot = 0;
            if (!materialSlots.Empty()) {
                const i32 s = materialMap.mapping == "AllSame" ? materialSlots[0]
                                                               : (polygonNumber < materialSlots.Size() ? materialSlots[polygonNumber] : 0);
                slot = s >= 0 ? u32(s) : 0u;
            }
            if (polygon.Size() >= 3) {
                Array<MeshVertex>& out = groupOf(slot);
                for (usize i = 1; i + 1 < polygon.Size(); ++i) // fan
                    out.Append({polygon[0], polygon[i], polygon[i + 1]});
            }
            start = corner + 1;
            ++polygonNumber;
        }

        MeshData mesh;
        mesh.name = String(m_objects[geometryObject].name);
        MeshLod& lod = mesh.lods.AddDefaulted();
        StableSort(groups, [](const Group& a, const Group& b) { return a.slot < b.slot; });
        for (Group& gr : groups) {
            MeshSection s;
            s.indexOffset = u32(lod.indices.Size());
            s.indexCount = u32(gr.corners.Size());
            s.materialIndex = gr.slot < slotMaterials.Size() ? slotMaterials[gr.slot] : DefaultMaterial();
            for (const MeshVertex& v : gr.corners) {
                lod.indices.PushBack(u32(mesh.vertices.Size()));
                mesh.vertices.PushBack(v);
            }
            lod.sections.PushBack(s);
        }
        mesh.attributes = (hasNormals ? MeshAttributes::Normals : MeshAttributes::None) | (hasUv0 ? MeshAttributes::UV0 : MeshAttributes::None) |
                          (hasUv1 ? MeshAttributes::UV1 : MeshAttributes::None) | (hasColors ? MeshAttributes::Colors : MeshAttributes::None) |
                          (!influences.Empty() ? MeshAttributes::Skin : MeshAttributes::None);
        return mesh;
    }

    Result<void> ImportMeshes()
    {
        for (u32 n = 0; n < m_modelOf.Size(); ++n) {
            const u32 model = m_modelOf[n];
            const i32 geometry = ChildOfKind(model, "Geometry", "Mesh");
            if (geometry < 0)
                continue;
            // Material slots = materials connected to the model, in connection order.
            Array<u32> slots;
            for (const Link& l : m_objects[model].children)
                if (m_materialOf[l.object] >= 0)
                    slots.PushBack(u32(m_materialOf[l.object]));
            const Skin* skin = nullptr;
            for (const Skin& s : m_skins)
                if (s.geometry == u32(geometry)) {
                    skin = &s;
                    break;
                }
            const i32 skeleton = skin ? m_boneOf[skin->clusters[0].bone].skeleton : -1;
            ZX_TRY_ASSIGN(mesh, BuildMesh(u32(geometry), slots.View(), skin, skeleton));
            ImportedNode& node = m_scene.nodes[n];
            // Vertices: geometric offset (not inherited), then file space -> engine space.
            DMat toNode = m_convert * GeometricMatrix(m_objects[model].node);
            if (skeleton >= 0)
                toNode = BindSkin(i32(n), *skin, u32(skeleton)) * toNode;
            mesh::Transform(mesh, toNode.ToMat4()); // the mirror flips the winding to clockwise
            mesh::WeldVertices(mesh);
            if ((mesh.attributes & MeshAttributes::Normals) == MeshAttributes::None && m_options.generateNormals) {
                mesh::GenerateNormals(mesh);
                mesh.attributes |= MeshAttributes::Normals;
            }
            if ((mesh.attributes & MeshAttributes::UV0) != MeshAttributes::None && m_options.generateTangents &&
                (mesh.attributes & MeshAttributes::Normals) != MeshAttributes::None) {
                mesh::GenerateTangents(mesh);
                mesh.attributes |= MeshAttributes::Tangents;
            }
            mesh.ComputeBounds();
            node.mesh = i32(m_scene.meshes.Size());
            m_scene.meshes.PushBack(move(mesh));
        }
        if (!m_scene.meshes.Empty() && m_scene.materials.Empty())
            DefaultMaterial();
        return {};
    }

    /// Skinned node: sets the skin and skinToNode, fills the skeleton's inverse binds on first use and returns the
    /// matrix taking engine-space bind-time vertices (converted geometry space) to their stored space.
    ///
    /// FBX skinning: world = boneGlobal * transformLink^-1 * transform * v. Engine: world = nodeGlobal * skinToNode *
    /// model * inverseBind * v' with model = rootParentGlobal^-1 * boneGlobal and skinToNode = nodeGlobal^-1 *
    /// rootParentGlobal. With inverseBind = transformLink^-1 * Y and v' = Y^-1 * transform * v both agree for any Y;
    /// Y = the first skinned node's global keeps that node's unskinned vertices in its own space.
    DMat BindSkin(i32 node, const Skin& skin, u32 skeleton)
    {
        SkeletonInfo& info = m_skeletonInfo[skeleton];
        Skeleton& sk = m_scene.skeletons[skeleton];
        const DMat nodeGlobal = NodeGlobal(node);
        if (!info.haveY) {
            info.y = nodeGlobal;
            info.haveY = true;
            for (u32 b = 0; b < sk.BoneCount(); ++b)
                sk.inverseBind[b] = (info.inverseBindSource[b] * info.y).ToMat4();
        }
        const DMat rootParentGlobal = NodeGlobal(info.rootParentNode);
        ImportedNode& n = m_scene.nodes[usize(node)];
        n.skin = i32(skeleton);
        n.skinToNode = (nodeGlobal.Inverse() * rootParentGlobal).ToMat4();
        return info.y.Inverse() * skin.clusters[0].transform;
    }

    // --- Animations ---------------------------------------------------------------------------------------------

    const Curve* LoadCurve(u32 curveObject)
    {
        if (const u32* existing = m_curveOf.Find(curveObject))
            return &m_curves[*existing];
        const Node* n = m_objects[curveObject].node;
        Curve c;
        Array<i64> times;
        Array<i32> flags, refCounts;
        m_doc.ReadChildArray(n, "KeyTime", times);
        m_doc.ReadChildArray(n, "KeyValueFloat", c.values);
        m_doc.ReadChildArray(n, "KeyAttrFlags", flags);
        m_doc.ReadChildArray(n, "KeyAttrRefCount", refCounts);
        const usize keys = Min(times.Size(), c.values.Size());
        c.values.Resize(keys);
        c.times.Resize(keys);
        c.constant.Resize(keys, 0);
        for (usize k = 0; k < keys; ++k)
            c.times[k] = f64(times[k]) / KTimeSecond;
        // Attribute runs: flags[r] applies to refCounts[r] keys (0x2 = constant interpolation).
        usize key = 0;
        for (usize r = 0; r < flags.Size() && key < keys; ++r) {
            const usize run = r < refCounts.Size() ? usize(Max(refCounts[r], 0)) : keys - key;
            for (usize k = 0; k < run && key < keys; ++k, ++key)
                c.constant[key] = (flags[r] & 0x2) != 0 ? 1 : 0;
        }
        m_curveOf.InsertOrAssign(curveObject, u32(m_curves.Size()));
        m_curves.PushBack(move(c));
        return &m_curves.Back();
    }

    void ImportAnimations()
    {
        if (m_scene.skeletons.Empty())
            return;
        m_curves.Reserve(m_objects.Size()); // stable pointers into m_curves
        for (u32 i = 0; i < m_objects.Size(); ++i) {
            if (m_objects[i].kind != "AnimationStack")
                continue;
            const Node* stackNode = m_objects[i].node;
            const i32 layer = ChildOfKind(i, "AnimationLayer");
            if (layer < 0)
                continue;
            // Channels per model.
            Array<ModelChannels> channels;
            f64 firstKey = 1e30, lastKey = -1e30;
            for (const Link& l : m_objects[usize(layer)].children) {
                const Object& curveNode = m_objects[l.object];
                if (curveNode.kind != "AnimationCurveNode")
                    continue;
                for (const Link& target : curveNode.parents) {
                    if (m_objects[target.object].kind != "Model")
                        continue;
                    const u32 channel = target.property == "Lcl Translation" ? 0u : target.property == "Lcl Rotation" ? 1u
                                        : target.property == "Lcl Scaling"   ? 2u
                                                                             : 3u;
                    if (channel > 2)
                        continue;
                    ModelChannels* mc = nullptr;
                    for (ModelChannels& c : channels)
                        if (c.model == target.object)
                            mc = &c;
                    if (!mc) {
                        mc = &channels.AddDefaulted();
                        mc->model = target.object;
                    }
                    mc->has[channel] = true;
                    mc->defaults[channel] = DVec3{PNumber(curveNode.node, "d|X", 0.0), PNumber(curveNode.node, "d|Y", 0.0), PNumber(curveNode.node, "d|Z", 0.0)};
                    for (const Link& cl : curveNode.children) {
                        if (m_objects[cl.object].kind != "AnimationCurve")
                            continue;
                        const u32 axis = cl.property == "d|X" ? 0u : cl.property == "d|Y" ? 1u : cl.property == "d|Z" ? 2u : 3u;
                        if (axis > 2)
                            continue;
                        const Curve* curve = LoadCurve(cl.object);
                        mc->curves[channel][axis] = curve;
                        if (!curve->times.Empty()) {
                            firstKey = Min(firstKey, curve->times[0]);
                            lastKey = Max(lastKey, curve->times.Back());
                        }
                    }
                }
            }
            // The skeleton most of the animated models belong to.
            Array<u32> votes;
            votes.Resize(m_scene.skeletons.Size(), 0);
            for (const ModelChannels& c : channels)
                if (m_boneOf[c.model].skeleton >= 0)
                    ++votes[usize(m_boneOf[c.model].skeleton)];
            u32 best = 0;
            for (u32 s = 1; s < votes.Size(); ++s)
                if (votes[s] > votes[best])
                    best = s;
            if (votes.Empty() || votes[best] == 0)
                continue;

            f64 start = firstKey, stop = lastKey;
            if (FindP(stackNode, "LocalStop")) {
                start = PNumber(stackNode, "LocalStart", 0.0) / KTimeSecond;
                stop = PNumber(stackNode, "LocalStop", 0.0) / KTimeSecond;
            }
            if (!(stop > start)) {
                start = firstKey;
                stop = lastKey;
            }
            if (!(stop >= start) || stop - start > MaxClipSeconds)
                continue;

            AnimationClip clip;
            StringView name = m_objects[i].name;
            if (const usize bar = name.FindLastOf("|"); bar != StringView::NPos) // Blender: "Armature|Action"
                name = name.SubStr(bar + 1);
            clip.name = name.Empty() ? String::Format("Animation{}", m_scene.animations.Size()) : String(name);
            clip.duration = f32(stop - start);
            clip.boneCount = m_scene.skeletons[best].BoneCount();
            for (const ModelChannels& c : channels)
                if (m_boneOf[c.model].skeleton == i32(best))
                    SampleTrack(c, u32(m_boneOf[c.model].bone), start, stop, clip);
            if (clip.tracks.Empty())
                continue;
            m_scene.animations.PushBack(move(clip));
            m_scene.animationSkeleton.PushBack(best);
        }
    }

    /// Resamples one bone: at its key times plus enough frames between them, through the full FBX transform and
    /// the change of basis; then drops keys that interpolation reproduces.
    void SampleTrack(const ModelChannels& c, u32 bone, f64 start, f64 stop, AnimationClip& clip)
    {
        Array<f64> times;
        times.PushBack(start);
        times.PushBack(stop);
        for (u32 ch = 0; ch < 3; ++ch)
            for (u32 a = 0; a < 3; ++a)
                if (const Curve* curve = c.curves[ch][a])
                    for (f64 t : curve->times)
                        if (t > start && t < stop)
                            times.PushBack(t);
        Sort(times);
        Array<f64> samples;
        const f64 step = 1.0 / Max(m_frameRate, 1.0);
        for (usize k = 0; k < times.Size(); ++k) {
            if (!samples.Empty() && times[k] - samples.Back() < 1e-6)
                continue;
            if (!samples.Empty())
                for (f64 t = samples.Back() + step; t < times[k] - step * 0.5; t += step)
                    samples.PushBack(t); // between sparse keys (Euler interpolation is not linear in quaternions)
            samples.PushBack(times[k]);
        }

        const Node* n = m_objects[c.model].node;
        const DVec3 zero{}, one{1, 1, 1};
        const DVec3 rest[3] = {PVec(n, "Lcl Translation", zero), PVec(n, "Lcl Rotation", zero), PVec(n, "Lcl Scaling", one)};
        AnimationTrack t;
        t.bone = bone;
        for (f64 time : samples) {
            DVec3 v[3];
            for (u32 ch = 0; ch < 3; ++ch) {
                const DVec3 base = c.has[ch] ? c.defaults[ch] : rest[ch];
                const f64 comps[3] = {base.x, base.y, base.z};
                f64 out[3];
                for (u32 a = 0; a < 3; ++a)
                    out[a] = c.curves[ch][a] ? c.curves[ch][a]->Evaluate(time) : comps[a];
                v[ch] = DVec3{out[0], out[1], out[2]};
            }
            const Transform local = Transform::FromMatrix(Convert(LocalMatrix(n, &v[0], &v[1], &v[2])).ToMat4());
            const f32 at = f32(time - start);
            t.positionTimes.PushBack(at);
            t.positions.PushBack(local.position);
            t.rotationTimes.PushBack(at);
            if (!t.rotations.Empty() && Dot(t.rotations.Back(), local.rotation) < 0.0f)
                t.rotations.PushBack(Quat(-local.rotation.x, -local.rotation.y, -local.rotation.z, -local.rotation.w));
            else
                t.rotations.PushBack(local.rotation);
            t.scaleTimes.PushBack(at);
            t.scales.PushBack(local.scale);
        }
        // Position error budget: 0.1 mm in the world (bone translations are in the parent's possibly scaled space).
        const i32 parent = m_scene.nodes[usize(m_nodeOf[c.model])].parent;
        f64 parentScale = 1.0;
        if (parent >= 0) {
            const DMat g = NodeGlobal(parent);
            parentScale = 0.0;
            for (u32 col = 0; col < 3; ++col)
                parentScale = Max(parentScale, std::sqrt(g.m[col][0] * g.m[col][0] + g.m[col][1] * g.m[col][1] + g.m[col][2] * g.m[col][2]));
        }
        ReduceVec3(t.positionTimes, t.positions, f32(1e-4 / Max(parentScale, 1e-9)));
        ReduceRotations(t.rotationTimes, t.rotations, 1e-4f); // 0.1 mm at 1 m
        ReduceVec3(t.scaleTimes, t.scales, 1e-4f);
        clip.tracks.PushBack(move(t));
    }

    /// Keeps the keys needed so that interpolating between kept keys reproduces every sample within the tolerance
    /// (each candidate segment is checked against all the samples it skips; segments span at most 256 samples).
    template <typename T, typename Interpolate, typename Error>
    static void ReduceKeys(Array<f32>& times, Array<T>& values, Interpolate interpolate, Error error, f32 tolerance)
    {
        const usize n = values.Size();
        if (n < 3)
            return;
        Array<f32> keptTimes;
        Array<T> kept;
        keptTimes.PushBack(times[0]);
        kept.PushBack(values[0]);
        usize anchor = 0;
        for (usize k = 2; k < n; ++k) {
            bool fits = k - anchor <= 256;
            for (usize m = anchor + 1; m < k && fits; ++m) {
                const f32 span = times[k] - times[anchor];
                const f32 a = span > 0.0f ? (times[m] - times[anchor]) / span : 0.0f;
                fits = error(interpolate(values[anchor], values[k], a), values[m]) <= tolerance;
            }
            if (!fits) {
                anchor = k - 1;
                keptTimes.PushBack(times[anchor]);
                kept.PushBack(values[anchor]);
            }
        }
        keptTimes.PushBack(times[n - 1]);
        kept.PushBack(values[n - 1]);
        if (kept.Size() == 2 && error(kept[0], kept[1]) <= tolerance) { // constant: one key
            keptTimes.PopBack();
            kept.PopBack();
        }
        times = move(keptTimes);
        values = move(kept);
    }

    static void ReduceVec3(Array<f32>& times, Array<Vec3>& values, f32 tolerance)
    {
        ReduceKeys(
            times, values, [](Vec3 a, Vec3 b, f32 t) { return a + (b - a) * t; }, [](Vec3 a, Vec3 b) { return Length(a - b); }, tolerance);
    }

    /// `tolerance` in radians.
    static void ReduceRotations(Array<f32>& times, Array<Quat>& values, f32 tolerance)
    {
        ReduceKeys(
            times, values, [](const Quat& a, const Quat& b, f32 t) { return Nlerp(a, b, t); },
            [](const Quat& a, const Quat& b) { return 2.0f * std::acos(Min(Abs(Dot(a, b)), 1.0f)); }, tolerance);
    }

    struct BoneRef
    {
        i32 skeleton;
        i32 bone;
    };
    struct SkeletonInfo
    {
        i32 rootParentNode = -1;
        Array<u32> boneModels;
        Array<DMat> bindGlobal; // engine space, bone global at bind time
        Array<u8> bound;
        Array<DMat> inverseBindSource; // bindGlobal^-1
        DMat y;
        bool haveY = false;
    };

    const Document& m_doc;
    String m_baseDir;
    String m_sourceName;
    const ImportOptions& m_options;
    ImportedScene m_scene;

    DMat m_convert, m_convertInverse;
    f64 m_frameRate = 30.0;
    bool m_blender = false;

    Array<Object> m_objects;
    HashMap<i64, u32> m_byId;
    Array<i32> m_nodeOf;     // object -> scene node (models)
    Array<u32> m_modelOf;    // scene node -> object
    Array<DMat> m_nodeLocal; // scene node -> engine-space local matrix
    Array<i32> m_materialOf; // object -> scene material
    HashMap<u32, i32> m_textureOf;
    i32 m_defaultMaterial = -1;

    Array<Skin> m_skins;
    HashMap<u32, i32> m_skeletonOfRoot;
    Array<SkeletonInfo> m_skeletonInfo;
    Array<BoneRef> m_boneOf; // object -> skeleton bone

    Array<Curve> m_curves;
    HashMap<u32, u32> m_curveOf;
};

} // namespace

Result<ImportedScene> ImportFbxFromMemory(ByteSpan data, StringView baseDirectory, StringView sourceName, const ImportOptions& options)
{
    Document doc;
    ZX_TRY(doc.Parse(data));
    FbxParser parser(doc, baseDirectory, sourceName, options);
    return parser.Run();
}

Array<String> ListFbxTextureFiles(StringView path)
{
    Array<u8> bytes;
    Document doc;
    if (!fs::ReadFile(path, bytes) || !doc.Parse(bytes.View()))
        return {};
    const ImportOptions options;
    FbxParser parser(doc, Path::Parent(path), Path::FileName(path), options);
    return parser.TextureFiles();
}

Result<ImportedScene> ImportFbx(StringView path, const ImportOptions& options)
{
    Array<u8> bytes;
    if (!fs::ReadFile(path, bytes))
        return Error::Make(ErrorCode::NotFound, "FBX: can't read '{}'", path);
    Result<ImportedScene> scene = ImportFbxFromMemory(bytes.View(), Path::Parent(path), Path::FileName(path), options);
    if (scene)
        scene->sourcePath = String(path);
    return scene;
}

} // namespace zx::asset
