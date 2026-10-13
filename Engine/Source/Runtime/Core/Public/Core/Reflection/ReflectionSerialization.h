#pragma once

// Generic (de)serialization of reflected values.
//
// JSON mapping: bool/numbers/strings as-is, Name/Guid as strings, enums by entry name ("A|B" for flags),
// structs as objects, Array<T> as arrays. Structs marked compact (Meta::Custom("Compact")) such as Vec3 are
// written as number arrays [x, y, z]; both forms are accepted when reading.
// Reading is tolerant: unknown fields are reported as warnings and skipped, missing fields keep their value.
//
// Binary: sequential, no field names. Valid only for the exact schema (see Reflection::ComputeSchemaHash).

#include "Core/Reflection/Reflection.h"
#include "Core/Serialization/Binary.h"
#include "Core/Serialization/Json.h"

namespace zx::Reflection {

/// Default-constructed instance of a type (cached, never destroyed). Null for Void.
const void* GetDefaultObject(const TypeInfo& type);

/// Serializes `object`. If `defaults` is given, struct fields equal to the defaults are omitted (delta).
JsonValue ToJson(const void* object, const TypeInfo& type, const void* defaults = nullptr);
bool FromJson(const JsonValue& json, void* object, const TypeInfo& type, Array<String>* warnings = nullptr);

template <typename T>
JsonValue ToJson(const T& value, bool deltaFromDefaults = false)
{
    const TypeInfo& t = TypeOf<T>();
    return ToJson(&value, t, deltaFromDefaults ? GetDefaultObject(t) : nullptr);
}

template <typename T>
bool FromJson(const JsonValue& json, T& value, Array<String>* warnings = nullptr)
{
    return FromJson(json, &value, TypeOf<T>(), warnings);
}

void WriteBinary(BinaryWriter& writer, const void* object, const TypeInfo& type);
bool ReadBinary(BinaryReader& reader, void* object, const TypeInfo& type);

/// Copies `src` into `dst` (both of `type`) via copyAssign.
void CopyValue(void* dst, const void* src, const TypeInfo& type);

/// Converts a scalar reflected value to display/edit text, and back (used by the inspector / console).
String ValueToString(const void* object, const TypeInfo& type);
bool ValueFromString(StringView text, void* object, const TypeInfo& type);

} // namespace zx::Reflection
