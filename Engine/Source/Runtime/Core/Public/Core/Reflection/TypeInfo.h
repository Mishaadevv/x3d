#pragma once

// Runtime type information (ZRefl). See Core/Reflection/Reflection.h for the registration API.

#include "Core/Containers/Array.h"
#include "Core/String/Name.h"

namespace zx {

enum class TypeKind : u8
{
    Void,
    Bool,
    Int8,
    Int16,
    Int32,
    Int64,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Float,
    Double,
    String,
    Name,
    Guid,
    Enum,
    Struct,
    Array, // zx::Array<T>
};

constexpr bool IsIntegerKind(TypeKind k) { return k >= TypeKind::Int8 && k <= TypeKind::UInt64; }
constexpr bool IsSignedKind(TypeKind k) { return k >= TypeKind::Int8 && k <= TypeKind::Int64; }
constexpr bool IsFloatKind(TypeKind k) { return k == TypeKind::Float || k == TypeKind::Double; }
constexpr bool IsNumericKind(TypeKind k) { return IsIntegerKind(k) || IsFloatKind(k); }

/// A metadata value attached to a field/type/enum entry (e.g. Range, Tooltip, Category).
struct MetaValue
{
    enum class Kind : u8
    {
        Flag,
        Number,
        Text
    };
    Kind kind = Kind::Flag;
    f64 number = 0.0;
    f64 number2 = 0.0; // second number for ranges
    String text;
};

struct MetaEntry
{
    Name key;
    MetaValue value;
};

class MetaData
{
public:
    void Add(MetaEntry e) { m_entries.PushBack(move(e)); }
    bool Has(Name key) const { return Find(key) != nullptr; }
    const MetaValue* Find(Name key) const
    {
        for (const MetaEntry& e : m_entries)
            if (e.key == key)
                return &e.value;
        return nullptr;
    }
    StringView GetText(Name key, StringView fallback = {}) const
    {
        const MetaValue* v = Find(key);
        return v ? v->text.View() : fallback;
    }
    f64 GetNumber(Name key, f64 fallback = 0.0) const
    {
        const MetaValue* v = Find(key);
        return v ? v->number : fallback;
    }
    Span<const MetaEntry> Entries() const { return m_entries.View(); }

private:
    Array<MetaEntry> m_entries;
};

/// Common metadata keys and constructors.
namespace Meta {
// Keys
Name KeyTooltip();
Name KeyCategory();
Name KeyDisplayName();
Name KeyRange();
Name KeyStep();
Name KeyUnits();
Name KeyHidden();
Name KeyReadOnly();
Name KeyTransient();
Name KeyColor();
Name KeyHDR();
Name KeyMultiline();
Name KeyEditCondition();
Name KeyFormerName();
Name KeyAngle();
Name KeyAssetType();

MetaEntry Tooltip(StringView text);
MetaEntry Category(StringView text);
MetaEntry DisplayName(StringView text);
/// Inclusive numeric range for sliders/clamping.
MetaEntry Range(f64 min, f64 max);
MetaEntry Step(f64 step);
MetaEntry Units(StringView units);
/// Not shown in the inspector (still serialized).
MetaEntry Hidden();
MetaEntry ReadOnly();
/// Not serialized (runtime-only state).
MetaEntry Transient();
/// Vec3/Vec4/LinearColor field edited with a color picker.
MetaEntry Color();
/// Allows color values > 1.
MetaEntry HDR();
MetaEntry Multiline();
/// Field is editable only when the named bool field is true.
MetaEntry EditCondition(StringView boolFieldName);
/// Previous name of the field, used when loading old data.
MetaEntry FormerName(StringView oldName);
/// Field is stored in radians but shown in degrees.
MetaEntry Angle();
MetaEntry AssetType(StringView typeName);
MetaEntry Custom(StringView key, StringView text = {});
} // namespace Meta

class TypeInfo;

enum class FieldFlags : u8
{
    None = 0,
    Hidden = 1 << 0,
    ReadOnly = 1 << 1,
    Transient = 1 << 2,
};
ZX_ENUM_FLAGS(FieldFlags)

struct FieldInfo
{
    Name name;
    String displayName;
    const TypeInfo* type = nullptr;
    u32 offset = 0;
    FieldFlags flags = FieldFlags::None;
    MetaData meta;

