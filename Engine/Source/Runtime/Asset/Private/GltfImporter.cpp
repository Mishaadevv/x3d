#include "Asset/SceneImport.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Serialization/Json.h"
#include "Core/String/Base64.h"
#include "Core/String/Path.h"

namespace zx::asset {

u32 ImportedScene::TotalTriangles() const
{
    u32 n = 0;
    for (const ImportedNode& node : nodes)
        if (node.mesh >= 0)
            n += meshes[node.mesh].TriangleCount();
    return n;
}

namespace {

constexpr u32 GlbMagic = 0x46546C67; // "glTF"
constexpr u32 GlbJson = 0x4E4F534A;  // "JSON"
constexpr u32 GlbBin = 0x004E4942;   // "BIN\0"

u32 ReadU32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }

u32 ComponentSize(u32 componentType)
{
    switch (componentType) {
    case 5120:
    case 5121: return 1;
    case 5122:
    case 5123: return 2;
    case 5125:
    case 5126: return 4;
    default: return 0;
    }
}

u32 TypeComponents(StringView type)
{
    if (type == "SCALAR")
        return 1;
    if (type == "VEC2")
        return 2;
    if (type == "VEC3")
        return 3;
    if (type == "VEC4" || type == "MAT2")
        return 4;
    if (type == "MAT3")
        return 9;
    if (type == "MAT4")
        return 16;
    return 0;
}

f32 ReadComponent(const u8* p, u32 componentType, bool normalized)
{
    switch (componentType) {
    case 5120: {
        const f32 v = f32(i8(p[0]));
        return normalized ? Max(v / 127.0f, -1.0f) : v;
    }
    case 5121: return normalized ? f32(p[0]) / 255.0f : f32(p[0]);
    case 5122: {
        const f32 v = f32(i16(u16(p[0] | (p[1] << 8))));
        return normalized ? Max(v / 32767.0f, -1.0f) : v;
    }
    case 5123: {
        const f32 v = f32(u16(p[0] | (p[1] << 8)));
        return normalized ? v / 65535.0f : v;
    }
    case 5125: return f32(ReadU32(p));
    case 5126: {
        f32 v;
        MemCopy(&v, p, 4);
        return v;
    }
    default: return 0.0f;
    }
}

u32 ReadIndexComponent(const u8* p, u32 componentType)
{
    switch (componentType) {
    case 5121: return p[0];
    case 5123: return u32(p[0]) | (u32(p[1]) << 8);
    case 5125: return ReadU32(p);
    default: return 0;
    }
}

Vec3 ToEngine(Vec3 v) { return Vec3(-v.x, v.y, v.z); }
Quat ToEngine(Quat q) { return Quat(q.x, -q.y, -q.z, q.w); }

class GltfParser
{
public:
    GltfParser(const JsonValue& root, StringView baseDir, ByteSpan glbBin, const ImportOptions& options)
        : m_root(root), m_baseDir(baseDir), m_glbBin(glbBin), m_options(options)
    {
    }

    Result<ImportedScene> Run()
    {
        const StringView version = m_root.Get("asset").Get("version").AsString();
        if (!version.StartsWith("2"))
            return Error::Make(ErrorCode::Unsupported, "glTF: unsupported version '{}'", version);
        for (const JsonValue& ext : m_root.Get("extensionsRequired").Elements()) {
            const StringView name = ext.AsString();
            if (name != "KHR_texture_transform" && name != "KHR_materials_emissive_strength" && name != "KHR_materials_unlit")
                return Error::Make(ErrorCode::Unsupported, "glTF: required extension '{}' is not supported", name);
        }
        ZX_TRY(LoadBuffers());
        ImportTextures();
        ImportMaterials();
        ZX_TRY(ImportMeshes());
        ImportNodes();
        return move(m_scene);
    }

private:
    Result<void> LoadBuffers()
    {
        for (const JsonValue& b : m_root.Get("buffers").Elements()) {
            Array<u8>& data = m_buffers.AddDefaulted();
            const JsonValue* uri = b.Find("uri");
            if (!uri) {
                if (m_buffers.Size() != 1 || m_glbBin.Empty())
                    return Error::Make(ErrorCode::InvalidData, "glTF: buffer without uri outside a GLB");
                data = Array<u8>(m_glbBin);
            } else if (!LoadUri(uri->AsString(), data)) {
                return Error::Make(ErrorCode::NotFound, "glTF: can't load buffer '{}'", uri->AsString().Left(64));
            }
            if (data.Size() < usize(b.Get("byteLength").AsU64()))
                return Error::Make(ErrorCode::InvalidData, "glTF: buffer is shorter than byteLength");
        }
        return {};
    }

