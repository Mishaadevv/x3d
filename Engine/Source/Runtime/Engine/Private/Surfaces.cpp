#include "Engine/Surfaces.h"
#include "Engine/SceneRendering.h"
#include "Engine/Terrain.h"

namespace zx {

StringView SurfaceFromName(StringView name)
{
    const String n = String(name).ToLower();
    struct Rule
    {
        const char* word;
        const char* surface;
    };
    static const Rule Rules[] = {
        {"grass", "Grass"}, {"terrain", "Grass"}, {"leaves", "Grass"}, {"leaf", "Grass"}, {"moss", "Grass"},   {"dirt", "Grass"},
        {"mud", "Grass"},   {"wood", "Wood"},     {"plank", "Wood"},   {"hay", "Wood"},    {"bark", "Wood"},    {"crate", "Wood"},
        {"snow", "Snow"},   {"ice", "Snow"},      {"sand", "Sand"},    {"carpet", "Carpet"}, {"cloth", "Carpet"}, {"fabric", "Carpet"},
        {"rubber", "Carpet"}, {"metal", "Metal"}, {"steel", "Metal"},  {"iron", "Metal"},  {"gold", "Metal"},   {"water", "Water"},
    };
    for (const Rule& r : Rules)
        if (n.Contains(r.word))
            return r.surface;
    return "Concrete";
}

StringView SurfaceOf(const ecs::World& world, ecs::Entity e, const Function<String(asset::AssetId)>& materialName)
{
    if (!world.IsAlive(e))
        return "Concrete";
    if (world.Get<const Terrain>(e))
        return "Grass";
    if (world.Get<const Water>(e))
        return "Water";
    if (const MeshRenderer* mr = world.Get<const MeshRenderer>(e); mr && !mr->materials.Empty() && materialName) {
        const String name = materialName(mr->materials[0]);
        if (!name.Empty())
            return SurfaceFromName(name);
    }
    return "Concrete";
}

StringView FootstepSurface(StringView surface)
{
    if (surface == "Grass" || surface == "Wood" || surface == "Snow" || surface == "Carpet")
        return surface;
    if (surface == "Sand")
        return "Snow"; // a soft crunch
    return "Concrete";
}

} // namespace zx
