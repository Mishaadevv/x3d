#pragma once

// Scene importers (own implementations): glTF 2.0 (.gltf + .bin / data URIs, .glb) and Wavefront OBJ/MTL.
// Everything is converted to engine conventions (left-handed, Y-up, +Z forward, meters, clockwise front faces,
// UV origin top-left).
//
//   Result<ImportedScene> scene = asset::ImportScene("C:/Models/Sponza/Sponza.gltf");

#include "Asset/Animation.h"
#include "Asset/MeshData.h"
#include "Core/Result.h"

namespace zx::asset {

enum class AlphaMode : u8
{
    Opaque,
    Mask,
    Blend
};

enum class TextureWrap : u8
{
    Repeat,
    Clamp,
    Mirror
};

struct TextureRef
{
    i32 texture = -1; // index into ImportedScene::textures
    u32 uvSet = 0;
    /// KHR_texture_transform: uv' = Rotate(uv * scale, rotation) + offset.
    Vec2 offset = Vec2(0.0f, 0.0f);
    Vec2 scale = Vec2(1.0f, 1.0f);
    f32 rotation = 0.0f;
    /// Normal map scale / occlusion strength.
    f32 strength = 1.0f;

    bool IsValid() const { return texture >= 0; }
};

struct ImportedMaterial
{
    String name;
    Vec4 baseColor = Vec4(1, 1, 1, 1); // linear
    f32 metallic = 1.0f;
    f32 roughness = 1.0f;
    Vec3 emissive = Vec3(0, 0, 0); // linear, already multiplied by emissive strength
    TextureRef baseColorTexture;         // sRGB
    TextureRef metallicRoughnessTexture; // linear, G = roughness, B = metallic
    TextureRef normalTexture;            // linear, OpenGL convention (green up)
    TextureRef occlusionTexture;         // linear, R
    TextureRef emissiveTexture;          // sRGB
    AlphaMode alphaMode = AlphaMode::Opaque;
    f32 alphaCutoff = 0.5f;
    bool doubleSided = false;
    bool unlit = false;
};

struct ImportedTexture
{
    String name;
    /// Resolved file path (empty if embedded).
    String path;
    /// Embedded encoded image (PNG/JPEG...), when path is empty.
    Array<u8> data;
    String mimeType;
    TextureWrap wrapU = TextureWrap::Repeat;
    TextureWrap wrapV = TextureWrap::Repeat;
    bool srgb = true; // decided by usage (base color / emissive)
    bool normalMap = false;
};

struct ImportedNode
{
    String name;
    Transform local;
    i32 parent = -1;
    i32 mesh = -1; // index into ImportedScene::meshes
    Array<i32> children;
    /// Skinned mesh: index into ImportedScene::skeletons; skinning matrices are skinToNode * model * inverseBind
    /// (skinToNode maps skeleton space into this node's space at import time).
    i32 skin = -1;
    Mat4 skinToNode = Mat4::Identity();
    bool isBone = false; // a joint of some skin (not instantiated as an entity)
};

struct ImportedScene
{
    String sourcePath;
    Array<MeshData> meshes;         // section materialIndex -> materials
    Array<ImportedMaterial> materials; // always non-empty if any mesh exists (a default material is added)
    Array<ImportedTexture> textures;
    Array<ImportedNode> nodes;
    Array<i32> rootNodes;
    Array<Skeleton> skeletons;          // one per glTF skin
    Array<AnimationClip> animations;
    Array<u32> animationSkeleton;       // per animation: the skeleton it drives

    u32 TotalTriangles() const;
};

struct ImportOptions
{
    bool generateNormals = true;  // when missing (flat for glTF as the spec requires, smooth for OBJ)
    bool generateTangents = true; // when missing and UV0 exists
    f32 scale = 1.0f;             // uniform scale applied to positions and node translations
};

Result<ImportedScene> ImportGltf(StringView path, const ImportOptions& options = {});
/// Parses glTF/GLB from memory; external files (buffers, images) are resolved relative to baseDirectory.
Result<ImportedScene> ImportGltfFromMemory(ByteSpan data, StringView baseDirectory, const ImportOptions& options = {});
Result<ImportedScene> ImportObj(StringView path, const ImportOptions& options = {});
/// Dispatches on the extension (.gltf/.glb/.obj).
Result<ImportedScene> ImportScene(StringView path, const ImportOptions& options = {});

} // namespace zx::asset
