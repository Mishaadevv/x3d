#include "Engine/ComponentRegistry.h"
#include "Core/Memory/Memory.h"
#include "Engine/Animation.h"
#include "Engine/AudioSystem.h"
#include "Engine/Fire.h"
#include "Engine/GameModule.h"
#include "Engine/Particles.h"
#include "Engine/PhysicsSystem.h"
#include "Engine/Gameplay.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/Prefab.h"
#include "Engine/SceneRendering.h"
#include "Engine/Scripting.h"
#include "Engine/Terrain.h"

namespace zx {

namespace {

Array<ComponentType>& Registry()
{
    static Array<ComponentType>* s_types = [] {
        LeakIgnoreScope permanent;
        return New<Array<ComponentType>>();
    }();
    return *s_types;
}

} // namespace

void RegisterComponentType(const ComponentType& type)
{
    LeakIgnoreScope permanent;
    Array<ComponentType>& r = Registry();
    for (ComponentType& existing : r)
        if (existing.id == type.id) {
            existing = type;
            return;
        }
    r.PushBack(type);
}

void RegisterEngineComponents()
{
    // Icons: Lucide codepoints (UI/Icons.h); kept as literals so the Engine module doesn't depend on UI.
    RegisterComponent<scene::EntityName>("Name", "\xEE\x85\xBF", false, false, true); // tag
    RegisterComponent<Transform>("Transform", "\xEE\x8B\xA5", false, false);          // move-3d
    RegisterComponent<MeshRenderer>("Mesh Renderer", "\xEE\x81\xA1");                 // box
    RegisterComponent<PointLight>("Point Light", "\xEE\x87\x82");                     // lightbulb
    RegisterComponent<SpotLight>("Spot Light", "\xEE\x9A\x82");                       // spotlight
    RegisterComponent<DirectionalLight>("Directional Light", "\xEE\x85\xB8");     // sun
    RegisterComponent<SkyLight>("Sky Light", "\xEE\x88\x96");                     // cloud-sun
    RegisterComponent<Rotator>("Rotator", "\xEE\x8B\xAA");                            // rotate-3d
    RegisterComponent<Oscillator>("Oscillator", "\xEE\x8A\x83");                      // waves
    RegisterComponent<AudioSource>("Audio Source", "\xEE\x86\xAB");                  // volume-2
    RegisterComponent<AudioListener>("Audio Listener", "\xEE\x8E\x82");              // ear
    RegisterComponent<ImpactSound>("Impact Sound", "\xEE\x86\xAB");                  // volume-2
    RegisterComponent<ReverbZone>("Reverb Zone", "\xEE\x95\x9A");                    // audio-lines
    RegisterComponent<HeightFog>("Height Fog", "\xEE\x88\x94");                      // cloud-fog
    RegisterComponent<FogVolume>("Fog Volume", "\xEE\x83\xB0");                      // haze
    RegisterComponent<VolumetricClouds>("Volumetric Clouds", "\xEE\x88\x97");       // cloudy
    RegisterComponent<FluidVolume>("Fluid Volume", "\xEE\x83\x92");                  // flame
    RegisterComponent<Water>("Water", "\xEE\x82\xB5");                                // droplets
    RegisterComponent<Terrain>("Terrain", "\xEE\x88\xB1");                            // mountain
    RegisterComponent<ParticleEmitter>("Particle Emitter", "\xEE\x90\x92");        // sparkles
    RegisterComponent<Animator>("Animator", "\xEE\x8D\x98");                         // bone
    RegisterComponent<Flammable>("Flammable", "\xEE\x83\x92");                     // flame
    RegisterComponent<Decal>("Decal", "\xEE\x8E\xBB");                                 // stamp
    RegisterComponent<ReflectionProbe>("Reflection Probe", "\xEE\x8D\x85");           // circle-dot
    RegisterComponent<HdriSky>("HDRI Sky", "\xEE\x88\x96");                          // cloud-sun
    RegisterComponent<RigidBody>("Rigid Body", "\xEE\x94\xB0");                     // weight
    RegisterComponent<BoxCollider>("Box Collider", "\xEE\x87\x8B");                 // square-dashed
    RegisterComponent<SphereCollider>("Sphere Collider", "\xEE\x92\xB0");           // circle-dashed
    RegisterComponent<CapsuleCollider>("Capsule Collider", "\xEE\x8E\xBD");         // pill
    RegisterComponent<CylinderCollider>("Cylinder Collider", "\xEE\x94\xA5");       // cylinder
    RegisterComponent<MeshCollider>("Mesh Collider", "\xEE\x86\x92");               // triangle
    RegisterComponent<HingeJoint>("Hinge Joint", "\xEE\x8F\x96");                   // door-open
    RegisterComponent<FixedJoint>("Fixed Joint", "\xEE\x84\x82");                   // link
    RegisterComponent<SliderJoint>("Slider Joint", "\xEE\x87\x86");                 // move-horizontal
    RegisterComponent<BallJoint>("Ball Joint", "\xEE\x8F\xA7");                     // orbit
    RegisterComponent<DistanceJoint>("Distance Joint", "\xEE\x8E\x8B");             // spline
    RegisterComponent<ConfigurableJoint>("Configurable Joint", "\xEE\x8A\x9A");     // sliders-horizontal
    RegisterComponent<PostProcessVolume>("Post Process Volume", "\xEE\x81\x80");      // aperture
    RegisterComponent<Camera>("Camera", "\xEE\x86\xA5");                              // video
    RegisterComponent<CharacterController>("Character Controller", "\xEE\x88\x9E");  // person-standing
    RegisterComponent<PaintedInstance>("Painted Instance", "\xEE\x8B\xA7", false, true, true); // paintbrush (Brush tool marker)
    RegisterComponent<Script>("Script", "\xEE\x83\x83");                              // file-code
    RegisterComponent<NativeScript>("C++ Script", "\xEE\x8D\xAA");                  // braces
    RegisterComponent<PrefabInstance>("Prefab", "\xEE\x84\xA9", false, false, true);  // package (shown in the prefab bar)
    RegisterComponent<PrefabLink>("Prefab Link", "\xEE\x84\xA9", false, false, true);
}

Span<const ComponentType> GetComponentTypes() { return Registry().View(); }

const ComponentType* FindComponentType(ecs::ComponentId id)
{
    for (const ComponentType& c : Registry())
        if (c.id == id)
            return &c;
    return nullptr;
}

const ComponentType* FindComponentType(StringView typeName)
{
    for (const ComponentType& c : Registry())
        if (c.type->name == Name(typeName))
            return &c;
    return nullptr;
}

} // namespace zx