    bool LoadUri(StringView uri, Array<u8>& out) const
    {
        if (uri.StartsWith("data:")) {
            const usize comma = uri.Find(',');
            if (comma == StringView::NPos || uri.Left(comma).Find(";base64") == StringView::NPos)
                return false;
            return Base64Decode(uri.SubStr(comma + 1), out);
        }
        return fs::ReadFile(Path::Join(m_baseDir, UriDecode(uri)), out);
    }

    /// Returns a pointer to the bufferView's data and its length.
    bool GetBufferView(i64 index, const u8*& data, usize& length, u32& stride) const
    {
        const JsonValue& views = m_root.Get("bufferViews");
        if (index < 0 || usize(index) >= views.Size())
            return false;
        const JsonValue& v = views[usize(index)];
        const i64 buffer = v.Get("buffer").AsInt(-1);
        if (buffer < 0 || usize(buffer) >= m_buffers.Size())
            return false;
        const u64 offset = v.Get("byteOffset").AsU64();
        length = usize(v.Get("byteLength").AsU64());
        if (offset + length > m_buffers[usize(buffer)].Size())
            return false;
        data = m_buffers[usize(buffer)].Data() + offset;
        stride = u32(v.Get("byteStride").AsU64());
        return true;
    }

    /// Reads an accessor as floats (count * components), applying normalization and sparse substitution.
    bool ReadFloats(i64 accessorIndex, u32 expectedComponents, Array<f32>& out) const
    {
        const JsonValue& accessors = m_root.Get("accessors");
        if (accessorIndex < 0 || usize(accessorIndex) >= accessors.Size())
            return false;
        const JsonValue& a = accessors[usize(accessorIndex)];
        const u32 componentType = u32(a.Get("componentType").AsU64());
        const u32 components = TypeComponents(a.Get("type").AsString());
        const u32 count = u32(a.Get("count").AsU64());
        const bool normalized = a.Get("normalized").AsBool();
        const u32 csize = ComponentSize(componentType);
        if (!csize || !components || (expectedComponents && components != expectedComponents))
            return false;
        out.Clear();
        out.Resize(usize(count) * components, 0.0f);
        if (const JsonValue* bv = a.Find("bufferView")) {
            const u8* data;
            usize length;
            u32 stride;
            if (!GetBufferView(bv->AsInt(-1), data, length, stride))
                return false;
            const u32 elementSize = csize * components;
            stride = stride ? stride : elementSize;
            const u64 offset = a.Get("byteOffset").AsU64();
            if (count && offset + u64(stride) * (count - 1) + elementSize > length)
                return false;
            for (u32 i = 0; i < count; ++i)
                for (u32 c = 0; c < components; ++c)
                    out[usize(i) * components + c] = ReadComponent(data + offset + usize(i) * stride + c * csize, componentType, normalized);
        }
        if (const JsonValue* sparse = a.Find("sparse")) {
            const u32 n = u32(sparse->Get("count").AsU64());
            const JsonValue& si = sparse->Get("indices");
            const JsonValue& sv = sparse->Get("values");
            const u8 *idata, *vdata;
            usize ilen, vlen;
            u32 istride, vstride;
            if (!GetBufferView(si.Get("bufferView").AsInt(-1), idata, ilen, istride) ||
                !GetBufferView(sv.Get("bufferView").AsInt(-1), vdata, vlen, vstride))
                return false;
            const u32 itype = u32(si.Get("componentType").AsU64());
            const usize ioff = usize(si.Get("byteOffset").AsU64()), voff = usize(sv.Get("byteOffset").AsU64());
            const u32 isize = ComponentSize(itype);
            if (ioff + usize(n) * isize > ilen || voff + usize(n) * csize * components > vlen)
                return false;
            for (u32 k = 0; k < n; ++k) {
                const u32 target = ReadIndexComponent(idata + ioff + usize(k) * isize, itype);
                if (target >= count)
                    return false;
                for (u32 c = 0; c < components; ++c)
                    out[usize(target) * components + c] =
                        ReadComponent(vdata + voff + (usize(k) * components + c) * csize, componentType, normalized);
            }
        }
        return true;
    }

