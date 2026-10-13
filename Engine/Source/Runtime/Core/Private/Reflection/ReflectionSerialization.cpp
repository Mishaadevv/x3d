#include "Core/Reflection/ReflectionSerialization.h"
#include "Core/Containers/HashMap.h"
#include "Core/String/Parse.h"
#include "Core/Threading/Mutex.h"

namespace zx::Reflection {

namespace {

Name CompactKey()
{
    static const Name n("Compact");
    return n;
}

bool IsCompactStruct(const TypeInfo& t) { return t.IsStruct() && t.meta.Has(CompactKey()); }

void Warn(Array<String>* warnings, String message)
{
    if (warnings)
        warnings->PushBack(move(message));
}

bool WriteNumberFromJson(const JsonValue& j, void* p, const TypeInfo& type)
{
    if (!j.IsNumber() && !j.IsBool())
        return false;
    if (IsFloatKind(type.kind)) {
        const f64 d = j.IsBool() ? (j.AsBool() ? 1.0 : 0.0) : j.AsDouble();
        if (type.kind == TypeKind::Float)
            *static_cast<f32*>(p) = f32(d);
        else
            *static_cast<f64*>(p) = d;
        return true;
    }
    const i64 v = j.IsBool() ? (j.AsBool() ? 1 : 0) : j.AsInt();
    type.WriteEnumValue(p, v); // handles every integer kind
    return true;
}

} // namespace

const void* GetDefaultObject(const TypeInfo& type)
{
    if (type.kind == TypeKind::Void || !type.construct)
        return nullptr;
    LeakIgnoreScope permanent; // default objects are cached for the process lifetime
    static Mutex s_mutex;
    static HashMap<const TypeInfo*, void*>* s_defaults = nullptr;
    ScopedLock<Mutex> guard(s_mutex);
    if (!s_defaults) {
        MemTagScope tag(MemTag::Reflection);
        s_defaults = new HashMap<const TypeInfo*, void*>(); // leaked intentionally
    }
    if (void** found = s_defaults->Find(&type))
        return *found;
    MemTagScope tag(MemTag::Reflection);
    void* obj = Malloc(type.size ? type.size : 1, type.alignment, MemTag::Reflection);
    type.construct(obj);
    s_defaults->Insert(&type, obj);
    return obj;
}

void CopyValue(void* dst, const void* src, const TypeInfo& type)
{
    if (type.copyAssign)
        type.copyAssign(dst, src);
}

// --- JSON -----------------------------------------------------------------------------------------------------------

JsonValue ToJson(const void* object, const TypeInfo& type, const void* defaults)
{
    switch (type.kind) {
    case TypeKind::Void:
        return JsonValue();
    case TypeKind::Bool:
        return JsonValue(*static_cast<const bool*>(object));
    case TypeKind::Float:
        return JsonValue(*static_cast<const f32*>(object));
    case TypeKind::Double:
        return JsonValue(*static_cast<const f64*>(object));
    case TypeKind::UInt64:
        return JsonValue(*static_cast<const u64*>(object));
    case TypeKind::String:
        return JsonValue(static_cast<const String*>(object)->View());
    case TypeKind::Name:
        return JsonValue(static_cast<const Name*>(object)->View());
    case TypeKind::Guid: {
        char buf[37];
        static_cast<const Guid*>(object)->ToChars(buf);
        return JsonValue(StringView(buf, 36));
    }
    case TypeKind::Enum:
        return JsonValue(EnumToString(type, type.ReadEnumValue(object)).View());
    case TypeKind::Array: {
        JsonValue arr = JsonValue::MakeArray();
        const usize n = type.arrayOps->size(object);
        for (usize i = 0; i < n; ++i)
            arr.PushBack(ToJson(type.arrayOps->atConst(object, i), *type.element, nullptr));
        return arr;
    }
    case TypeKind::Struct: {
        if (IsCompactStruct(type)) {
            JsonValue arr = JsonValue::MakeArray();
            type.ForEachField([&](const FieldInfo& f) { arr.PushBack(ToJson(f.Ptr(object), *f.type, nullptr)); });
            return arr;
        }
        JsonValue obj = JsonValue::MakeObject();
        type.ForEachField([&](const FieldInfo& f) {
            if (f.IsTransient())
                return;
            const void* fieldDefault = defaults ? f.Ptr(defaults) : nullptr;
            if (fieldDefault && ValuesEqual(f.Ptr(object), fieldDefault, *f.type))
                return;
            obj.Set(f.name.View(), ToJson(f.Ptr(object), *f.type, fieldDefault));
        });
        return obj;
    }
    default:
        if (IsIntegerKind(type.kind))
            return JsonValue(type.ReadEnumValue(object));
        return JsonValue();
    }
}

bool FromJson(const JsonValue& j, void* object, const TypeInfo& type, Array<String>* warnings)
{
    switch (type.kind) {
    case TypeKind::Void:
        return true;
    case TypeKind::Bool:
        if (!j.IsBool() && !j.IsNumber()) {
            Warn(warnings, String::Format("Expected bool for {}", type.name));
            return false;
        }
        *static_cast<bool*>(object) = j.IsBool() ? j.AsBool() : j.AsDouble() != 0.0;
        return true;
    case TypeKind::String:
        if (!j.IsString()) {
            Warn(warnings, "Expected string");
            return false;
        }
        *static_cast<String*>(object) = j.AsString();
        return true;
    case TypeKind::Name:
        if (!j.IsString()) {
            Warn(warnings, "Expected string for Name");
            return false;
        }
        *static_cast<Name*>(object) = Name(j.AsString());
        return true;
    case TypeKind::Guid: {
        const Optional<Guid> g = j.IsString() ? Guid::Parse(j.AsString()) : NullOpt;
        if (!g) {
            Warn(warnings, "Expected GUID string");
            return false;
        }
        *static_cast<Guid*>(object) = *g;
        return true;
    }
    case TypeKind::Enum: {
        i64 v = 0;
        bool ok = false;
        if (j.IsString())
            ok = EnumFromString(type, j.AsString(), v);
        else if (j.IsNumber()) {
            v = j.AsInt();
            ok = true;
        }
        if (!ok) {
            Warn(warnings, String::Format("Invalid value '{}' for enum {}", j.AsString(), type.name));
            return false;
        }
        type.WriteEnumValue(object, v);
        return true;
    }
    case TypeKind::Array: {
        if (!j.IsArray()) {
            Warn(warnings, String::Format("Expected array for {}", type.name));
            return false;
        }
        const usize n = j.Size();
        type.arrayOps->resize(object, n);
        bool ok = true;
        for (usize i = 0; i < n; ++i)
            ok &= FromJson(j[i], type.arrayOps->at(object, i), *type.element, warnings);
        return ok;
    }
    case TypeKind::Struct: {
        if (j.IsArray()) {
            // Compact positional form.
            usize i = 0;
            bool ok = true;
            type.ForEachField([&](const FieldInfo& f) {
                if (i < j.Size())
                    ok &= FromJson(j[i], f.Ptr(object), *f.type, warnings);
                ++i;
            });
            return ok;
        }
        if (!j.IsObject()) {
            Warn(warnings, String::Format("Expected object for {}", type.name));
            return false;
        }
        bool ok = true;
        for (const JsonMember& m : j.Members()) {
            const Name fieldName = Name::Find(m.first.View());
            const FieldInfo* f = fieldName.IsNone() ? nullptr : type.FindField(fieldName);
            if (!f) {
                // Former names are stored as text and may not be interned yet.
                type.ForEachField([&](const FieldInfo& candidate) {
                    if (!f && candidate.meta.GetText(Meta::KeyFormerName()) == m.first.View())
                        f = &candidate;
                });
            }
            if (!f) {
                Warn(warnings, String::Format("Unknown field '{}' in {}", m.first, type.name));
                continue;
            }
            if (f->IsTransient())
                continue;
            ok &= FromJson(m.second, f->Ptr(object), *f->type, warnings);
        }
        return ok;
    }
    default:
        if (IsNumericKind(type.kind)) {
            if (!WriteNumberFromJson(j, object, type)) {
                Warn(warnings, String::Format("Expected number for {}", type.name));
                return false;
            }
            return true;
        }
        return false;
    }
}

// --- Binary ---------------------------------------------------------------------------------------------------------

void WriteBinary(BinaryWriter& w, const void* object, const TypeInfo& type)
{
    switch (type.kind) {
    case TypeKind::Void:
        return;
    case TypeKind::String:
        w.WriteString(static_cast<const String*>(object)->View());
        return;
    case TypeKind::Name:
        w.WriteString(static_cast<const Name*>(object)->View());
        return;
    case TypeKind::Enum:
        w.WriteVarI64(type.ReadEnumValue(object));
        return;
    case TypeKind::Array: {
        const usize n = type.arrayOps->size(object);
        w.WriteVarU64(n);
        for (usize i = 0; i < n; ++i)
            WriteBinary(w, type.arrayOps->atConst(object, i), *type.element);
        return;
    }
    case TypeKind::Struct:
        type.ForEachField([&](const FieldInfo& f) {
            if (!f.IsTransient())
                WriteBinary(w, f.Ptr(object), *f.type);
        });
        return;
    default:
        // Bool, numbers, Guid: raw bytes.
        w.WriteBytes(object, type.size);
        return;
    }
}

bool ReadBinary(BinaryReader& r, void* object, const TypeInfo& type)
{
    switch (type.kind) {
    case TypeKind::Void:
        return true;
    case TypeKind::String:
        *static_cast<String*>(object) = r.ReadStringView();
        break;
    case TypeKind::Name:
        *static_cast<Name*>(object) = Name(r.ReadStringView());
        break;
    case TypeKind::Enum:
        type.WriteEnumValue(object, r.ReadVarI64());
        break;
    case TypeKind::Array: {
        const u64 n = r.ReadVarU64();
        if (r.HasError() || n > r.Remaining())
            return false; // every element takes at least one byte except Void/empty structs
        type.arrayOps->resize(object, usize(n));
        for (usize i = 0; i < n && !r.HasError(); ++i)
            ReadBinary(r, type.arrayOps->at(object, i), *type.element);
        break;
    }
    case TypeKind::Struct:
        type.ForEachField([&](const FieldInfo& f) {
            if (!f.IsTransient() && !r.HasError())
                ReadBinary(r, f.Ptr(object), *f.type);
        });
        break;
    case TypeKind::Bool: {
        u8 b = 0;
        r.Read(b);
        *static_cast<bool*>(object) = b != 0;
        break;
    }
    default:
        r.ReadBytes(object, type.size);
        break;
    }
    return !r.HasError();
}

// --- Text -----------------------------------------------------------------------------------------------------------

String ValueToString(const void* object, const TypeInfo& type)
{
    switch (type.kind) {
    case TypeKind::Bool:
        return String(*static_cast<const bool*>(object) ? "true" : "false");
    case TypeKind::String:
        return *static_cast<const String*>(object);
    case TypeKind::Name:
        return String(static_cast<const Name*>(object)->View());
    case TypeKind::Guid:
        return String::Format("{}", *static_cast<const Guid*>(object));
    case TypeKind::Enum:
        return EnumToString(type, type.ReadEnumValue(object));
    case TypeKind::Float:
        return String::Format("{}", *static_cast<const f32*>(object));
    case TypeKind::Double:
        return String::Format("{}", *static_cast<const f64*>(object));
    case TypeKind::UInt64:
        return String::Format("{}", *static_cast<const u64*>(object));
    default:
        if (IsIntegerKind(type.kind))
            return String::Format("{}", type.ReadEnumValue(object));
        return ToJsonString(ToJson(object, type), JsonWriteOptions{.pretty = false});
    }
}

bool ValueFromString(StringView text, void* object, const TypeInfo& type)
{
    switch (type.kind) {
    case TypeKind::Bool: {
        const auto b = ParseBool(text);
        if (!b)
            return false;
        *static_cast<bool*>(object) = *b;
        return true;
    }
    case TypeKind::String:
        *static_cast<String*>(object) = text;
        return true;
    case TypeKind::Name:
        *static_cast<Name*>(object) = Name(text.Trim());
        return true;
    case TypeKind::Guid: {
        const auto g = Guid::Parse(text);
        if (!g)
            return false;
        *static_cast<Guid*>(object) = *g;
        return true;
    }
    case TypeKind::Enum: {
        i64 v;
        if (!EnumFromString(type, text, v))
            return false;
        type.WriteEnumValue(object, v);
        return true;
    }
    case TypeKind::Float: {
        const auto f = ParseFloat(text);
        if (!f)
            return false;
        *static_cast<f32*>(object) = *f;
        return true;
    }
    case TypeKind::Double: {
        const auto d = ParseDouble(text);
        if (!d)
            return false;
        *static_cast<f64*>(object) = *d;
        return true;
    }
    case TypeKind::UInt64: {
        const auto u = ParseUInt(text);
        if (!u)
            return false;
        *static_cast<u64*>(object) = *u;
        return true;
    }
    default:
        if (IsIntegerKind(type.kind)) {
            const auto i = ParseInt(text);
            if (!i)
                return false;
            type.WriteEnumValue(object, *i);
            return true;
        }
        JsonValue j;
        return ParseJson(text, j) && FromJson(j, object, type);
    }
}

} // namespace zx::Reflection
