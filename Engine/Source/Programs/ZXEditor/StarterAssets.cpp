// Starter Content prefabs: ready-made, fully editable assets written into new projects (Content/StarterContent/
// Prefabs) - particle effects, characters and a tank, gameplay props built on physics joints. Each one is assembled
// in a scratch world from built-in shapes, Starter Content materials, scripts and sounds, then saved as a .zprefab.

#include "Editor.h"

#include "Core/Log/Log.h"
#include "Engine/AudioSystem.h"
#include "Engine/BuiltinAssets.h"
#include "Engine/Fire.h"
#include "Engine/Gameplay.h"
#include "Engine/Particles.h"
#include "Engine/PhysicsSystem.h"
#include "Engine/Prefab.h"
#include "Engine/SceneRendering.h"
#include "Engine/SoftBody.h"
#include "Engine/Scripting.h"
#include "Scene/Hierarchy.h"

namespace zx::editor {

ZX_DEFINE_LOG_CATEGORY_STATIC(LogStarter, Info);

namespace {

/// Scratch world that builds one prefab.
struct PrefabBuilder
{
    ecs::World world;
    PrefabBuilder() { scene::InstallHierarchyHooks(world); }

    ecs::Entity Node(StringView name, Vec3 position = {}, ecs::Entity parent = {}, Quat rotation = {}, Vec3 scale = Vec3(1.0f))
    {
        Transform t(position, rotation);
        t.scale = scale;
        const ecs::Entity e = scene::CreateNode(world, name, t, parent);
        EnsureEntityGuid(world, e);
        return e;
    }
    ecs::Entity Mesh(StringView name, BuiltinShape shape, Vec3 position, Vec3 size, StringView material, ecs::Entity parent = {},
                     Quat rotation = {})
    {
        const ecs::Entity e = Node(name, position, parent, rotation, size);
        MeshRenderer mr;
        mr.mesh = builtin::Mesh(shape);
        mr.materials.PushBack(Guid::FromName(Guid(), material)); // Starter Content materials have stable guids
        world.Add(e, mr);
        return e;
    }
    void Script(ecs::Entity e, StringView file, StringView properties = {})
    {
        world.Add(e, zx::Script{String::Format("StarterContent/Scripts/{}", file), String(properties)});
    }
    void Body(ecs::Entity e, f32 mass, bool box = true, StringView impactSound = "Impact_Wood_Medium")
    {
        RigidBody rb;
        rb.mass = mass;
        world.Add(e, rb);
        if (box)
            world.Add(e, BoxCollider{});
        Impact(e, impactSound);
    }
    void Impact(ecs::Entity e, StringView sound)
    {
        if (sound.Empty())
            return;
        ImpactSound s;
        s.sound = String::Format("StarterContent/Sounds/Impacts/{}", sound);
        world.Add(e, s);
    }
};

// --- Particle presets ------------------------------------------------------------------------------------------------

ParticleEmitter Flames()
{
    ParticleEmitter p;
    p.rate = 45.0f;
    p.maxParticles = 160;
    p.lifetime = 0.8f;
    p.shape = EmitterShape::Cone;
    p.radius = 0.22f;
    p.coneAngle = 0.2f;
    p.speed = 1.0f;
    p.acceleration = Vec3(0.0f, 2.2f, 0.0f);
    p.drag = 1.0f;
    p.turbulence = 1.2f;
    p.startSize = 0.55f;
    p.endSize = 0.12f;
    p.spin = 1.0f;
    p.startColor = Vec3(1.0f, 0.62f, 0.22f);
    p.endColor = Vec3(0.9f, 0.2f, 0.03f);
    p.fadeIn = 0.15f;
    p.look = ParticleLook::Flame;
    p.blend = ParticleBlend::Additive;
    p.intensity = 4.0f;
    p.flipbook = "StarterContent/Flipbooks/T_Flipbook_Fire.png"; // baked from a fluid simulation
    p.flipbookFps = 24.0f;
    p.startSize = 0.8f;
    p.endSize = 0.35f;
    p.spin = 0.0f;
    p.lightFlicker = 0.35f;
    return p;
}

ParticleEmitter Smoke(f32 density = 1.0f)
{
    ParticleEmitter p;
    p.rate = 7.0f * density;
    p.maxParticles = 80;
    p.lifetime = 4.5f;
    p.radius = 0.3f;
    p.coneAngle = 0.25f;
    p.speed = 0.7f;
    p.acceleration = Vec3(0.0f, 0.5f, 0.0f);
    p.drag = 0.4f;
    p.turbulence = 0.7f;
    p.startSize = 0.6f;
    p.endSize = 2.8f;
    p.spin = 0.3f;
    p.startColor = Vec3(0.12f);
    p.endColor = Vec3(0.3f);
    p.startAlpha = 0.55f;
    p.fadeIn = 0.25f;
    p.look = ParticleLook::Soft;
    p.blend = ParticleBlend::Alpha;
    p.emissive = false;
    p.flipbook = "StarterContent/Flipbooks/T_Flipbook_Smoke.png"; // a billowing puff over each particle's life
    return p;
}

ParticleEmitter Embers()
{
    ParticleEmitter p;
    p.rate = 5.0f;
    p.maxParticles = 30;
    p.lifetime = 1.8f;
    p.radius = 0.25f;
    p.coneAngle = 0.6f;
    p.speed = 1.8f;
    p.acceleration = Vec3(0.0f, 1.0f, 0.0f);
    p.drag = 0.6f;
    p.turbulence = 4.0f;
    p.startSize = 0.05f;
    p.endSize = 0.02f;
    p.startColor = Vec3(1.0f, 0.55f, 0.15f);
    p.endColor = Vec3(1.0f, 0.25f, 0.05f);
    p.fadeIn = 0.0f;
    p.look = ParticleLook::Spark;
    p.intensity = 4.0f;
    return p;
}

ParticleEmitter HeatHaze(f32 size = 1.0f)
{
    ParticleEmitter p;
    p.look = ParticleLook::Heat;
    p.emissive = false;
    p.rate = 10.0f;
    p.maxParticles = 40;
    p.lifetime = 1.3f;
    p.radius = 0.25f * size;
    p.coneAngle = 0.2f;
    p.speed = 0.8f;
    p.acceleration = Vec3(0.0f, 0.6f, 0.0f);
    p.drag = 0.8f;
    p.turbulence = 0.4f;
    p.startSize = 1.0f * size;
    p.endSize = 1.8f * size;
    p.startAlpha = 1.0f;
    p.endAlpha = 0.0f;
    p.fadeIn = 0.25f;
    return p;
}

ParticleEmitter Sparks()
{
    ParticleEmitter p;
    p.rate = 60.0f;
    p.maxParticles = 200;
    p.lifetime = 1.2f;
    p.radius = 0.05f;
    p.coneAngle = 0.9f;
    p.speed = 5.0f;
    p.speedVariation = 0.4f;
    p.acceleration = Vec3(0.0f, -9.8f, 0.0f);
    p.drag = 0.2f;
    p.turbulence = 0.0f;
    p.startSize = 0.05f;
    p.endSize = 0.03f;
    p.spin = 0.0f;
    p.startColor = Vec3(1.0f, 0.8f, 0.4f);
    p.endColor = Vec3(1.0f, 0.35f, 0.05f);
    p.fadeIn = 0.0f;
    p.look = ParticleLook::Spark;
    p.intensity = 5.0f;
    p.collision = ParticleCollision::Bounce; // they skitter over the floor
    p.bounciness = 0.4f;
    return p;
}

ParticleEmitter Steam()
{
    ParticleEmitter p = Smoke();
    p.rate = 12.0f;
    p.lifetime = 2.5f;
    p.radius = 0.15f;
    p.coneAngle = 0.2f;
    p.speed = 2.0f;
    p.acceleration = Vec3(0.0f, 0.3f, 0.0f);
    p.drag = 1.2f;
    p.turbulence = 0.8f;
    p.startSize = 0.3f;
    p.endSize = 1.8f;
    p.startColor = Vec3(0.9f, 0.9f, 0.92f);
    p.endColor = Vec3(0.95f);
    p.startAlpha = 0.45f;
    return p;
}

// --- Fire presets ----------------------------------------------------------------------------------------------------

Flammable Wood(f32 spreadSpeed = 0.12f, f32 burnTime = 20.0f)
{
    Flammable f;
    f.spreadSpeed = spreadSpeed;
    f.burnTime = burnTime;
    f.sound = "StarterContent/Sounds/Fire_Loop.ogg";
    return f;
}

Flammable Hay()
{
    Flammable f = Wood(0.45f, 7.0f);
    f.ignitionTemperature = 200.0f;
    f.resistance = 0.6f;
    f.heat = 1.5f;
    f.heatRange = 1.0f;
    f.flameHeight = 1.0f;
    f.flameDensity = 1.3f;
    f.smoke = 1.6f;
    f.smokeColor = Vec3(0.16f, 0.15f, 0.13f);
    f.whenBurnt = BurntResult::Crumble;
    return f;
}

} // namespace

u32 Editor::WriteStarterPrefabs(const NewProjectOptions& options)
{
    if (!prefabs)
        return 0;
    u32 written = 0;
    auto save = [&](PrefabBuilder& b, ecs::Entity root, StringView path) {
        const String rel = String::Format("StarterContent/Prefabs/{}.zprefab", path);
        if (CreatePrefabFromEntity(b.world, root, rel, *prefabs, assets.Get()))
            ++written;
        else
            ZX_LOG_WARN(LogStarter, "Starter Content: could not write {}", rel);
    };

    if (options.starterEffects) {
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Fire");
            b.world.Add(root, Flames());
            PointLight light;
            light.color = Vec3(1.0f, 0.62f, 0.3f);
            light.lumens = 1600.0f;
            light.range = 9.0f;
            b.world.Add(root, light);
            const ecs::Entity smoke = b.Node("Smoke", Vec3(0, 0.9f, 0), root);
            b.world.Add(smoke, Smoke());
            const ecs::Entity embers = b.Node("Embers", Vec3(0, 0.2f, 0), root);
            b.world.Add(embers, Embers());
            b.world.Add(b.Node("Heat Haze", Vec3(0, 0.9f, 0), root), HeatHaze());
            save(b, root, "Effects/P_Fire");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Campfire");
            const ecs::Entity flames = b.Node("Flames", Vec3(0, 0.15f, 0), root);
            b.world.Add(flames, Flames());
            PointLight light;
            light.color = Vec3(1.0f, 0.6f, 0.28f);
            light.lumens = 2400.0f;
            light.range = 11.0f;
            light.castShadows = true;
            b.world.Add(flames, light);
            b.world.Add(b.Node("Smoke", Vec3(0, 1.0f, 0), root), Smoke());
            b.world.Add(b.Node("Embers", Vec3(0, 0.3f, 0), root), Embers());
            b.world.Add(b.Node("Heat Haze", Vec3(0, 1.0f, 0), root), HeatHaze(1.2f));
            for (u32 i = 0; i < 4; ++i) {
                const Quat q = Quat::FromAxisAngle(Vec3(0, 1, 0), DegToRad(f32(i) * 45.0f)) * Quat::FromAxisAngle(Vec3(0, 0, 1), DegToRad(78.0f));
                b.Mesh(String::Format("Log {}", i + 1), BuiltinShape::Cylinder, Vec3(0, 0.12f, 0), Vec3(0.16f, 1.1f, 0.16f), "M_Wood", root, q);
            }
            for (u32 i = 0; i < 9; ++i) {
                const f32 a = TwoPi * f32(i) / 9.0f;
                b.Mesh(String::Format("Stone {}", i + 1), BuiltinShape::Sphere, Vec3(Cos(a) * 0.75f, 0.06f, Sin(a) * 0.75f),
                       Vec3(0.32f, 0.2f, 0.26f), "M_Rock", root, Quat::FromAxisAngle(Vec3(0, 1, 0), a));
            }
            AudioSource crackle;
            crackle.clip = "StarterContent/Sounds/Fire_Loop.ogg";
            crackle.volume = 0.7f;
            crackle.maxDistance = 18.0f;
            b.world.Add(root, crackle);
            save(b, root, "Effects/P_Campfire");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Torch");
            b.Mesh("Handle", BuiltinShape::Cylinder, Vec3(0, 0.4f, 0), Vec3(0.07f, 0.8f, 0.07f), "M_Wood", root);
            b.Mesh("Head", BuiltinShape::Cylinder, Vec3(0, 0.85f, 0), Vec3(0.12f, 0.14f, 0.12f), "M_Metal_Steel", root);
            const ecs::Entity flame = b.Node("Flame", Vec3(0, 0.95f, 0), root);
            ParticleEmitter f = Flames();
            f.rate = 30.0f;
            f.radius = 0.06f;
            f.startSize = 0.28f;
            f.endSize = 0.06f;
            f.lifetime = 0.55f;
            f.acceleration = Vec3(0.0f, 1.6f, 0.0f);
            b.world.Add(flame, f);
            PointLight light;
            light.color = Vec3(1.0f, 0.6f, 0.3f);
            light.lumens = 700.0f;
            light.range = 6.0f;
            b.world.Add(flame, light);
            save(b, root, "Effects/P_Torch");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Smoke Column");
            ParticleEmitter s = Smoke(2.0f);
            s.startColor = Vec3(0.08f);
            s.endColor = Vec3(0.22f);
            s.startSize = 1.0f;
            s.endSize = 5.0f;
            s.lifetime = 8.0f;
            s.speed = 1.2f;
            s.maxParticles = 160;
            b.world.Add(root, s);
            save(b, root, "Effects/P_Smoke");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Sparks");
            b.world.Add(root, Sparks());
            PointLight light;
            light.color = Vec3(1.0f, 0.75f, 0.45f);
            light.lumens = 500.0f;
            light.range = 5.0f;
            b.world.Add(root, light);
            save(b, root, "Effects/P_Sparks");
        }
        {
            // A welding / grinder spark shower: tens of thousands of sparks, simulated on the GPU, skittering off the
            // floor and walls the camera sees.
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Spark Shower");
            ParticleEmitter p = Sparks();
            p.gpuSimulation = true;
            p.rate = 9000.0f;
            p.lifetime = 2.2f;
            p.lifetimeVariation = 0.35f;
            p.coneAngle = 0.55f;
            p.speed = 6.5f;
            p.speedVariation = 0.45f;
            p.drag = 0.15f;
            p.startSize = 0.035f;
            p.endSize = 0.015f;
            p.intensity = 3.0f;
            p.endAlpha = 0.0f;
            p.bounciness = 0.45f;
            b.world.Add(root, p);
            PointLight light;
            light.color = Vec3(1.0f, 0.7f, 0.4f);
            light.lumens = 900.0f;
            light.range = 6.0f;
            b.world.Add(root, light);
            AudioSource crackle; // a fast, bright crackle: the sizzle of the shower
            crackle.clip = "StarterContent/Sounds/Fire_Crackle.ogg";
            crackle.volume = 0.4f;
            crackle.pitch = 1.9f;
            crackle.maxDistance = 16.0f;
            b.world.Add(root, crackle);
            save(b, root, "Effects/P_SparkShower");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Steam Vent");
            b.Mesh("Grate", BuiltinShape::Cylinder, Vec3(0, 0.03f, 0), Vec3(0.6f, 0.06f, 0.6f), "M_Metal_Steel", root);
            b.world.Add(b.Node("Steam", Vec3(0, 0.1f, 0), root), Steam());
            AudioSource hiss;
            hiss.clip = "StarterContent/Sounds/Steam_Loop.ogg";
            hiss.volume = 0.35f;
            hiss.maxDistance = 14.0f;
            b.world.Add(root, hiss);
            save(b, root, "Effects/P_Steam");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Ground Fog");
            ParticleEmitter fog;
            fog.shape = EmitterShape::Box;
            fog.boxSize = Vec3(16.0f, 0.5f, 16.0f);
            fog.rate = 3.0f;
            fog.burst = 30;
            fog.maxParticles = 70;
            fog.lifetime = 14.0f;
            fog.speed = 0.15f;
            fog.speedVariation = 1.0f;
            fog.acceleration = Vec3();
            fog.drag = 0.0f;
            fog.turbulence = 0.15f;
            fog.startSize = 6.0f;
            fog.endSize = 9.0f;
            fog.spin = 0.05f;
            fog.startColor = Vec3(0.85f, 0.88f, 0.92f);
            fog.endColor = Vec3(0.85f, 0.88f, 0.92f);
            fog.startAlpha = 0.14f;
            fog.fadeIn = 0.3f;
            fog.look = ParticleLook::Soft;
            fog.blend = ParticleBlend::Alpha;
            fog.emissive = false;
            b.world.Add(b.Node("Fog", Vec3(0, 0.4f, 0), root), fog);
            save(b, root, "Effects/P_GroundFog");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Magic Orb");
            ParticleEmitter m;
            m.shape = EmitterShape::Sphere;
            m.radius = 0.6f;
            m.rate = 40.0f;
            m.lifetime = 1.6f;
            m.speed = 0.4f;
            m.acceleration = Vec3(0.0f, 0.6f, 0.0f);
            m.turbulence = 2.5f;
            m.startSize = 0.18f;
            m.endSize = 0.0f;
            m.startColor = Vec3(0.3f, 0.6f, 1.0f);
            m.endColor = Vec3(0.8f, 0.3f, 1.0f);
            m.intensity = 3.0f;
            m.lightFlicker = 0.15f;
            b.world.Add(root, m);
            PointLight light;
            light.color = Vec3(0.5f, 0.6f, 1.0f);
            light.lumens = 600.0f;
            light.range = 6.0f;
            b.world.Add(root, light);
            b.Mesh("Core", BuiltinShape::Sphere, Vec3(), Vec3(0.3f), "M_Emissive_Cyan", root);
            AudioSource hum;
            hum.clip = "StarterContent/Sounds/Props/Machine_Hum_Loop.ogg";
            hum.volume = 0.25f;
            hum.pitch = 1.4f;
            hum.maxDistance = 10.0f;
            b.world.Add(root, hum);
            save(b, root, "Effects/P_Magic");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Rain");
            // Emits along its +Y: turned upside down so the drops fall.
            const ecs::Entity cloud = b.Node("Rain Cloud", Vec3(0, 15, 0), root, Quat::FromAxisAngle(Vec3(1, 0, 0), Pi));
            ParticleEmitter r;
            r.shape = EmitterShape::Box;
            r.boxSize = Vec3(30.0f, 0.1f, 30.0f);
            r.rate = 1500.0f;
            r.maxParticles = 3000;
            r.lifetime = 1.4f;
            r.lifetimeVariation = 0.1f;
            r.speed = 12.0f;
            r.speedVariation = 0.1f;
            r.acceleration = Vec3(0.0f, -4.0f, 0.0f);
            r.drag = 0.0f;
            r.turbulence = 0.0f;
            r.startSize = 0.03f;
            r.endSize = 0.03f;
            r.sizeVariation = 0.2f;
            r.spin = 0.0f;
            r.startColor = Vec3(0.7f, 0.75f, 0.8f);
            r.endColor = Vec3(0.7f, 0.75f, 0.8f);
            r.startAlpha = 0.5f;
            r.endAlpha = 0.5f;
            r.fadeIn = 0.05f;
            r.look = ParticleLook::Spark;
            r.blend = ParticleBlend::Alpha;
            r.emissive = false;
            r.collision = ParticleCollision::Die; // drops end on whatever they hit...
            r.onHitEmitter = "Rain Splash";      // ...in a little splash
            r.onHitCount = 2;
            b.world.Add(cloud, r);
            ParticleEmitter splash;
            splash.rate = 0.0f;
            splash.shape = EmitterShape::Cone;
            splash.radius = 0.02f;
            splash.coneAngle = 0.9f;
            splash.speed = 1.6f;
            splash.speedVariation = 0.5f;
            splash.acceleration = Vec3(0.0f, -9.8f, 0.0f);
            splash.drag = 0.5f;
            splash.turbulence = 0.0f;
            splash.lifetime = 0.3f;
            splash.startSize = 0.025f;
            splash.endSize = 0.015f;
            splash.startColor = splash.endColor = Vec3(0.75f, 0.8f, 0.85f);
            splash.startAlpha = 0.6f;
            splash.fadeIn = 0.0f;
            splash.maxParticles = 1500;
            splash.look = ParticleLook::Spark;
            splash.blend = ParticleBlend::Alpha;
            splash.emissive = false;
            b.world.Add(b.Node("Rain Splash", Vec3(), root), splash);
            AudioSource rain;
            rain.clip = "StarterContent/Sounds/Rain_Loop.ogg";
            rain.spatial = false; // all around the listener
            rain.volume = 0.6f;
            b.world.Add(root, rain);
            save(b, root, "Effects/P_Rain");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Fountain");
            b.Mesh("Basin Floor", BuiltinShape::Cylinder, Vec3(0, 0.1f, 0), Vec3(4.0f, 0.2f, 4.0f), "M_Concrete", root);
            for (u32 i = 0; i < 12; ++i) {
                const f32 a = TwoPi * f32(i) / 12.0f;
                b.Mesh(String::Format("Rim {}", i + 1), BuiltinShape::Cube, Vec3(Cos(a) * 2.0f, 0.35f, Sin(a) * 2.0f), Vec3(0.3f, 0.5f, 1.12f),
                       "M_Proto_Light", root, Quat::FromAxisAngle(Vec3(0, 1, 0), -a));
            }
            b.Mesh("Pillar", BuiltinShape::Cylinder, Vec3(0, 0.6f, 0), Vec3(0.35f, 1.0f, 0.35f), "M_Proto_Light", root);
            const ecs::Entity basinWater = b.Node("Water", Vec3(0, 0.45f, 0), root);
            Water w;
            ApplyWaterPreset(w, WaterPreset::Pool);
            w.size = Vec2(3.7f, 3.7f);
            w.waveAmount = 2.5f; // stirred by the jet
            w.shoreFoam = 0.25f;
            b.world.Add(basinWater, w);
            ParticleEmitter jet;
            jet.radius = 0.05f;
            jet.coneAngle = 0.08f;
            jet.speed = 6.5f;
            jet.speedVariation = 0.1f;
            jet.acceleration = Vec3(0.0f, -9.8f, 0.0f);
            jet.drag = 0.1f;
            jet.turbulence = 0.3f;
            jet.rate = 120.0f;
            jet.maxParticles = 300;
            jet.lifetime = 1.5f;
            jet.startSize = 0.12f;
            jet.endSize = 0.35f;
            jet.startColor = Vec3(0.75f, 0.85f, 0.95f);
            jet.endColor = Vec3(0.8f, 0.9f, 1.0f);
            jet.startAlpha = 0.6f;
            jet.look = ParticleLook::Soft;
            jet.blend = ParticleBlend::Alpha;
            jet.emissive = false;
            b.world.Add(b.Node("Jet", Vec3(0, 1.1f, 0), root), jet);
            AudioSource splash;
            splash.clip = "StarterContent/Sounds/Water_Stream_Loop.ogg"; // falling water
            splash.volume = 0.55f;
            splash.maxDistance = 14.0f;
            b.world.Add(root, splash);
            save(b, root, "Effects/P_Fountain");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Water Pool");
            b.Mesh("Pool Floor", BuiltinShape::Cube, Vec3(0, -1.2f, 0), Vec3(8.0f, 0.2f, 8.0f), "M_Proto_Teal", root);
            const Vec3 walls[4][2] = {{Vec3(0, -0.5f, 4.15f), Vec3(8.6f, 1.6f, 0.3f)},
                                      {Vec3(0, -0.5f, -4.15f), Vec3(8.6f, 1.6f, 0.3f)},
                                      {Vec3(4.15f, -0.5f, 0), Vec3(0.3f, 1.6f, 8.0f)},
                                      {Vec3(-4.15f, -0.5f, 0), Vec3(0.3f, 1.6f, 8.0f)}};
            for (u32 i = 0; i < 4; ++i)
                b.Mesh(String::Format("Wall {}", i + 1), BuiltinShape::Cube, walls[i][0], walls[i][1], "M_Proto_Light", root);
            const ecs::Entity water = b.Node("Water", Vec3(0, 0.1f, 0), root);
            Water w;
            ApplyWaterPreset(w, WaterPreset::Pool);
            w.size = Vec2(8.0f, 8.0f);
            b.world.Add(water, w);
            save(b, root, "Effects/P_WaterPool");
        }
        {
            // Simulated (GPU fluid) fire and smoke: real buoyant, swirling gas instead of particles.
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Fluid Fire");
            b.world.Add(b.Node("Simulation", Vec3(0, 2.0f, 0), root, {}, Vec3(2.0f, 4.0f, 2.0f)), FluidVolume{});
            PointLight light;
            light.color = Vec3(1.0f, 0.55f, 0.25f);
            light.lumens = 2500.0f;
            light.range = 10.0f;
            b.world.Add(b.Node("Glow", Vec3(0, 0.6f, 0), root), light);
            save(b, root, "Effects/P_FluidFire");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Fluid Smoke");
            FluidVolume smoke;
            smoke.heat = 0.15f;
            smoke.fireIntensity = 0.0f;
            smoke.smoke = 2.5f;
            smoke.smokeColor = Vec3(0.35f);
            smoke.buoyancy = 2.5f;
            smoke.smokeFade = 0.15f;
            b.world.Add(b.Node("Simulation", Vec3(0, 3.0f, 0), root, {}, Vec3(3.0f, 6.0f, 3.0f)), smoke);
            save(b, root, "Effects/P_FluidSmoke");
        }
    }