    bool ReadIndices(i64 accessorIndex, Array<u32>& out) const
    {
        Array<f32> values;
        // Indices are integers <= 2^32; floats lose precision above 2^24, so read them directly.
        const JsonValue& accessors = m_root.Get("accessors");
        if (accessorIndex < 0 || usize(accessorIndex) >= accessors.Size())
            return false;
        const JsonValue& a = accessors[usize(accessorIndex)];
        const u32 componentType = u32(a.Get("componentType").AsU64());
        const u32 count = u32(a.Get("count").AsU64());
        const u32 csize = ComponentSize(componentType);
        if (componentType != 5121 && componentType != 5123 && componentType != 5125)
            return false;
        const u8* data;
        usize length;
        u32 stride;
        if (!GetBufferView(a.Get("bufferView").AsInt(-1), data, length, stride))
            return false;
        stride = stride ? stride : csize;
        const u64 offset = a.Get("byteOffset").AsU64();
        if (count && offset + u64(stride) * (count - 1) + csize > length)
            return false;
        out.ResizeUninitialized(count);
        for (u32 i = 0; i < count; ++i)
            out[i] = ReadIndexComponent(data + offset + usize(i) * stride, componentType);
        return true;
    }

    TextureRef ReadTextureRef(const JsonValue& info, bool srgb, bool normalMap)
    {
        TextureRef ref;
        if (info.IsNull())
            return ref;
        ref.texture = i32(info.Get("index").AsInt(-1));
        ref.uvSet = u32(info.Get("texCoord").AsU64());
        ref.strength = info.Contains("scale") ? info.Get("scale").AsFloat(1.0f) : info.Get("strength").AsFloat(1.0f);
        const JsonValue& tt = info.Get("extensions").Get("KHR_texture_transform");
        if (tt.IsObject()) {
            const JsonValue& off = tt.Get("offset");
            const JsonValue& sc = tt.Get("scale");
            if (off.Size() == 2)
                ref.offset = Vec2(off[0].AsFloat(), off[1].AsFloat());
            if (sc.Size() == 2)
                ref.scale = Vec2(sc[0].AsFloat(1.0f), sc[1].AsFloat(1.0f));
            ref.rotation = tt.Get("rotation").AsFloat();
            if (tt.Contains("texCoord"))
                ref.uvSet = u32(tt.Get("texCoord").AsU64());
        }
        if (ref.texture >= 0 && usize(ref.texture) < m_scene.textures.Size()) {
            m_scene.textures[usize(ref.texture)].srgb = srgb;
            m_scene.textures[usize(ref.texture)].normalMap |= normalMap;
        } else {
            ref.texture = -1;
        }
        return ref;
    }

