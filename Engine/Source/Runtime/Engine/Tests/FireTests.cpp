// Spreading fire: heat ignites neighbours (not distant objects), flames creep over the surface and burn out, Crumble
// removes the object, water puts fires out, fire Particle Emitters light what is next to them.

#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Core/Testing/Test.h"
#include "Engine/Fire.h"
#include "Engine/Particles.h"
#include "Engine/PhysicsSystem.h"
#include "Engine/SceneRendering.h"
#include "Engine/Scripting.h"
#include "Scene/Hierarchy.h"

using namespace zx;

namespace {

ecs::Entity AddBox(ecs::World& world, StringView name, Vec3 position, Vec3 size, const Flammable& f)
{
    Transform t(position);
    t.scale = size;
    const ecs::Entity e = scene::CreateNode(world, name, t);
    world.Add(e, BoxCollider{});
    world.Add(e, f);
    return e;
}

void Run(FireSystem& fire, ParticleSystem& particles, ecs::World& world, f32 seconds)
{
    for (u32 i = 0; i < u32(seconds * 30.0f); ++i) {
        scene::UpdateWorldTransforms(world);
        fire.Tick(world, 1.0f / 30.0f, particles);
        particles.Tick(world, 1.0f / 30.0f, true);
    }
}

FireState StateOf(ecs::World& world, ecs::Entity e)
{
    const Flammable* f = world.IsAlive(e) ? world.Get<const Flammable>(e) : nullptr;
    return f ? f->state : FireState::None;
}

} // namespace

ZX_TEST(Fire, SpreadsToNeighboursOnly)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    Flammable wood;
    const ecs::Entity a = AddBox(world, "A", Vec3(0, 0.5f, 0), Vec3(1.0f), wood);
    const ecs::Entity b = AddBox(world, "B", Vec3(1.05f, 0.5f, 0), Vec3(1.0f), wood); // 5 cm gap
    const ecs::Entity far = AddBox(world, "Far", Vec3(6.0f, 0.5f, 0), Vec3(1.0f), wood);
    scene::UpdateWorldTransforms(world);
    FireSystem fire;
    ParticleSystem particles;
    ZX_REQUIRE(fire.Ignite(world, a));
    ZX_EXPECT(StateOf(world, a) == FireState::Burning);
    Run(fire, particles, world, 12.0f);
    ZX_EXPECT(StateOf(world, b) == FireState::Burning);
    ZX_EXPECT(StateOf(world, far) == FireState::None);
    ZX_EXPECT(world.Get<const Flammable>(far)->temperature < 100.0f);
    // Flames, smoke and a light while burning; the burnt area grows.
    ZX_EXPECT(particles.ParticleCount() > 50u);
    ZX_EXPECT(!fire.Lights().Empty());
    ZX_EXPECT(world.Get<const Flammable>(a)->charRadius > 0.5f);
    ZX_EXPECT(fire.BurningCount() == 2u);
}

ZX_TEST(Fire, BurnsOutAndCrumbles)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    Flammable straw;
    straw.burnTime = 1.0f;
    straw.spreadSpeed = 1.0f;
    straw.heat = 0.0f; // doesn't light the other one
    const ecs::Entity charred = AddBox(world, "Charred", Vec3(0, 0.5f, 0), Vec3(1.0f), straw);
    straw.whenBurnt = BurntResult::Crumble;
    const ecs::Entity ash = AddBox(world, "Ash", Vec3(5, 0.5f, 0), Vec3(1.0f), straw);
    scene::UpdateWorldTransforms(world);
    FireSystem fire;
    ParticleSystem particles;
    fire.Ignite(world, charred);
    fire.Ignite(world, ash);
    Run(fire, particles, world, 1.0f);
    ZX_EXPECT(world.Get<const Flammable>(charred)->intensity > 0.2f);
    Run(fire, particles, world, 3.0f);
    ZX_EXPECT(StateOf(world, charred) == FireState::Burnt);
    ZX_EXPECT(world.Get<const Flammable>(charred)->intensity == 0.0f);
    ZX_EXPECT(!fire.Ignite(world, charred)); // nothing left to burn
    Run(fire, particles, world, 3.0f);
    ZX_EXPECT(world.IsAlive(charred));
    ZX_EXPECT(!world.IsAlive(ash));
}

