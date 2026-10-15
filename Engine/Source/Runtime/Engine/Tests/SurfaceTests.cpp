// Surface types for footsteps: from material names, Terrain and Water components, and the footstep sets they map to.

#include "Core/Testing/Test.h"
#include "Engine/SceneRendering.h"
#include "Engine/Surfaces.h"
#include "Engine/Terrain.h"

using namespace zx;

ZX_TEST(Surfaces, NamesTerrainWaterAndFootsteps)
{
    ZX_EXPECT(SurfaceFromName("M_Grass") == "Grass");
    ZX_EXPECT(SurfaceFromName("M_Terrain") == "Grass");
    ZX_EXPECT(SurfaceFromName("M_Wood") == "Wood");
    ZX_EXPECT(SurfaceFromName("M_Hay") == "Wood");
    ZX_EXPECT(SurfaceFromName("M_Metal_Steel") == "Metal");
    ZX_EXPECT(SurfaceFromName("M_Sand") == "Sand");
    ZX_EXPECT(SurfaceFromName("M_Proto_Grey") == "Concrete");
    ZX_EXPECT(FootstepSurface("Sand") == "Snow");
    ZX_EXPECT(FootstepSurface("Metal") == "Concrete");
    ZX_EXPECT(FootstepSurface("Wood") == "Wood");

    ecs::World world;
    const ecs::Entity terrain = world.Create(Transform::Identity());
    world.Add(terrain, Terrain{});
    const ecs::Entity sea = world.Create(Transform::Identity());
    world.Add(sea, Water{});
    const Guid wood = Guid::New();
    const ecs::Entity deck = world.Create(Transform::Identity());
    MeshRenderer mr;
    mr.materials.PushBack(wood);
    world.Add(deck, mr);
    const Function<String(asset::AssetId)> names = [&](asset::AssetId id) { return id == wood ? String("M_Wood") : String(); };
    ZX_EXPECT(SurfaceOf(world, terrain, names) == "Grass");
    ZX_EXPECT(SurfaceOf(world, sea, names) == "Water");
    ZX_EXPECT(SurfaceOf(world, deck, names) == "Wood");
    ZX_EXPECT(SurfaceOf(world, deck, nullptr) == "Concrete");
    ZX_EXPECT(SurfaceOf(world, ecs::NullEntity, names) == "Concrete");
}