    void ImportTextures()
    {
        const JsonValue& images = m_root.Get("images");
        const JsonValue& samplers = m_root.Get("samplers");
        auto wrap = [](i64 mode) {
            return mode == 33071 ? TextureWrap::Clamp : mode == 33648 ? TextureWrap::Mirror : TextureWrap::Repeat;
        };
        for (const JsonValue& t : m_root.Get("textures").Elements()) {
            ImportedTexture& tex = m_scene.textures.AddDefaulted();
            tex.srgb = false;
            i64 source = t.Get("source").AsInt(-1);
            // Fall back to extension-provided sources (e.g. KHR_texture_basisu/EXT_texture_webp) only if no PNG/JPEG.
            if (source < 0)
                for (const JsonMember& ext : t.Get("extensions").Members())
                    source = ext.second.Get("source").AsInt(-1);
            if (const JsonValue* s = t.Find("sampler"); s && usize(s->AsInt()) < samplers.Size()) {
                const JsonValue& smp = samplers[usize(s->AsInt())];
                tex.wrapU = wrap(smp.Get("wrapS").AsInt(10497));
                tex.wrapV = wrap(smp.Get("wrapT").AsInt(10497));
            }
            if (source < 0 || usize(source) >= images.Size())
                continue;
            const JsonValue& img = images[usize(source)];
            tex.name = String(img.Get("name").AsString(t.Get("name").AsString()));
            tex.mimeType = String(img.Get("mimeType").AsString());
            if (const JsonValue* uri = img.Find("uri")) {
                const StringView u = uri->AsString();
                if (u.StartsWith("data:")) {
                    LoadUri(u, tex.data);
                    const usize semi = u.Find(';');
                    if (tex.mimeType.Empty() && semi != StringView::NPos)
                        tex.mimeType = String(u.SubStr(5, semi - 5));
                } else {
                    tex.path = Path::Normalize(Path::Join(m_baseDir, UriDecode(u)));
                    if (tex.name.Empty())
                        tex.name = String(Path::Stem(tex.path));
                }
            } else if (const JsonValue* bv = img.Find("bufferView")) {
                const u8* data;
                usize length;
                u32 stride;
                if (GetBufferView(bv->AsInt(-1), data, length, stride))
                    tex.data = Array<u8>(Span<const u8>(data, length));
            }
        }
    }

    void ImportMaterials()
    {
        for (const JsonValue& m : m_root.Get("materials").Elements()) {
            ImportedMaterial& mat = m_scene.materials.AddDefaulted();
            mat.name = String(m.Get("name").AsString());
            const JsonValue& pbr = m.Get("pbrMetallicRoughness");
            const JsonValue& bc = pbr.Get("baseColorFactor");
            if (bc.Size() == 4)
                mat.baseColor = Vec4(bc[0].AsFloat(), bc[1].AsFloat(), bc[2].AsFloat(), bc[3].AsFloat());
            mat.metallic = pbr.Get("metallicFactor").AsFloat(1.0f);
            mat.roughness = pbr.Get("roughnessFactor").AsFloat(1.0f);
            mat.baseColorTexture = ReadTextureRef(pbr.Get("baseColorTexture"), true, false);
            mat.metallicRoughnessTexture = ReadTextureRef(pbr.Get("metallicRoughnessTexture"), false, false);
            mat.normalTexture = ReadTextureRef(m.Get("normalTexture"), false, true);
            mat.occlusionTexture = ReadTextureRef(m.Get("occlusionTexture"), false, false);
            mat.emissiveTexture = ReadTextureRef(m.Get("emissiveTexture"), true, false);
            const JsonValue& em = m.Get("emissiveFactor");
            if (em.Size() == 3)
                mat.emissive = Vec3(em[0].AsFloat(), em[1].AsFloat(), em[2].AsFloat());
            mat.emissive *= m.Get("extensions").Get("KHR_materials_emissive_strength").Get("emissiveStrength").AsFloat(1.0f);
            const StringView alpha = m.Get("alphaMode").AsString("OPAQUE");
            mat.alphaMode = alpha == "MASK" ? AlphaMode::Mask : alpha == "BLEND" ? AlphaMode::Blend : AlphaMode::Opaque;
            mat.alphaCutoff = m.Get("alphaCutoff").AsFloat(0.5f);
            mat.doubleSided = m.Get("doubleSided").AsBool();
            mat.unlit = m.Get("extensions").Contains("KHR_materials_unlit");
        }
    }

