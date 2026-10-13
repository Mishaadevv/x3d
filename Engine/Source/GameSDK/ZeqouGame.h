#pragma once

// ZeqouX3D Game SDK: write gameplay in C++ as a DLL the editor builds and hot-reloads while the game is playing.
//
//   #include "ZeqouGame.h"
//   using namespace zx::game;
//
//   class Spinner : public ScriptComponent
//   {
//   public:
//       float speed = 90.0f;                      // degrees per second, edited in the Details panel
//       void OnUpdate(float dt) override { Rotate(Vec3(0, speed * dt, 0)); }
//   };
//   ZX_GAME_COMPONENT(Spinner, "Spins the entity")
//       ZX_GAME_PROP(speed, -720, 720, "Degrees per second")
//   ZX_GAME_COMPONENT_END()
//
//   ZX_IMPLEMENT_GAME_MODULE("MyGame")
//
// The SDK is a single header with no dependency on the engine's libraries: the engine talks to the module through
// the Host interface below, so the DLL can be unloaded and loaded again at any time. Props (ZX_GAME_PROP) are saved in
// the scene and carried over a hot reload by name; other members start from their defaults after a reload.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace zx::game {

inline constexpr uint32_t ApiVersion = 2; // 2: physics (Host forces/velocity/raycast, collision callbacks)

// --- Math --------------------------------------------------------------------------------------------------------------

struct Vec3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    constexpr Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(Vec3 o) { return *this = *this + o; }
    Vec3& operator-=(Vec3 o) { return *this = *this - o; }
    Vec3& operator*=(float s) { return *this = *this * s; }
    float Length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 Normalized() const
    {
        const float l = Length();
        return l > 1e-8f ? *this / l : Vec3();
    }
};
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Distance(Vec3 a, Vec3 b) { return (a - b).Length(); }
inline Vec3 Lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

/// Rotation (x, y, z, w). Build with Quat::Euler (degrees) or Quat::AxisAngle.
struct Quat
{
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    static Quat AxisAngle(Vec3 axis, float degrees)
    {
        const Vec3 a = axis.Normalized();
        const float h = degrees * 0.0087266462599716f; // half angle in radians
        const float s = std::sin(h);
        return {a.x * s, a.y * s, a.z * s, std::cos(h)};
    }
    /// Pitch (x), yaw (y), roll (z) in degrees - the engine's convention (yaw, then pitch, then roll).
    static Quat Euler(float pitch, float yaw, float roll)
    {
        return AxisAngle(Vec3(0, 1, 0), yaw) * AxisAngle(Vec3(1, 0, 0), pitch) * AxisAngle(Vec3(0, 0, 1), roll);
    }
    Quat operator*(const Quat& b) const
    {
        return {w * b.x + x * b.w + y * b.z - z * b.y, w * b.y - x * b.z + y * b.w + z * b.x, w * b.z + x * b.y - y * b.x + z * b.w,
                w * b.w - x * b.x - y * b.y - z * b.z};
    }
    Vec3 operator*(Vec3 v) const
    {
        const Vec3 q(x, y, z);
        const Vec3 t = Cross(q, v) * 2.0f;
        return v + t * w + Cross(q, t);
    }
};

using EntityId = uint64_t; // 0 = none

/// Result of Host::Raycast.
struct RaycastResult
{
    EntityId entity = 0;
    Vec3 point;
    Vec3 normal;
    float distance = 0.0f;
};

// --- Host: what the engine provides -------------------------------------------------------------------------------------

class Host
{
public:
    virtual void Log(const char* message, bool isError) = 0;
    // Entities (positions are local to the parent).
    virtual const char* GetName(EntityId e) = 0;
    virtual EntityId Find(const char* name) = 0;
    virtual bool IsAlive(EntityId e) = 0;
    virtual void Destroy(EntityId e) = 0;
    /// Creates a built-in shape ("Cube", "Sphere", "Cylinder", "Plane") or an empty entity ("") at a position.
    virtual EntityId Spawn(const char* shape, Vec3 position) = 0;
    virtual Vec3 GetPosition(EntityId e) = 0;
    virtual void SetPosition(EntityId e, Vec3 p) = 0;
    virtual Quat GetRotation(EntityId e) = 0;
    virtual void SetRotation(EntityId e, Quat q) = 0;
    virtual Vec3 GetScale(EntityId e) = 0;
    virtual void SetScale(EntityId e, Vec3 s) = 0;
    // Input: keys by name ("W", "Space", "Shift"), mouse buttons ("left"), and the project's Input Actions.
    virtual bool KeyDown(const char* key) = 0;
    virtual bool KeyPressed(const char* key) = 0;
    virtual bool ActionDown(const char* action) = 0;
    virtual bool ActionPressed(const char* action) = 0;
    virtual float Axis(const char* axis) = 0;
    virtual Vec3 MouseDelta() = 0;
    // Time.
    virtual float DeltaTime() = 0;
    virtual double Time() = 0;
    // Character Controller of an entity.
    virtual void CharacterMove(EntityId e, Vec3 velocity) = 0;
    virtual void CharacterJump(EntityId e, float speed) = 0;
    virtual bool CharacterGrounded(EntityId e) = 0;
    // Sound (path under the project's Content folder).
    virtual uint32_t PlaySound(const char* file, float volume, bool spatial, Vec3 position) = 0;
    virtual void StopSound(uint32_t voice) = 0;
    // Physics: the entity's Rigid Body (no effect without one).
    virtual void AddForce(EntityId e, Vec3 force) = 0;     // newtons, this frame
    virtual void AddImpulse(EntityId e, Vec3 impulse) = 0; // N*s, instant
    virtual Vec3 GetVelocity(EntityId e) = 0;
    virtual void SetVelocity(EntityId e, Vec3 velocity) = 0;
    /// Closest solid collider along the ray (unit or not); layerMask: one bit per physics layer (~0u = all).
    virtual bool Raycast(Vec3 origin, Vec3 direction, float maxDistance, RaycastResult* hit, uint32_t layerMask) = 0;

protected:
    ~Host() = default;
};

