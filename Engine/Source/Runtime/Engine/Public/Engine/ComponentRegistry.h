#pragma once

// Components known to the editor and to scene serialization: ECS component id <-> reflected type, display name, icon.
//
//   RegisterEngineComponents();                         // once at startup (Transform, MeshRenderer, lights, ...)
//   for (const ComponentType& c : GetComponentTypes()) { ... world.GetComponent(e, c.id, false) ... }

#include "Core/Reflection/Reflection.h"
#include "Scene/ECS/World.h"

namespace zx {

struct ComponentType
{
    ecs::ComponentId id = ecs::InvalidComponentId;
    const TypeInfo* type = nullptr;
    StringView displayName;
    StringView icon;           // UTF-8 icon glyph (UI/Icons.h)
    bool userAddable = true;   // offered in "Add Component"
    bool removable = true;
    bool hiddenInDetails = false; // e.g. EntityName (edited in the header)
};

void RegisterComponentType(const ComponentType& type);

template <typename T>
void RegisterComponent(StringView displayName, StringView icon, bool userAddable = true, bool removable = true, bool hidden = false)
{
    ComponentType c;
    c.id = ecs::ComponentIdOf<T>();
    c.type = &TypeOf<T>();
    c.displayName = displayName;
    c.icon = icon;
    c.userAddable = userAddable;
    c.removable = removable;
    c.hiddenInDetails = hidden;
    RegisterComponentType(c);
}

/// Registers the engine's built-in components (idempotent).
void RegisterEngineComponents();

Span<const ComponentType> GetComponentTypes();
const ComponentType* FindComponentType(ecs::ComponentId id);
const ComponentType* FindComponentType(StringView typeName);

} // namespace zx