    Result<void> ImportPrimitive(const JsonValue& prim, MeshData& out, u32& defaultMaterialUses)
    {
        const i64 mode = prim.Get("mode").AsInt(4);
        if (mode != 4 && mode != 5 && mode != 6)
            return {}; // points/lines are not rendered as meshes
        const JsonValue& attrs = prim.Get("attributes");
        Array<f32> pos, nrm, tan, uv0, uv1, col;
        if (!ReadFloats(attrs.Get("POSITION").AsInt(-1), 3, pos))
            return Error::Make(ErrorCode::InvalidData, "glTF: primitive without valid POSITION");
        const u32 count = u32(pos.Size() / 3);
        const bool hasNormals = attrs.Contains("NORMAL") && ReadFloats(attrs.Get("NORMAL").AsInt(-1), 3, nrm);
        const bool hasTangents = hasNormals && attrs.Contains("TANGENT") && ReadFloats(attrs.Get("TANGENT").AsInt(-1), 4, tan);
        const bool hasUv0 = attrs.Contains("TEXCOORD_0") && ReadFloats(attrs.Get("TEXCOORD_0").AsInt(-1), 2, uv0);
        const bool hasUv1 = attrs.Contains("TEXCOORD_1") && ReadFloats(attrs.Get("TEXCOORD_1").AsInt(-1), 2, uv1);
        u32 colorComponents = 0;
        if (attrs.Contains("COLOR_0")) {
            if (ReadFloats(attrs.Get("COLOR_0").AsInt(-1), 4, col))
                colorComponents = 4;
            else if (ReadFloats(attrs.Get("COLOR_0").AsInt(-1), 3, col))
                colorComponents = 3;
        }

        MeshData p;
        p.vertices.Resize(count);
        for (u32 i = 0; i < count; ++i) {
            MeshVertex& v = p.vertices[i];
            v.position = Vec3(pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]) * m_options.scale;
            if (hasNormals)
                v.normal = SafeNormalize(Vec3(nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]), Vec3::Up());
            if (hasTangents)
                v.tangent = Vec4(tan[i * 4], tan[i * 4 + 1], tan[i * 4 + 2], tan[i * 4 + 3] < 0.0f ? -1.0f : 1.0f);
            if (hasUv0)
                v.uv0 = Vec2(uv0[i * 2], uv0[i * 2 + 1]);
            if (hasUv1)
                v.uv1 = Vec2(uv1[i * 2], uv1[i * 2 + 1]);
            if (colorComponents) {
                const f32* c = &col[usize(i) * colorComponents];
                v.color = PackUnorm4x8(Vec4(c[0], c[1], c[2], colorComponents == 4 ? c[3] : 1.0f));
            }
        }
        Array<u32> indices;
        if (prim.Contains("indices")) {
            if (!ReadIndices(prim.Get("indices").AsInt(-1), indices))
                return Error::Make(ErrorCode::InvalidData, "glTF: invalid index accessor");
        } else {
            indices.ResizeUninitialized(count);
            for (u32 i = 0; i < count; ++i)
                indices[i] = i;
        }
        for (u32 i : indices)
            if (i >= count)
                return Error::Make(ErrorCode::InvalidData, "glTF: index {} out of range ({} vertices)", i, count);
        MeshLod& lod = p.lods.AddDefaulted();
        if (mode == 4) {
            lod.indices = move(indices);
            lod.indices.Resize(lod.indices.Size() / 3 * 3);
        } else if (mode == 5) { // strip
            for (usize i = 0; i + 2 < indices.Size(); ++i)
                lod.indices.Append((i & 1) ? std::initializer_list<u32>{indices[i + 1], indices[i], indices[i + 2]}
                                           : std::initializer_list<u32>{indices[i], indices[i + 1], indices[i + 2]});
        } else { // fan
            for (usize i = 1; i + 1 < indices.Size(); ++i)
                lod.indices.Append({indices[0], indices[i], indices[i + 1]});
        }
        i64 material = prim.Get("material").AsInt(-1);
        if (material < 0 || usize(material) >= m_scene.materials.Size()) {
            material = i64(m_scene.materials.Size()); // default material, appended at the end
            ++defaultMaterialUses;
        }
        lod.sections.PushBack(MeshSection{0, u32(lod.indices.Size()), u32(material)});
        p.attributes = (hasNormals ? MeshAttributes::Normals : MeshAttributes::None) |
                       (hasTangents ? MeshAttributes::Tangents : MeshAttributes::None) |
                       (hasUv0 ? MeshAttributes::UV0 : MeshAttributes::None) |
                       (hasUv1 ? MeshAttributes::UV1 : MeshAttributes::None) |
                       (colorComponents ? MeshAttributes::Colors : MeshAttributes::None);