// --- Components -----------------------------------------------------------------------------------------------------------

class ScriptComponent
{
public:
    virtual ~ScriptComponent() = default;
    virtual void OnStart() {}
    virtual void OnUpdate(float dt) { (void)dt; }
    virtual void OnStop() {}
    /// Called instead of OnStart on the new object after a hot reload (props already restored).
    virtual void OnReloaded() {}
    // Physics callbacks (colliders of this entity): touching another solid body / entering a trigger.
    virtual void OnCollisionEnter(EntityId other, float speed) { (void)other, (void)speed; }
    virtual void OnCollisionExit(EntityId other) { (void)other; }
    virtual void OnTriggerEnter(EntityId other) { (void)other; }
    virtual void OnTriggerExit(EntityId other) { (void)other; }

    // Set by the engine before OnStart.
    EntityId entity = 0;
    Host* host = nullptr;

protected:
    // Convenience wrappers for this component's entity.
    Vec3 Position() const { return host->GetPosition(entity); }
    void SetPosition(Vec3 p) const { host->SetPosition(entity, p); }
    Quat Rotation() const { return host->GetRotation(entity); }
    void SetRotation(Quat q) const { host->SetRotation(entity, q); }
    Vec3 Forward() const { return Rotation() * Vec3(0, 0, 1); }
    Vec3 Right() const { return Rotation() * Vec3(1, 0, 0); }
    /// Rotates by Euler degrees in local space.
    void Rotate(Vec3 eulerDegrees) const { SetRotation(Rotation() * Quat::Euler(eulerDegrees.x, eulerDegrees.y, eulerDegrees.z)); }
    void Translate(Vec3 localOffset) const { SetPosition(Position() + Rotation() * localOffset); }
    const char* Name() const { return host->GetName(entity); }
    void Log(const char* message) const { host->Log(message, false); }
};

// --- Reflection for the editor and hot reload ---------------------------------------------------------------------------

enum class PropType : uint32_t
{
    Float,
    Int,
    Bool,
    Vec3,
};

struct PropDesc
{
    const char* name;
    PropType type;
    uint32_t offset;
    float min, max; // equal = unbounded
    const char* tooltip;
};

struct ComponentDesc
{
    const char* name;
    const char* description;
    ScriptComponent* (*create)();
    void (*destroy)(ScriptComponent*);
    const PropDesc* props;
    uint32_t propCount;
    const ComponentDesc* next; // module-local registration list
};

struct ModuleDesc
{
    uint32_t apiVersion;
    const char* name;
    const ComponentDesc* first;
};

namespace detail {
inline const ComponentDesc*& Head()
{
    static const ComponentDesc* head = nullptr;
    return head;
}
template <typename T>
constexpr PropType TypeOfProp()
{
    if constexpr (std::is_same_v<T, Vec3>)
        return PropType::Vec3;
    else if constexpr (std::is_same_v<T, bool>)
        return PropType::Bool;
    else if constexpr (std::is_same_v<T, int>)
        return PropType::Int;
    else {
        static_assert(std::is_same_v<T, float>, "ZX_GAME_PROP supports float, int, bool and Vec3 members");
        return PropType::Float;
    }
}
struct Registrar
{
    explicit Registrar(ComponentDesc* d)
    {
        d->next = Head();
        Head() = d;
    }
};
} // namespace detail

} // namespace zx::game

#define ZX_GAME_PROP(member, minValue, maxValue, tooltipText)                                                                    \
    ::zx::game::PropDesc{#member, ::zx::game::detail::TypeOfProp<decltype(ZxThis::member)>(),                                   \
                         static_cast<uint32_t>(offsetof(ZxThis, member)), float(minValue), float(maxValue), tooltipText},

#define ZX_GAME_COMPONENT(Type, descriptionText)                                                                                \
    namespace zx_game_component_##Type {                                                                                        \
    using ZxThis = Type;                                                                                                         \
    static ::zx::game::ScriptComponent* Create() { return new Type(); }                                                          \
    static void Destroy(::zx::game::ScriptComponent* c) { delete c; }                                                            \
    static const char* const Description = descriptionText;                                                                     \
    static const char* const Name = #Type;                                                                                      \
    static const ::zx::game::PropDesc Props[] = {ZX_GAME_PROP_FIRST_

// A dummy first element keeps the array non-empty for components without props; it is skipped by count.
#define ZX_GAME_PROP_FIRST_ ::zx::game::PropDesc{nullptr, ::zx::game::PropType::Float, 0, 0.0f, 0.0f, nullptr},

#define ZX_GAME_COMPONENT_END()                                                                                                \
    };                                                                                                                           \
    static ::zx::game::ComponentDesc Desc = {Name, Description, &Create, &Destroy, Props + 1,                                    \
                                             static_cast<uint32_t>(sizeof(Props) / sizeof(Props[0]) - 1), nullptr};               \
    static ::zx::game::detail::Registrar Register(&Desc);                                                                        \
    }

#define ZX_IMPLEMENT_GAME_MODULE(moduleName)                                                                                    \
    extern "C" __declspec(dllexport) const ::zx::game::ModuleDesc* ZxGetGameModule()                                             \
    {                                                                                                                            \
        static ::zx::game::ModuleDesc desc = {::zx::game::ApiVersion, moduleName, nullptr};                                     \
        desc.first = ::zx::game::detail::Head();                                                                                 \
        return &desc;                                                                                                            \
    }
