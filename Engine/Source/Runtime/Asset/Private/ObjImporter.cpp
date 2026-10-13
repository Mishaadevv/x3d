#include "Asset/SceneImport.h"
#include "Core/Containers/HashMap.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/String/Parse.h"
#include "Core/String/Path.h"

#include <cmath>

namespace zx::asset {

namespace {

struct LineReader
{
    StringView text;
    usize pos = 0;
    bool Next(StringView& line)
    {
        if (pos >= text.Size())
            return false;
        const usize start = pos;
        while (pos < text.Size() && text[pos] != '\n')
            ++pos;
        line = text.SubStr(start, pos - start);
        ++pos;
        // Line continuation is rare in OBJ; ignore. Strip comments and CR.
        const usize hash = line.Find('#');
        if (hash != StringView::NPos)
            line = line.Left(hash);
        line = line.Trim();
        return true;
    }
};

StringView NextToken(StringView& s)
{
    s = s.TrimLeft();
    usize i = 0;
    while (i < s.Size() && s[i] != ' ' && s[i] != '\t')
        ++i;
    const StringView tok = s.Left(i);
    s = s.SubStr(i);
    return tok;
}

f32 NextFloat(StringView& s, f32 fallback = 0.0f)
{
    s = s.TrimLeft();
    f64 v;
    const usize n = ParseDoublePrefix(s, v);
    if (n == 0)
        return fallback;
    s = s.SubStr(n);
    return f32(v);
}

/// Texture path of a map_* statement (options like -bm 1.0 are skipped; the rest of the line is the file name).
StringView MapPath(StringView rest)
{
    rest = rest.Trim();
    while (rest.StartsWith('-')) {
        const StringView option = NextToken(rest);
        const bool vector = option == "-o" || option == "-s" || option == "-t"; // 1-3 numbers
        const u32 maxArgs = vector ? 3 : option == "-mm" ? 2 : 1;
        for (u32 i = 0; i < maxArgs; ++i) {
            f64 v;
            if ((vector || i > 0) && ParseDoublePrefix(rest.TrimLeft(), v) == 0)
                break; // optional numeric args ended
            NextToken(rest);
        }
        rest = rest.Trim();
    }
    return rest;
}

class MaterialTable
{
public:
    explicit MaterialTable(ImportedScene& scene) : m_scene(scene) {}

    i32 Texture(StringView baseDir, StringView file, bool srgb, bool normalMap)
    {
        String path = Path::Normalize(Path::Join(baseDir, file));
        if (const i32* existing = m_textures.Find(path))
            return *existing;
        const i32 index = i32(m_scene.textures.Size());
        ImportedTexture& t = m_scene.textures.AddDefaulted();
        t.name = String(Path::Stem(path));
        t.path = path;
        t.srgb = srgb;
        t.normalMap = normalMap;
        m_textures.InsertOrAssign(move(path), index);
        return index;
    }

    void LoadMtl(StringView path)
    {
        String text;
        if (!fs::ReadTextFile(path, text))
            return;
        const StringView baseDir = Path::Parent(path);
        LineReader reader{text.View()};
        StringView line;
        ImportedMaterial* mat = nullptr;
        while (reader.Next(line)) {
            StringView rest = line;
            const StringView key = NextToken(rest);
            if (key == "newmtl") {
                const String name(rest.Trim());
                m_byName.InsertOrAssign(name, u32(m_scene.materials.Size()));
                mat = &m_scene.materials.AddDefaulted();
                mat->name = name;
                mat->metallic = 0.0f;
                mat->roughness = 0.5f;
                continue;
            }
            if (!mat)
                continue;
            if (key == "Kd") {
                const f32 r = NextFloat(rest, 1.0f), g = NextFloat(rest, r), b = NextFloat(rest, g);
                mat->baseColor = Vec4(r, g, b, mat->baseColor.w);
            } else if (key == "d") {
                mat->baseColor.w = NextFloat(rest, 1.0f);
            } else if (key == "Tr") {
                mat->baseColor.w = 1.0f - NextFloat(rest, 0.0f);
            } else if (key == "Ns") {
                const f32 ns = Max(NextFloat(rest, 0.0f), 0.0f);
                mat->roughness = Clamp(std::sqrt(2.0f / (ns + 2.0f)), 0.04f, 1.0f); // Blinn-Phong exponent -> GGX roughness
            } else if (key == "Pr") {
                mat->roughness = NextFloat(rest, mat->roughness);
            } else if (key == "Pm") {
                mat->metallic = NextFloat(rest, 0.0f);
            } else if (key == "Ke") {
                const f32 r = NextFloat(rest), g = NextFloat(rest, r), b = NextFloat(rest, g);
                mat->emissive = Vec3(r, g, b);
            } else if (key == "map_Kd") {
                mat->baseColorTexture.texture = Texture(baseDir, MapPath(rest), true, false);
            } else if (key == "map_Bump" || key == "map_bump" || key == "bump" || key == "norm" || key == "map_Kn") {
                mat->normalTexture.texture = Texture(baseDir, MapPath(rest), false, true);
            } else if (key == "map_d") {
                // Alpha comes from a separate mask texture; treat the material as alpha-tested.
                mat->alphaMode = AlphaMode::Mask;
            } else if (key == "map_Ke") {
                mat->emissiveTexture.texture = Texture(baseDir, MapPath(rest), true, false);
                if (mat->emissive == Vec3(0, 0, 0))
                    mat->emissive = Vec3(1, 1, 1);
            } else if (key == "map_Pr" || key == "map_Pm" || key == "map_ORM") {
                // Rare: separate roughness/metallic maps are not packed here (needs the texture cooker).
            }
        }
        for (ImportedMaterial& m : m_scene.materials)
            if (m.baseColor.w < 1.0f && m.alphaMode == AlphaMode::Opaque)
                m.alphaMode = AlphaMode::Blend;
    }