        mesh::MirrorX(p);
        if (!hasNormals && m_options.generateNormals) {
            // glTF: missing normals mean flat shading - give every triangle its own vertices.
            Array<MeshVertex> flat;
            flat.ResizeUninitialized(p.lods[0].indices.Size());
            for (usize i = 0; i < flat.Size(); ++i) {
                flat[i] = p.vertices[p.lods[0].indices[i]];
                p.lods[0].indices[i] = u32(i);
            }
            p.vertices = move(flat);
            mesh::GenerateNormals(p);
            mesh::WeldVertices(p);
        }
        if (!hasTangents && hasUv0 && m_options.generateTangents && (p.attributes & MeshAttributes::Normals) != MeshAttributes::None)
            mesh::GenerateTangents(p);
        p.ComputeBounds();
        mesh::Append(out, p);
        return {};
    }

    Result<void> ImportMeshes()
    {
        u32 defaultMaterialUses = 0;
        for (const JsonValue& m : m_root.Get("meshes").Elements()) {
            MeshData& mesh = m_scene.meshes.AddDefaulted();
            mesh.name = String(m.Get("name").AsString());
            mesh.lods.AddDefaulted();
            for (const JsonValue& prim : m.Get("primitives").Elements())
                ZX_TRY(ImportPrimitive(prim, mesh, defaultMaterialUses));
            mesh.ComputeBounds();
        }
        if (defaultMaterialUses || (m_scene.materials.Empty() && !m_scene.meshes.Empty())) {
            ImportedMaterial& def = m_scene.materials.AddDefaulted();
            def.name = "Default";
            def.metallic = 0.0f;
            def.roughness = 0.5f;
        }
        return {};
    }

    void ImportNodes()
    {
        const JsonValue& nodes = m_root.Get("nodes");
        m_scene.nodes.Resize(nodes.Size());
        for (usize i = 0; i < nodes.Size(); ++i) {
            const JsonValue& n = nodes[i];
            ImportedNode& node = m_scene.nodes[i];
            node.name = String(n.Get("name").AsString());
            node.mesh = i32(n.Get("mesh").AsInt(-1));
            if (node.mesh >= i32(m_scene.meshes.Size()))
                node.mesh = -1;
            const JsonValue& matrix = n.Get("matrix");
            Transform t;
            if (matrix.Size() == 16) {
                Mat4 m;
                for (u32 c = 0; c < 4; ++c)
                    m.columns[c] = Vec4(matrix[c * 4].AsFloat(), matrix[c * 4 + 1].AsFloat(), matrix[c * 4 + 2].AsFloat(),
                                        matrix[c * 4 + 3].AsFloat());
                t = Transform::FromMatrix(m);
            } else {
                const JsonValue& tr = n.Get("translation");
                const JsonValue& rot = n.Get("rotation");
                const JsonValue& sc = n.Get("scale");
                if (tr.Size() == 3)
                    t.position = Vec3(tr[0].AsFloat(), tr[1].AsFloat(), tr[2].AsFloat());
                if (rot.Size() == 4)
                    t.rotation = Normalize(Quat(rot[0].AsFloat(), rot[1].AsFloat(), rot[2].AsFloat(), rot[3].AsFloat(1.0f)));
                if (sc.Size() == 3)
                    t.scale = Vec3(sc[0].AsFloat(1.0f), sc[1].AsFloat(1.0f), sc[2].AsFloat(1.0f));
            }
            node.local = Transform(ToEngine(t.position) * m_options.scale, ToEngine(t.rotation), t.scale);
            for (const JsonValue& c : n.Get("children").Elements()) {
                const i64 child = c.AsInt(-1);
                if (child >= 0 && usize(child) < nodes.Size())
                    node.children.PushBack(i32(child));
            }
        }
        for (usize i = 0; i < m_scene.nodes.Size(); ++i)
            for (i32 c : m_scene.nodes[i].children)
                m_scene.nodes[usize(c)].parent = i32(i);
        const JsonValue& scenes = m_root.Get("scenes");
        const usize sceneIndex = usize(m_root.Get("scene").AsInt(0));
        if (sceneIndex < scenes.Size()) {
            for (const JsonValue& r : scenes[sceneIndex].Get("nodes").Elements())
                if (usize(r.AsInt(-1)) < m_scene.nodes.Size())
                    m_scene.rootNodes.PushBack(i32(r.AsInt()));
        } else {
            for (usize i = 0; i < m_scene.nodes.Size(); ++i)
                if (m_scene.nodes[i].parent < 0)
                    m_scene.rootNodes.PushBack(i32(i));
        }
        // Meshes with no node referencing them (rare) still get a root node so nothing is lost.
        if (m_scene.nodes.Empty())
            for (usize m = 0; m < m_scene.meshes.Size(); ++m) {
                ImportedNode& node = m_scene.nodes.AddDefaulted();
                node.name = m_scene.meshes[m].name;
                node.mesh = i32(m);
                m_scene.rootNodes.PushBack(i32(m_scene.nodes.Size() - 1));
            }
    }

    const JsonValue& m_root;
    String m_baseDir;
    ByteSpan m_glbBin;
    const ImportOptions& m_options;
    Array<Array<u8>> m_buffers;
    ImportedScene m_scene;
};

} // namespace