    if (options.starterCharacters) {
        auto mannequin = [](PrefabBuilder& b, ecs::Entity player) {
            b.Mesh("Body", BuiltinShape::Cylinder, Vec3(0, 0.95f, 0), Vec3(0.55f, 1.1f, 0.4f), "M_Proto_Light", player);
            b.Mesh("Head", BuiltinShape::Sphere, Vec3(0, 1.7f, 0), Vec3(0.36f), "M_Proto_Light", player);
            b.Mesh("Visor", BuiltinShape::Cube, Vec3(0, 1.72f, 0.15f), Vec3(0.26f, 0.09f, 0.08f), "M_Emissive_Cyan", player);
            b.Mesh("Arm L", BuiltinShape::Cylinder, Vec3(-0.36f, 1.05f, 0), Vec3(0.14f, 0.7f, 0.14f), "M_Proto_Grey", player);
            b.Mesh("Arm R", BuiltinShape::Cylinder, Vec3(0.36f, 1.05f, 0), Vec3(0.14f, 0.7f, 0.14f), "M_Proto_Grey", player);
            b.Mesh("Leg L", BuiltinShape::Cylinder, Vec3(-0.13f, 0.25f, 0), Vec3(0.17f, 0.5f, 0.17f), "M_Proto_Grey", player);
            b.Mesh("Leg R", BuiltinShape::Cylinder, Vec3(0.13f, 0.25f, 0), Vec3(0.17f, 0.5f, 0.17f), "M_Proto_Grey", player);
        };
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Third Person Character");
            const ecs::Entity player = b.Node("Player", Vec3(), root);
            b.world.Add(player, CharacterController{});
            b.Script(player, "ThirdPersonController.zs");
            mannequin(b, player);
            const ecs::Entity cam = b.Node("Camera", Vec3(0, 3, -6), root);
            b.world.Add(cam, Camera{});
            b.Script(cam, "ThirdPersonCamera.zs");
            b.Script(b.Node("Fire Starter", Vec3(), cam), "FireStarter.zs"); // F lights what you look at, G puts it out
            save(b, root, "Characters/P_ThirdPersonCharacter");
        }
        {
            PrefabBuilder b;
            const ecs::Entity player = b.Node("Player");
            b.world.Add(player, CharacterController{});
            b.Script(player, "FirstPersonController.zs");
            const ecs::Entity cam = b.Node("Camera", Vec3(0, 1.65f, 0), player);
            b.world.Add(cam, Camera{});
            b.Script(b.Node("Fire Starter", Vec3(), cam), "FireStarter.zs");
            save(b, player, "Characters/P_FirstPersonCharacter");
        }
        {
            PrefabBuilder b;
            const ecs::Entity tank = b.Node("Tank");
            CharacterController cc;
            cc.radius = 1.6f;
            cc.height = 2.6f;
            cc.stepHeight = 0.5f;
            cc.maxSlope = 35.0f;
            cc.pushMass = 3000.0f;
            b.world.Add(tank, cc);
            b.Script(tank, "TankController.zs");
            b.Mesh("Hull", BuiltinShape::Cube, Vec3(0, 0.95f, 0), Vec3(2.6f, 0.8f, 4.4f), "M_Paint_Olive", tank);
            b.Mesh("Glacis", BuiltinShape::Cube, Vec3(0, 1.05f, 2.2f), Vec3(2.5f, 0.12f, 0.9f), "M_Paint_Olive", tank,
                   Quat::FromAxisAngle(Vec3(1, 0, 0), DegToRad(-30.0f)));
            for (i32 side = -1; side <= 1; side += 2) {
                b.Mesh(side < 0 ? "Track L" : "Track R", BuiltinShape::Cube, Vec3(f32(side) * 1.45f, 0.5f, 0), Vec3(0.7f, 0.8f, 4.8f), "M_Rubber_Black", tank);
                for (u32 i = 0; i < 5; ++i)
                    b.Mesh(String::Format("Wheel {}{}", side < 0 ? "L" : "R", i + 1), BuiltinShape::Cylinder,
                           Vec3(f32(side) * 1.82f, 0.45f, -1.8f + f32(i) * 0.9f), Vec3(0.62f, 0.08f, 0.62f), "M_Metal_Steel", tank,
                           Quat::FromAxisAngle(Vec3(0, 0, 1), DegToRad(90.0f)));
            }
            const ecs::Entity turret = b.Node("Turret", Vec3(0, 1.35f, -0.2f), tank);
            b.Mesh("Turret Ring", BuiltinShape::Cylinder, Vec3(0, 0.12f, 0), Vec3(2.0f, 0.24f, 2.0f), "M_Paint_Olive", turret);
            b.Mesh("Turret Body", BuiltinShape::Cube, Vec3(0, 0.45f, 0.1f), Vec3(1.7f, 0.6f, 2.2f), "M_Paint_Olive", turret);
            b.Mesh("Hatch", BuiltinShape::Cylinder, Vec3(0.4f, 0.8f, -0.3f), Vec3(0.6f, 0.1f, 0.6f), "M_Metal_Steel", turret);
            const ecs::Entity barrel = b.Node("Barrel", Vec3(0, 0.45f, 1.1f), turret);
            b.Mesh("Barrel Tube", BuiltinShape::Cylinder, Vec3(0, 0, 1.4f), Vec3(0.2f, 2.8f, 0.2f), "M_Metal_Steel", barrel,
                   Quat::FromAxisAngle(Vec3(1, 0, 0), DegToRad(90.0f)));
            b.Mesh("Muzzle Brake", BuiltinShape::Cylinder, Vec3(0, 0, 2.8f), Vec3(0.3f, 0.35f, 0.3f), "M_Rubber_Black", barrel,
                   Quat::FromAxisAngle(Vec3(1, 0, 0), DegToRad(90.0f)));
            const ecs::Entity muzzle = b.Node("Muzzle", Vec3(0, 0, 3.05f), barrel);
            ParticleEmitter flash;
            flash.playing = false;
            flash.rate = 0.0f;
            flash.burst = 0;
            flash.shape = EmitterShape::Sphere;
            flash.radius = 0.15f;
            flash.speed = 2.5f;
            flash.lifetime = 0.25f;
            flash.startSize = 0.9f;
            flash.endSize = 0.2f;
            flash.acceleration = Vec3();
            flash.startColor = Vec3(1.0f, 0.75f, 0.35f);
            flash.endColor = Vec3(1.0f, 0.3f, 0.05f);
            flash.look = ParticleLook::Flame;
            flash.intensity = 6.0f;
            flash.fadeIn = 0.0f;
            b.world.Add(muzzle, flash);
            const ecs::Entity impact = b.Node("Impact", Vec3(0, -100, 0), tank);
            ParticleEmitter sparks = Sparks();
            sparks.playing = false;
            sparks.rate = 0.0f;
            sparks.shape = EmitterShape::Point;
            sparks.speed = 7.0f;
            sparks.lifetime = 0.9f;
            sparks.burst = 50;
            b.world.Add(impact, sparks);
            const ecs::Entity cam = b.Node("Tank Camera", Vec3(0, 3.0f, -8.0f), turret, Quat::FromAxisAngle(Vec3(1, 0, 0), DegToRad(12.0f)));
            b.world.Add(cam, Camera{});
            AudioSource engine;
            engine.clip = "StarterContent/Sounds/Props/Engine_Loop.ogg";
            engine.volume = 0.6f;
            engine.pitch = 0.8f;
            engine.maxDistance = 30.0f;
            b.world.Add(tank, engine);
            save(b, tank, "Characters/P_Tank");
        }
        {
            // Soft-body car: node-beam body that dents in crashes, four wheels on suspension, engine sound.
            PrefabBuilder b;
            const ecs::Entity car = b.Node("Car");
            b.world.Add(car, SoftBody{});
            MeshRenderer mr;
            mr.mesh = SoftBodyCarMeshId();
            mr.collision = false;
            for (StringView m : {"M_CarPaint", "M_CarGlass", "M_CarTrim", "M_CarHeadlight", "M_CarTaillight"})
                mr.materials.PushBack(Guid::FromName(Guid(), m));
            b.world.Add(car, mr);
            AudioSource engine;
            engine.clip = "StarterContent/Sounds/Props/Engine_Loop.ogg";
            engine.volume = 0.4f;
            engine.maxDistance = 45.0f;
            b.world.Add(car, engine);
            b.Script(car, "SoftBodyCar.zs");
            const Quat axle = Quat::FromAxisAngle(Vec3(0, 0, 1), HalfPi); // cylinders stand on Y: lay them on X
            for (u32 w = 0; w < 4; ++w) {
                const ecs::Entity wheel = b.Node(SoftBodyWheelNames[w], SoftBodyWheelPosition(w), car);
                const f32 d = SoftBodyWheelRadius * 2.0f;
                b.Mesh("Tire", BuiltinShape::Cylinder, Vec3(), Vec3(d, SoftBodyWheelWidth, d), "M_Rubber_Black", wheel, axle);
                b.Mesh("Rim", BuiltinShape::Cylinder, Vec3(), Vec3(d * 0.62f, SoftBodyWheelWidth + 0.01f, d * 0.62f), "M_Metal_Steel", wheel, axle);
                for (u32 k = 0; k < 2; ++k) // spokes: show the wheel turning
                    b.Mesh("Spoke", BuiltinShape::Cube, Vec3(), Vec3(SoftBodyWheelWidth + 0.02f, 0.07f, d * 0.58f), "M_CarTrim", wheel,
                           Quat::FromAxisAngle(Vec3(1, 0, 0), f32(k) * HalfPi));
            }
            b.world.Query<MeshRenderer>().Each([](ecs::Entity, MeshRenderer& r) { r.collision = false; });
            save(b, car, "Characters/P_SoftCar");
        }
    }

    if (options.starterProps) {
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Door");
            b.Mesh("Post L", BuiltinShape::Cube, Vec3(-0.75f, 1.2f, 0), Vec3(0.2f, 2.4f, 0.3f), "M_Proto_Light", root);
            b.Mesh("Post R", BuiltinShape::Cube, Vec3(0.75f, 1.2f, 0), Vec3(0.2f, 2.4f, 0.3f), "M_Proto_Light", root);
            b.Mesh("Lintel", BuiltinShape::Cube, Vec3(0, 2.5f, 0), Vec3(1.7f, 0.2f, 0.3f), "M_Proto_Light", root);
            const ecs::Entity panel = b.Mesh("Door Panel", BuiltinShape::Cube, Vec3(0, 1.12f, 0), Vec3(1.24f, 2.2f, 0.08f), "M_Wood", root);
            b.Body(panel, 40.0f);
            b.world.Add(panel, Wood());
            HingeJoint hinge;
            hinge.anchor = Vec3(-0.5f, 0, 0); // the left edge (local, scaled)
            hinge.axis = Vec3(0, 1, 0);
            hinge.useLimits = true;
            hinge.lowerLimit = 0.0f;
            hinge.upperLimit = DegToRad(105.0f);
            b.world.Add(panel, hinge);
            b.Script(panel, "Door.zs");
            save(b, root, "Props/P_Door");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Elevator");
            for (i32 x = -1; x <= 1; x += 2)
                for (i32 z = -1; z <= 1; z += 2)
                    b.Mesh("Rail", BuiltinShape::Cube, Vec3(f32(x) * 1.65f, 2.6f, f32(z) * 1.65f), Vec3(0.15f, 5.2f, 0.15f), "M_Metal_Steel", root);
            const ecs::Entity platform = b.Mesh("Platform", BuiltinShape::Cube, Vec3(0, 0.15f, 0), Vec3(3.0f, 0.3f, 3.0f), "M_Proto_Orange", root);
            b.Body(platform, 2000.0f, true, "Impact_Metal_Heavy");
            RigidBody& rb = *b.world.Get<RigidBody>(platform);
            rb.canSleep = false;
            SliderJoint slider;
            slider.axis = Vec3(0, 1, 0);
            slider.useLimits = true;
            slider.lowerLimit = 0.0f;
            slider.upperLimit = 4.0f;
            slider.useMotor = true;
            slider.motorSpeed = 0.0f;
            slider.motorForce = 200000.0f;
            b.world.Add(platform, slider);
            AudioSource motor; // started / stopped by Elevator.zs while it moves
            motor.clip = "StarterContent/Sounds/Props/Motor_Loop.ogg";
            motor.playOnStart = false;
            motor.volume = 0.5f;
            motor.maxDistance = 25.0f;
            b.world.Add(platform, motor);
            b.Script(platform, "Elevator.zs");
            save(b, root, "Props/P_Elevator");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Mesh("Jump Pad", BuiltinShape::Cylinder, Vec3(0, 0.1f, 0), Vec3(2.0f, 0.2f, 2.0f), "M_Proto_Blue");
            b.Mesh("Ring", BuiltinShape::Cylinder, Vec3(0, 0.6f, 0), Vec3(0.8f, 0.3f, 0.8f), "M_Emissive_Cyan", root);
            // Child of the pad: local values are in its scaled space (2 x 0.2 x 2), so this is 1.8 x 1.2 x 1.8 m.
            const ecs::Entity trigger = b.Node("Trigger", Vec3(0, 3.5f, 0), root, {}, Vec3(0.9f, 6.0f, 0.9f));
            BoxCollider zone;
            zone.isTrigger = true;
            b.world.Add(trigger, zone);
            ParticleEmitter glow;
            glow.shape = EmitterShape::Cone;
            glow.radius = 0.8f;
            glow.coneAngle = 0.05f;
            glow.rate = 12.0f;
            glow.lifetime = 1.0f;
            glow.speed = 2.0f;
            glow.acceleration = Vec3();
            glow.turbulence = 0.3f;
            glow.startSize = 0.25f;
            glow.endSize = 0.05f;
            glow.startColor = Vec3(0.3f, 0.9f, 1.0f);
            glow.endColor = Vec3(0.2f, 0.4f, 1.0f);
            glow.intensity = 2.0f;
            glow.worldSpace = false;
            b.world.Add(trigger, glow);
            b.Script(trigger, "JumpPad.zs");
            save(b, root, "Props/P_JumpPad");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Coin", Vec3(0, 1, 0));
            b.Mesh("Coin Mesh", BuiltinShape::Cylinder, Vec3(), Vec3(0.6f, 0.08f, 0.6f), "M_Gold_Glow", root,
                   Quat::FromAxisAngle(Vec3(1, 0, 0), DegToRad(90.0f)));
            SphereCollider zone;
            zone.radius = 0.6f;
            zone.isTrigger = true;
            b.world.Add(root, zone);
            ParticleEmitter sparkle;
            sparkle.playing = false;
            sparkle.rate = 0.0f;
            sparkle.shape = EmitterShape::Sphere;
            sparkle.radius = 0.3f;
            sparkle.speed = 2.5f;
            sparkle.lifetime = 0.8f;
            sparkle.acceleration = Vec3(0.0f, 1.0f, 0.0f);
            sparkle.startSize = 0.15f;
            sparkle.endSize = 0.0f;
            sparkle.startColor = Vec3(1.0f, 0.85f, 0.3f);
            sparkle.endColor = Vec3(1.0f, 0.6f, 0.1f);
            sparkle.intensity = 3.0f;
            sparkle.fadeIn = 0.0f;
            b.world.Add(root, sparkle);
            b.Script(root, "Coin.zs");
            save(b, root, "Props/P_Coin");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Crate Stack");
            u32 n = 0;
            for (u32 row = 0; row < 3; ++row)
                for (u32 i = 0; i < 3 - row; ++i) {
                    const ecs::Entity crate = b.Mesh(String::Format("Crate {}", ++n), BuiltinShape::Cube,
                                                     Vec3(-1.05f + f32(i) * 1.05f + f32(row) * 0.525f, 0.5f + f32(row) * 1.0f, 0), Vec3(1.0f), "M_Wood", root);
                    b.Body(crate, 25.0f);
                    b.world.Add(crate, Wood(0.15f, 25.0f));
                }
            save(b, root, "Props/P_CrateStack");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Swinging Lamp");
            b.Mesh("Hook", BuiltinShape::Cube, Vec3(0, 0, 0), Vec3(0.3f, 0.1f, 0.3f), "M_Metal_Steel", root);
            const ecs::Entity lamp = b.Mesh("Lamp", BuiltinShape::Sphere, Vec3(0, -2.0f, 0), Vec3(0.45f), "M_Emissive_Cyan", root);
            RigidBody rb;
            rb.mass = 6.0f;
            rb.canSleep = false;
            b.world.Add(lamp, rb);
            b.world.Add(lamp, SphereCollider{});
            b.Impact(lamp, "Impact_Metal_Light");
            BallJoint ball;
            ball.anchor = Vec3(0, 2.0f / 0.45f, 0); // the hook, in the lamp's scaled local space
            b.world.Add(lamp, ball);
            b.Mesh("Cord", BuiltinShape::Cylinder, Vec3(0, 2.22f, 0), Vec3(0.05f, 4.44f, 0.05f), "M_Rubber_Black", lamp); // local: x 1/0.45
            PointLight light;
            light.color = Vec3(0.6f, 0.9f, 1.0f);
            light.lumens = 900.0f;
            light.range = 8.0f;
            b.world.Add(lamp, light);
            b.Script(lamp, "Push.zs");
            save(b, root, "Props/P_SwingingLamp");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Rope Bridge");
            const u32 planks = 10;
            const f32 pitch = 0.6f, length = f32(planks) * pitch;
            for (i32 end = 0; end < 2; ++end)
                for (i32 x = -1; x <= 1; x += 2)
                    b.world.Add(b.Mesh("Post", BuiltinShape::Cylinder, Vec3(f32(x) * 1.0f, 0.4f, end == 0 ? -0.15f : length + 0.15f),
                                       Vec3(0.18f, 2.0f, 0.18f), "M_Wood", root),
                                Wood(0.1f, 40.0f));
            b.Mesh("Deck Start", BuiltinShape::Cube, Vec3(0, -0.35f, -1.15f), Vec3(2.2f, 0.5f, 2.0f), "M_Proto_Light", root);
            b.Mesh("Deck End", BuiltinShape::Cube, Vec3(0, -0.35f, length + 1.15f), Vec3(2.2f, 0.5f, 2.0f), "M_Proto_Light", root);
            for (u32 i = 0; i < planks; ++i) {
                const ecs::Entity plank = b.Mesh(String::Format("Plank {}", i + 1), BuiltinShape::Cube, Vec3(0, -0.15f, pitch * (f32(i) + 0.5f)),
                                                 Vec3(1.8f, 0.08f, pitch - 0.04f), "M_Wood", root);
                b.Body(plank, 8.0f, true, "Impact_Plank");
                b.world.Add(plank, Wood(0.25f, 10.0f)); // burns through and drops off its hinges
                // Hinged (around X) at its near edge to the previous plank - the first one to the world.
                HingeJoint h;
                h.anchor = Vec3(0, 0, -0.5f);
                h.axis = Vec3(1, 0, 0);
                if (i > 0)
                    h.connectedBody = String::Format("Plank {}", i);
                b.world.Add(plank, h);
                if (i + 1 == planks) {
                    BallJoint end; // the far edge pinned to the world
                    end.anchor = Vec3(0, 0, 0.5f);
                    b.world.Add(plank, end);
                }
            }
            save(b, root, "Props/P_RopeBridge");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Wooden Fence");
            const u32 sections = 5;
            const f32 span = 1.2f;
            for (u32 i = 0; i <= sections; ++i)
                b.world.Add(b.Mesh(String::Format("Post {}", i + 1), BuiltinShape::Cube, Vec3(f32(i) * span, 0.6f, 0), Vec3(0.14f, 1.2f, 0.14f), "M_Wood", root),
                            Wood(0.12f, 25.0f));
            for (u32 i = 0; i < sections; ++i)
                for (u32 r = 0; r < 2; ++r)
                    b.world.Add(b.Mesh(String::Format("Rail {}{}", i + 1, r == 0 ? "a" : "b"), BuiltinShape::Cube,
                                       Vec3((f32(i) + 0.5f) * span, r == 0 ? 0.4f : 0.95f, 0.09f), Vec3(span + 0.1f, 0.1f, 0.04f), "M_Wood", root),
                                Wood(0.18f, 14.0f));
            save(b, root, "Props/P_WoodenFence");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Mesh("Hay Bale", BuiltinShape::Cube, Vec3(0, 0.35f, 0), Vec3(1.2f, 0.7f, 0.7f), "M_Hay");
            b.Body(root, 25.0f, true, "Impact_Soft");
            b.world.Add(root, Hay());
            save(b, root, "Props/P_HayBale");
        }
        {
            PrefabBuilder b;
            const ecs::Entity root = b.Node("Wood Pile");
            u32 n = 0;
            for (u32 row = 0; row < 3; ++row)
                for (u32 i = 0; i < 3 - row; ++i) {
                    const ecs::Entity logEntity = b.Mesh(String::Format("Log {}", ++n), BuiltinShape::Cylinder,
                                                   Vec3(-0.3f + f32(i) * 0.3f + f32(row) * 0.15f, 0.15f + f32(row) * 0.27f, 0), Vec3(0.3f, 1.6f, 0.3f),
                                                   "M_Wood", root, Quat::FromAxisAngle(Vec3(1, 0, 0), DegToRad(90.0f)));
                    b.Body(logEntity, 12.0f);
                    b.world.Add(logEntity, Wood(0.1f, 35.0f));
                }
            save(b, root, "Props/P_WoodPile");
        }
    }
    ZX_LOG_INFO(LogStarter, "Starter Content: {} prefabs", written);
    return written;
}

} // namespace zx::editor