    void* Ptr(void* object) const { return static_cast<u8*>(object) + offset; }
    const void* Ptr(const void* object) const { return static_cast<const u8*>(object) + offset; }
    bool IsTransient() const { return HasAnyFlags(flags, FieldFlags::Transient); }
};

struct EnumEntry
{
    Name name;
    String displayName;
    i64 value = 0;
    MetaData meta;
};

/// Type-erased operations on a dynamic array container (zx::Array<T>).
struct ArrayOps
{
    usize (*size)(const void* array);
    void* (*at)(void* array, usize index);
    const void* (*atConst)(const void* array, usize index);
    void (*resize)(void* array, usize newSize);
    void (*clear)(void* array);
};

/// Reflected callable (member function). Arguments are passed as pointers to values of the parameter types.
struct MethodInfo
{
    Name name;
    String displayName;
    const TypeInfo* returnType = nullptr; // Void type for no return
    Array<const TypeInfo*> parameterTypes;
    Array<Name> parameterNames;
    bool isConst = false;
    MetaData meta;
    /// result may be null if returnType is Void.
    void (*invoke)(void* object, void* const* args, void* result) = nullptr;
};

class TypeInfo
{
public:
    Name name;
    String displayName;
    TypeKind kind = TypeKind::Void;
    u32 size = 0;
    u32 alignment = 1;
    u64 id = 0; // stable hash of the name

    // Lifecycle (null for Void).
    void (*construct)(void* dst) = nullptr;
    void (*destruct)(void* dst) = nullptr;
    void (*copyAssign)(void* dst, const void* src) = nullptr;

    // Struct
    const TypeInfo* base = nullptr;
    Array<FieldInfo> fields;
    Array<MethodInfo> methods;

    // Enum
    const TypeInfo* underlying = nullptr;
    Array<EnumEntry> enumEntries;
    bool isFlags = false;

    // Array
    const TypeInfo* element = nullptr;
    const ArrayOps* arrayOps = nullptr;

    MetaData meta;

    bool IsStruct() const { return kind == TypeKind::Struct; }
    bool IsEnum() const { return kind == TypeKind::Enum; }
    bool IsArray() const { return kind == TypeKind::Array; }
    /// True if this type is `other` or derives from it.
    bool IsA(const TypeInfo& other) const;

    /// Field by name including base classes (nullptr if not found). Also matches Meta::FormerName.
    const FieldInfo* FindField(Name fieldName) const;
    const MethodInfo* FindMethod(Name methodName) const;
    /// Visits fields of base classes first, then own fields.
    template <typename Fn>
    void ForEachField(Fn&& fn) const
    {
        if (base)
            base->ForEachField(fn);
        for (const FieldInfo& f : fields)
            fn(f);
    }

    // Enum helpers
    const EnumEntry* FindEnumByValue(i64 value) const;
    const EnumEntry* FindEnumByName(StringView entryName) const;
    /// Reads/writes the integral value of an enum (or integer) object.
    i64 ReadEnumValue(const void* object) const;
    void WriteEnumValue(void* object, i64 value) const;
};

namespace Reflection {

/// Registers a type for lookup by name (done automatically by the registration macros).
void RegisterType(const TypeInfo* type);
const TypeInfo* FindType(Name name);
const TypeInfo* FindType(StringView name);
inline const TypeInfo* FindType(const char* name) { return FindType(StringView(name)); }
/// All registered types (struct/enum/builtin).
Array<const TypeInfo*> GetAllTypes();
/// All registered struct types deriving from `base`.
Array<const TypeInfo*> GetDerivedTypes(const TypeInfo& base);

/// "m_fieldName"/"fieldName"/"FieldName" -> "Field Name".
String MakeDisplayName(StringView identifier);

/// Deep equality of two values of `type`.
bool ValuesEqual(const void* a, const void* b, const TypeInfo& type);
/// Hash of the type layout (names, kinds, nested schema). Changes whenever the serialized form changes.
u64 ComputeSchemaHash(const TypeInfo& type);

/// Enum/flags to text: "Value" or "A|B" for flags. Unknown values print as numbers.
String EnumToString(const TypeInfo& enumType, i64 value);
bool EnumFromString(const TypeInfo& enumType, StringView text, i64& outValue);

} // namespace Reflection
} // namespace zx