Result<ImportedScene> ImportGltfFromMemory(ByteSpan data, StringView baseDirectory, const ImportOptions& options)
{
    StringView jsonText;
    ByteSpan bin;
    if (data.Size() >= 12 && ReadU32(data.Data()) == GlbMagic) {
        if (ReadU32(data.Data() + 4) != 2)
            return Error::Make(ErrorCode::Unsupported, "GLB: unsupported container version");
        const usize total = Min(usize(ReadU32(data.Data() + 8)), data.Size());
        usize pos = 12;
        while (pos + 8 <= total) {
            const u32 length = ReadU32(data.Data() + pos), type = ReadU32(data.Data() + pos + 4);
            if (pos + 8 + length > total)
                return Error::Make(ErrorCode::InvalidData, "GLB: truncated chunk");
            if (type == GlbJson && jsonText.Empty())
                jsonText = StringView(reinterpret_cast<const char*>(data.Data() + pos + 8), length);
            else if (type == GlbBin && bin.Empty())
                bin = ByteSpan(data.Data() + pos + 8, length);
            pos += 8 + usize(AlignUp(u64(length), u64(4)));
        }
        if (jsonText.Empty())
            return Error::Make(ErrorCode::InvalidData, "GLB: missing JSON chunk");
    } else {
        jsonText = StringView(reinterpret_cast<const char*>(data.Data()), data.Size());
        if (jsonText.StartsWith("\xEF\xBB\xBF"))
            jsonText = jsonText.SubStr(3);
    }
    JsonValue root;
    JsonParseError err;
    if (!ParseJson(jsonText, root, &err))
        return Error::Make(ErrorCode::InvalidData, "glTF: JSON error at {}:{}: {}", err.line, err.column, err.message);
    GltfParser parser(root, baseDirectory, bin, options);
    return parser.Run();
}

Result<ImportedScene> ImportGltf(StringView path, const ImportOptions& options)
{
    Array<u8> bytes;
    if (!fs::ReadFile(path, bytes))
        return Error::Make(ErrorCode::NotFound, "glTF: can't read '{}'", path);
    Result<ImportedScene> scene = ImportGltfFromMemory(bytes.View(), Path::Parent(path), options);
    if (scene)
        scene->sourcePath = String(path);
    return scene;
}

Result<ImportedScene> ImportScene(StringView path, const ImportOptions& options)
{
    if (Path::HasExtension(path, "gltf") || Path::HasExtension(path, "glb"))
        return ImportGltf(path, options);
    if (Path::HasExtension(path, "obj"))
        return ImportObj(path, options);
    return Error::Make(ErrorCode::Unsupported, "No importer for '{}'", Path::FileName(path));
}

} // namespace zx::asset