    u32 Find(StringView name)
    {
        if (const u32* i = m_byName.Find(name))
            return *i;
        // Unknown material name: create a default-looking one so sections stay separate.
        const u32 index = u32(m_scene.materials.Size());
        ImportedMaterial& m = m_scene.materials.AddDefaulted();
        m.name = String(name);
        m.metallic = 0.0f;
        m.roughness = 0.5f;
        m_byName.InsertOrAssign(String(name), index);
        return index;
    }

private:
    ImportedScene& m_scene;
    HashMap<String, u32> m_byName;
    HashMap<String, i32> m_textures;
};

} // namespace

Result<ImportedScene> ImportObj(StringView path, const ImportOptions& options)
{
    String text;
    if (!fs::ReadTextFile(path, text))
        return Error::Make(ErrorCode::NotFound, "OBJ: can't read '{}'", path);
    const StringView baseDir = Path::Parent(path);

    ImportedScene scene;
    scene.sourcePath = String(path);
    MaterialTable materials(scene);
    Array<Vec3> positions, normals;
    Array<Vec2> uvs;
    Array<u32> colors;
    bool hasNormals = false, hasUvs = false, hasColors = false;

    // Corners grouped by material, in file order of first use.
    struct Group
    {
        u32 material;
        Array<MeshVertex> corners; // 3 per triangle
    };
    Array<Group> groups;
    i32 currentGroup = -1;
    auto useMaterial = [&](u32 material) {
        for (usize i = 0; i < groups.Size(); ++i)
            if (groups[i].material == material) {
                currentGroup = i32(i);
                return;
            }
        groups.PushBack(Group{material, {}});
        currentGroup = i32(groups.Size() - 1);
    };

    LineReader reader{text.View()};
    StringView line;
    u32 lineNumber = 0;
    SmallArray<MeshVertex, 16> polygon;
    while (reader.Next(line)) {
        ++lineNumber;
        if (line.Empty())
            continue;
        StringView rest = line;
        const StringView key = NextToken(rest);
        if (key == "v") {
            const f32 x = NextFloat(rest), y = NextFloat(rest), z = NextFloat(rest);
            positions.PushBack(Vec3(x, y, z) * options.scale);
            rest = rest.TrimLeft();
            if (!rest.Empty()) { // "v x y z r g b" vertex colors extension
                const f32 r = NextFloat(rest, 1.0f), g = NextFloat(rest, 1.0f), b = NextFloat(rest, 1.0f);
                colors.Resize(positions.Size() - 1, 0xFFFFFFFF);
                colors.PushBack(PackUnorm4x8(Vec4(r, g, b, 1.0f)));
                hasColors = true;
            }
        } else if (key == "vt") {
            const f32 u = NextFloat(rest), v = NextFloat(rest);
            uvs.PushBack(Vec2(u, 1.0f - v)); // OBJ origin is bottom-left
        } else if (key == "vn") {
            const f32 x = NextFloat(rest), y = NextFloat(rest), z = NextFloat(rest);
            normals.PushBack(Vec3(x, y, z));
        } else if (key == "f") {
            if (currentGroup < 0)
                useMaterial(materials.Find("Default"));
            polygon.Clear();
            for (StringView tok = NextToken(rest); !tok.Empty(); tok = NextToken(rest)) {
                // v, v/vt, v//vn, v/vt/vn; negative = relative to the end.
                i64 idx[3] = {0, 0, 0};
                u32 part = 0;
                usize start = 0;
                for (usize i = 0; i <= tok.Size() && part < 3; ++i) {
                    if (i == tok.Size() || tok[i] == '/') {
                        const StringView s = tok.SubStr(start, i - start);
                        if (!s.Empty()) {
                            const Optional<i64> v = ParseInt(s);
                            if (!v)
                                return Error::Make(ErrorCode::InvalidData, "OBJ:{}: bad index '{}'", lineNumber, tok);
                            idx[part] = *v;
                        }
                        ++part;
                        start = i + 1;
                    }
                }
                auto resolve = [](i64 i, usize count) -> i64 { return i > 0 ? i - 1 : i < 0 ? i64(count) + i : -1; };
                const i64 p = resolve(idx[0], positions.Size()), t = resolve(idx[1], uvs.Size()), n = resolve(idx[2], normals.Size());
                if (p < 0 || p >= i64(positions.Size()) || t >= i64(uvs.Size()) || n >= i64(normals.Size()))
                    return Error::Make(ErrorCode::InvalidData, "OBJ:{}: index out of range in '{}'", lineNumber, tok);
                MeshVertex v;
                v.position = positions[usize(p)];
                if (t >= 0) {
                    v.uv0 = uvs[usize(t)];
                    hasUvs = true;
                }
                if (n >= 0) {
                    v.normal = SafeNormalize(normals[usize(n)], Vec3::Up());
                    hasNormals = true;
                }
                if (hasColors && usize(p) < colors.Size())
                    v.color = colors[usize(p)];
                polygon.PushBack(v);
            }
            Array<MeshVertex>& corners = groups[usize(currentGroup)].corners;
            for (usize i = 1; i + 1 < polygon.Size(); ++i) // fan triangulation
                corners.Append({polygon[0], polygon[i], polygon[i + 1]});
        } else if (key == "usemtl") {
            useMaterial(materials.Find(rest.Trim()));
        } else if (key == "mtllib") {
            materials.LoadMtl(Path::Join(baseDir, rest.Trim()));
        }
        // o / g / s / l / p: ignored (one mesh per file; smoothing groups come from welding).
    }

    MeshData mesh;
    mesh.name = String(Path::Stem(path));
    MeshLod& lod = mesh.lods.AddDefaulted();
    for (Group& g : groups) {
        if (g.corners.Empty())
            continue;
        MeshSection s;
        s.indexOffset = u32(lod.indices.Size());
        s.indexCount = u32(g.corners.Size());
        s.materialIndex = g.material;
        for (const MeshVertex& v : g.corners) {
            lod.indices.PushBack(u32(mesh.vertices.Size()));
            mesh.vertices.PushBack(v);
        }
        lod.sections.PushBack(s);
    }
    if (lod.indices.Empty())
        return Error::Make(ErrorCode::InvalidData, "OBJ: '{}' contains no faces", Path::FileName(path));
    mesh.attributes = (hasNormals ? MeshAttributes::Normals : MeshAttributes::None) |
                      (hasUvs ? MeshAttributes::UV0 : MeshAttributes::None) |
                      (hasColors ? MeshAttributes::Colors : MeshAttributes::None);
    mesh::MirrorX(mesh);
    mesh::WeldVertices(mesh);
    if (!hasNormals && options.generateNormals)
        mesh::GenerateNormals(mesh); // smooth over welded positions
    if (hasUvs && options.generateTangents && (mesh.attributes & MeshAttributes::Normals) != MeshAttributes::None)
        mesh::GenerateTangents(mesh);
    mesh.ComputeBounds();
    scene.meshes.PushBack(move(mesh));
    if (scene.materials.Empty())
        materials.Find("Default");
    ImportedNode& node = scene.nodes.AddDefaulted();
    node.name = scene.meshes[0].name;
    node.mesh = 0;
    scene.rootNodes.PushBack(0);
    return scene;
}

} // namespace zx::asset
