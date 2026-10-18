#pragma once

// Wind for the whole scene (like Unity's Wind Zone), tunable per object.
//
//   Wind Zone      a source: Directional (everywhere, blowing along the entity's forward +Z) or Spherical (blowing out
//                  from the entity, fading to nothing at the radius). Speed, turbulence (swirls) and gusts. Zones add up.
//   Wind Response  on any object: how much the wind affects it (0 = sheltered, 2 = twice as much), or its own wind
//                  that ignores the zones; Drag sets how hard the wind pushes a Rigid Body.
//
// Who uses it: Particle Emitters (particles drift towards the wind's speed at their drag rate), fire and smoke, Height
// Fog / Fog Volumes (drift), Volumetric Clouds and Water (direction and speed) unless they override their own wind,
// Rigid Bodies with a Wind Response (aerodynamic drag), soft-body cars.
//
//   TickWind(world, dt);                                  // once per frame (editing too): gusts advance
//   Vec3 w = WindAt(world, position);                     // the zones at a point (m/s)
//   Vec3 w = WindFor(world, entity, position);            // with the object's Wind Response (or its ancestors')

#include "Core/Math/Math.h"
#include "Core/Reflection/Reflection.h"
#include "Scene/ECS/World.h"

namespace zx {

enum class WindZoneMode : u8
{
    Directional,
    Spherical,
};
ZX_DECLARE_ENUM(WindZoneMode);

struct WindZone
{
    WindZoneMode mode = WindZoneMode::Directional;
    f32 speed = 6.0f;           // m/s (Directional: along the entity's forward)
    f32 radius = 25.0f;         // Spherical: meters; the wind fades to zero there
    f32 turbulence = 0.35f;     // 0..1: swirls, as a fraction of the speed
    f32 gustStrength = 0.5f;    // 0..1: how much stronger the gusts blow
    f32 gustFrequency = 0.12f;  // gusts per second
};

struct WindResponse
{
    f32 strength = 1.0f;      // multiplier of the scene's wind (0 = sheltered)
    bool customWind = false;  // ignore the Wind Zones and use this wind:
    f32 customSpeed = 5.0f;   // m/s
    f32 customDirection = 0.0f; // degrees around +Y (0 = towards +Z)
    f32 drag = 0.6f;          // Rigid Body: area * drag coefficient (m^2) - how hard the wind pushes it
};

ZX_DECLARE_TYPE(WindZone);
ZX_DECLARE_TYPE(WindResponse);

/// Wind clock (a world singleton): gusts and swirls are functions of it.
struct WindClock
{
    f64 time = 0.0;
};

/// Advances the wind's gusts (call once per frame, also while editing so previews move).
void TickWind(ecs::World& world, f32 dt);
/// The environment clock (seconds, game time: paused / slowed with the game): gusts, waves. Negative = never ticked.
f64 EnvironmentTime(ecs::World& world);
/// Wind of all zones at `position` (m/s).
Vec3 WindAt(ecs::World& world, Vec3 position);
/// Wind felt by `entity` at `position`: the zones scaled by its Wind Response (or the nearest ancestor's), or its own
/// custom wind.
Vec3 WindFor(ecs::World& world, ecs::Entity entity, Vec3 position);
/// The mean wind (no gusts, no swirls) felt by `entity` - for things that must not flicker with every gust (waves).
Vec3 WindSteadyFor(ecs::World& world, ecs::Entity entity, Vec3 position);
/// True when the scene has at least one Wind Zone (systems with their own wind settings use those otherwise).
bool HasWind(ecs::World& world);
/// Air drag force on a body moving at `velocity` in `wind`: 0.5 * rho * CdA * |v_rel| * v_rel (N).
Vec3 WindDragForce(Vec3 wind, Vec3 velocity, f32 dragArea);

} // namespace zx
