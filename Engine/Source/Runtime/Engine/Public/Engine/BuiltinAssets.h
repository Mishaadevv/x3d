#pragma once

// Built-in assets available in every project: basic shapes (1 m sized, UV-mapped, with tangents) registered as
// in-memory meshes under fixed GUIDs, so scenes can reference them like any other mesh.
//
//   RegisterBuiltinAssets(assets);                 // once, after creating the AssetManager
//   meshRenderer.mesh = builtin::Mesh(BuiltinShape::Sphere);

#include "Asset/AssetManager.h"

namespace zx {

enum class BuiltinShape : u8
{
    Cube,     // 1 x 1 x 1, centered
    Sphere,   // diameter 1
    Plane,    // 10 x 10 m, facing +Y
    Cylinder, // diameter 1, height 1, centered
    Count
};

namespace builtin {

asset::AssetId Mesh(BuiltinShape shape);
StringView ShapeName(BuiltinShape shape);
/// True for the GUID of a built-in mesh.
bool IsBuiltin(const asset::AssetId& id);

} // namespace builtin

void RegisterBuiltinAssets(asset::AssetManager& assets);

/// The procedural geometry (exposed for tests).
asset::MeshData GenerateShapeMesh(BuiltinShape shape);

} // namespace zx
