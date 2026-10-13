#pragma once

// ZJSON: JSON DOM with a few relaxations for hand-edited engine files:
//   - // line and /* block */ comments
//   - trailing commas in arrays/objects
//   - unquoted identifier keys ({ name: 1 })
// Object member order is preserved (stable diffs). Integers are kept exactly (i64), others as f64.
// The writer always produces strict JSON.

#include "Core/Containers/Array.h"
#include "Core/String/String.h"

namespace zx {

enum class JsonType : u8
{
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
};

class JsonValue;
using JsonMember = Pair<String, JsonValue>;

class JsonValue
{
public:
    JsonValue() = default;
    JsonValue(std::nullptr_t) {}
    JsonValue(bool v) : m_type(JsonType::Bool) { m_bool = v; }
    JsonValue(i32 v) : JsonValue(i64(v)) {}
    JsonValue(u32 v) : JsonValue(i64(v)) {}
    JsonValue(i64 v) : m_type(JsonType::Number), m_isInteger(true) { m_int = v; }
    JsonValue(u64 v) : JsonValue(i64(v)) {} // stored as two's complement; read back with AsU64()
    JsonValue(f32 v) : JsonValue(f64(v)) { m_isSingle = true; } // written with the shortest f32 round-trip form
    JsonValue(f64 v) : m_type(JsonType::Number), m_isInteger(false) { m_double = v; }
    JsonValue(StringView s);
    JsonValue(const char* s) : JsonValue(StringView(s)) {}
    JsonValue(const String& s) : JsonValue(s.View()) {}
    JsonValue(const JsonValue& o);
    JsonValue(JsonValue&& o) noexcept;
    ~JsonValue() { Reset(); }
    JsonValue& operator=(const JsonValue& o);
    JsonValue& operator=(JsonValue&& o) noexcept;

    static JsonValue MakeArray();
    static JsonValue MakeObject();

    JsonType Type() const { return m_type; }
    bool IsNull() const { return m_type == JsonType::Null; }
    bool IsBool() const { return m_type == JsonType::Bool; }
    bool IsNumber() const { return m_type == JsonType::Number; }
    bool IsInteger() const { return m_type == JsonType::Number && m_isInteger; }
    /// Number created from an f32 (affects only how it is written).
    bool IsSingle() const { return m_type == JsonType::Number && m_isSingle; }
    bool IsString() const { return m_type == JsonType::String; }
    bool IsArray() const { return m_type == JsonType::Array; }
    bool IsObject() const { return m_type == JsonType::Object; }

    bool AsBool(bool fallback = false) const { return IsBool() ? m_bool : fallback; }
    i64 AsInt(i64 fallback = 0) const;
    u64 AsU64(u64 fallback = 0) const { return IsNumber() ? u64(AsInt(i64(fallback))) : fallback; }
    f64 AsDouble(f64 fallback = 0.0) const;
    f32 AsFloat(f32 fallback = 0.0f) const { return f32(AsDouble(fallback)); }
    StringView AsString(StringView fallback = {}) const;

    /// Number of array elements / object members (0 otherwise).
    usize Size() const;

    // --- Array ---
    JsonValue& operator[](usize index);
    const JsonValue& operator[](usize index) const;
    JsonValue& PushBack(JsonValue v); // converts Null to Array
    Span<JsonValue> Elements();
    Span<const JsonValue> Elements() const;

    // --- Object ---
    /// Member lookup (nullptr if absent or not an object).
    JsonValue* Find(StringView key);
    const JsonValue* Find(StringView key) const;
    bool Contains(StringView key) const { return Find(key) != nullptr; }
    /// Returns member, inserting Null if absent (converts Null to Object).
    JsonValue& operator[](StringView key);
    /// Read-only member access returning a static Null if absent.
    const JsonValue& Get(StringView key) const;
    JsonValue& Set(StringView key, JsonValue v);
    bool Remove(StringView key);
    Span<JsonMember> Members();
    Span<const JsonMember> Members() const;

    void Reset();
    friend bool operator==(const JsonValue& a, const JsonValue& b);

private:
    JsonType m_type = JsonType::Null;
    bool m_isInteger = false;
    bool m_isSingle = false;
    union {
        bool m_bool;
        i64 m_int;
        f64 m_double = 0.0;
        String* m_string;
        Array<JsonValue>* m_array;
        Array<JsonMember>* m_object;
    };
};

struct JsonParseError
{
    String message;
    u32 line = 0;
    u32 column = 0;
};

/// Parses ZJSON text. On failure returns false and fills `error` (if provided).
bool ParseJson(StringView text, JsonValue& out, JsonParseError* error = nullptr);

struct JsonWriteOptions
{
    bool pretty = true;
    u32 indent = 2;
    /// Arrays of only numbers/bools with at most this many elements are written on one line (pretty mode).
    u32 inlineArrayMax = 16;
};

void WriteJson(const JsonValue& value, String& out, const JsonWriteOptions& options = {});
String ToJsonString(const JsonValue& value, const JsonWriteOptions& options = {});

} // namespace zx