ZX_TEST(Fire, WaterAndExtinguish)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    Flammable wood;
    wood.heat = 0.0f;
    const ecs::Entity dry = AddBox(world, "Dry", Vec3(0, 0.5f, 0), Vec3(1.0f), wood);
    const ecs::Entity sunk = AddBox(world, "Sunk", Vec3(10, -1.0f, 0), Vec3(1.0f), wood);
    const ecs::Entity pond = scene::CreateNode(world, "Pond", Transform(Vec3(10, 0, 0)));
    world.Add(pond, Water{});
    scene::UpdateWorldTransforms(world);
    FireSystem fire;
    ParticleSystem particles;
    fire.Ignite(world, dry);
    fire.Ignite(world, sunk);
    Run(fire, particles, world, 0.5f);
    ZX_EXPECT(StateOf(world, dry) == FireState::Burning);
    ZX_EXPECT(StateOf(world, sunk) == FireState::None); // put out under the water
    ZX_EXPECT_EQ(fire.ExtinguishAt(world, Vec3(0, 1.2f, 0), 1.0f), 1u);
    ZX_EXPECT(StateOf(world, dry) == FireState::None);
    ZX_EXPECT(world.Get<const Flammable>(dry)->charRadius > 0.0f); // stays charred
    ZX_EXPECT(fire.Ignite(world, dry));                            // and can burn again
}

ZX_TEST(Fire, CampfireLightsWhatTouchesIt)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    ParticleEmitter flames;
    flames.look = ParticleLook::Flame;
    flames.rate = 45.0f;
    flames.radius = 0.22f;
    flames.startSize = 0.55f;
    flames.speed = 1.0f;
    flames.lifetime = 0.8f;
    const ecs::Entity campfire = scene::CreateNode(world, "Campfire", Transform(Vec3()));
    world.Add(campfire, flames);
    Flammable wood;
    const ecs::Entity touching = AddBox(world, "Touching", Vec3(0.55f, 0.3f, 0), Vec3(0.6f), wood);
    const ecs::Entity away = AddBox(world, "Away", Vec3(3.0f, 0.3f, 0), Vec3(0.6f), wood);
    scene::UpdateWorldTransforms(world);
    FireSystem fire;
    ParticleSystem particles;
    Run(fire, particles, world, 10.0f);
    ZX_EXPECT(StateOf(world, touching) == FireState::Burning);
    ZX_EXPECT(StateOf(world, away) == FireState::None);
}

ZX_TEST(Fire, ScriptApi)
{
    const String root = Path::Join(Path::Normalize(platform::GetTempDirectory()), "zx_script_test/fire");
    platform::DeleteDirectoryTree(root);
    platform::CreateDirectoryTree(root + "/Scripts");
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/Lighter.zs", R"zs(
        script Lighter {
            var step = 0
            fn check(ok, what) { if not ok { log("FAIL " + what) } }
            fn update(dt) {
                step += 1
                let crate = find("Crate")
                if step == 1 { check(not crate.burning, "cold"); check(crate.ignite(), "ignite") }
                if step == 2 { check(crate.burning and crate.temperature > 500, "burning"); crate.extinguish() }
                if step == 3 { check(not crate.burning, "out"); check(fire.ignite(vec3(0, 0.5, 0), 1.0) == 1, "fire.ignite") }
                if step == 4 { check(fire.burning == 1, "count"); check(fire.extinguish(vec3(0, 0.5, 0), 1.0) == 1, "fire.extinguish"); log("done") }
            }
        }
    )zs"));
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    AddBox(world, "Crate", Vec3(0, 0.5f, 0), Vec3(1.0f), Flammable{});
    world.Create(scene::EntityName{String("Lighter")}, Transform::Identity(), Script{String("Scripts/Lighter.zs"), String()});
    scene::UpdateWorldTransforms(world);
    FireSystem fire;
    ParticleSystem particles;
    ScriptSystem scripts;
    scripts.SetContentRoot(root);
    scripts.fire = &fire;
    String log;
    scripts.onLog = [&](StringView line, bool) {
        log += line;
        log += '\n';
    };
    scripts.Begin(world);
    for (u32 i = 0; i < 4; ++i) {
        scripts.Tick(world, 1.0f / 30.0f, nullptr);
        fire.Tick(world, 1.0f / 30.0f, particles);
    }
    scripts.End();
    if (log.Contains("FAIL") || !log.Contains("done"))
        test::ReportFailure(__FILE__, __LINE__, log.View());
}
