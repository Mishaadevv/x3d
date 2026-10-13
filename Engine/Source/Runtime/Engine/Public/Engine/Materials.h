#pragma once

// Material graph assets of a project: .zmat files under the content root are loaded, compiled and registered with the
// renderer under their GUID, which MeshRenderer.materials reference like imported materials. Editing a graph and
// calling Apply() updates the viewport: value edits immediately, structural edits once the shader is compiled.
//
// Material instances (.zmati) reference a parent graph and override its named parameters (Scalar/Color values,
// texture paths). They share the parent's shader, so creating or editing one never compiles anything:
//   { "format": "zmati", "version": 1, "guid": "...", "parent": "Materials/Rust.zmat",
//     "overrides": { "Tint": { "value": [1, 0, 0, 1] }, "Albedo": { "texture": "Textures/Brick.png" } } }

#include "Render/MaterialGraph.h"
#include "Render/RenderAssets.h"

namespace zx {

class MaterialLibrary
{
public:
    struct Material
    {
        Guid guid;
        String path; // physical
        render::MaterialGraph graph;
        render::CompiledMaterialGraph compiled;
        u32 shader = 0;
        bool dirty = false; // edited since the last save
        // Instances only.
        Guid parent;
        JsonValue overrides; // parameter name -> { value | texture }
        bool IsInstance() const { return parent.IsValid(); }
    };

    MaterialLibrary(asset::AssetManager& assets, render::RenderAssets& renderAssets, StringView contentRoot);

    /// Loads every .zmat under the content root.
    void ScanContent();
    /// Loads (or reloads) a material file; returns its GUID or an invalid one on error (logged).
    Guid LoadFile(StringView physicalPath);
    /// Writes a new default material and loads it.
    Guid Create(StringView physicalPath);
    /// Writes a new instance of `parent` (no overrides) and loads it.
    Guid CreateInstance(const Guid& parent, StringView physicalPath);
    /// Instances: rebuilds the graph from the parent's graph + overrides (call after editing `overrides`).
    void RebuildInstance(const Guid& instance);
    bool Save(const Guid& guid);

    Material* Find(const Guid& guid);
    Material* FindByPath(StringView physicalPath);
    Span<const UniquePtr<Material>> All() const { return m_materials.View(); }

    /// Recompiles the graph and pushes it to the renderer (values and textures always, the shader when it changed).
    void Apply(const Guid& guid);
    /// Shader compile state of the material (Ready also when there is no shader yet... see MaterialShaderCache).
    render::MaterialShaderCache::State ShaderState(const Guid& guid) const;

    StringView ContentRoot() const { return m_contentRoot; }

private:
    asset::AssetId TextureAsset(StringView relativePath, bool normalMap);

    asset::AssetManager& m_assets;
    render::RenderAssets& m_renderAssets;
    String m_contentRoot;
    Array<UniquePtr<Material>> m_materials;
    HashMap<String, asset::AssetId> m_textures;
};

} // namespace zx
