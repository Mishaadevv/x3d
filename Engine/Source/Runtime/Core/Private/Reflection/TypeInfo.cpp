#include "Core/Containers/HashMap.h"
#include "Core/Reflection/Reflection.h"
#include "Core/String/Parse.h"

#include "Windows/WindowsInclude.h"

namespace zx {

// --- Meta -----------------------------------------------------------------------------------------------------------

namespace Meta {

#define ZX_META_KEY(Fn, Text)                                                                                          \
    Name Fn()                                                                                                          \
    {                                                                                                                  \
        static const Name n(Text);                                                                                     \
        return n;                                                                                                      \
    }
ZX_META_KEY(KeyTooltip, "Tooltip")
ZX_META_KEY(KeyCategory, "Category")
ZX_META_KEY(KeyDisplayName, "DisplayName")
ZX_META_KEY(KeyRange, "Range")
ZX_META_KEY(KeyStep, "Step")
ZX_META_KEY(KeyUnits, "Units")
ZX_META_KEY(KeyHidden, "Hidden")
ZX_META_KEY(KeyReadOnly, "ReadOnly")
ZX_META_KEY(KeyTransient, "Transient")
ZX_META_KEY(KeyColor, "Color")
ZX_META_KEY(KeyHDR, "HDR")
ZX_META_KEY(KeyMultiline, "Multiline")
ZX_META_KEY(KeyEditCondition, "EditCondition")
ZX_META_KEY(KeyFormerName, "FormerName")
ZX_META_KEY(KeyAngle, "Angle")
ZX_META_KEY(KeyAssetType, "AssetType")
#undef ZX_META_KEY

namespace {
MetaEntry Text(Name key, StringView text)
{
    MetaEntry e;
    e.key = key;
    e.value.kind = MetaValue::Kind::Text;
    e.value.text = text;
    return e;
}
MetaEntry Flag(Name key)
{
    MetaEntry e;
    e.key = key;
    return e;
}
} // namespace

MetaEntry Tooltip(StringView text) { return Text(KeyTooltip(), text); }
MetaEntry Category(StringView text) { return Text(KeyCategory(), text); }
MetaEntry DisplayName(StringView text) { return Text(KeyDisplayName(), text); }
MetaEntry Range(f64 min, f64 max)
{
    MetaEntry e;
    e.key = KeyRange();
    e.value.kind = MetaValue::Kind::Number;
    e.value.number = min;
    e.value.number2 = max;
    return e;
}
MetaEntry Step(f64 step)
{
    MetaEntry e;
    e.key = KeyStep();
    e.value.kind = MetaValue::Kind::Number;
    e.value.number = step;
    return e;
}
MetaEntry Units(StringView units) { return Text(KeyUnits(), units); }
MetaEntry Hidden() { return Flag(KeyHidden()); }
MetaEntry ReadOnly() { return Flag(KeyReadOnly()); }
MetaEntry Transient() { return Flag(KeyTransient()); }
MetaEntry Color() { return Flag(KeyColor()); }
MetaEntry HDR() { return Flag(KeyHDR()); }
MetaEntry Multiline() { return Flag(KeyMultiline()); }
MetaEntry EditCondition(StringView field) { return Text(KeyEditCondition(), field); }
MetaEntry FormerName(StringView oldName) { return Text(KeyFormerName(), oldName); }
MetaEntry Angle() { return Flag(KeyAngle()); }
MetaEntry AssetType(StringView typeName) { return Text(KeyAssetType(), typeName); }
MetaEntry Custom(StringView key, StringView text) { return Text(Name(key), text); }

} // namespace Meta

// --- TypeInfo -------------------------------------------------------------------------------------------------------

bool TypeInfo::IsA(const TypeInfo& other) const
{
    for (const TypeInfo* t = this; t; t = t->base)
        if (t == &other)
            return true;
    return false;
}

const FieldInfo* TypeInfo::FindField(Name fieldName) const
{
    for (const TypeInfo* t = this; t; t = t->base) {
        for (const FieldInfo& f : t->fields)
            if (f.name == fieldName)
                return &f;
    }
    // Renamed fields.
    for (const TypeInfo* t = this; t; t = t->base) {
        for (const FieldInfo& f : t->fields)
            if (const MetaValue* former = f.meta.Find(Meta::KeyFormerName()); former && former->text == fieldName.View())
                return &f;
    }
    return nullptr;
}

const MethodInfo* TypeInfo::FindMethod(Name methodName) const
{
    for (const TypeInfo* t = this; t; t = t->base)
        for (const MethodInfo& m : t->methods)
            if (m.name == methodName)
                return &m;
    return nullptr;
}

const EnumEntry* TypeInfo::FindEnumByValue(i64 value) const
{
    for (const EnumEntry& e : enumEntries)
        if (e.value == value)
            return &e;
    return nullptr;
}

const EnumEntry* TypeInfo::FindEnumByName(StringView entryName) const
{
    for (const EnumEntry& e : enumEntries)
        if (e.name.View() == entryName)
            return &e;
    for (const EnumEntry& e : enumEntries)
        if (e.name.View().EqualsIgnoreCase(entryName))
            return &e;
    return nullptr;
}

i64 TypeInfo::ReadEnumValue(const void* object) const
{
    const TypeInfo* t = kind == TypeKind::Enum ? underlying : this;
    switch (t->kind) {
    case TypeKind::Int8:
        return *static_cast<const i8*>(object);
    case TypeKind::Int16:
        return *static_cast<const i16*>(object);
    case TypeKind::Int32:
        return *static_cast<const i32*>(object);
    case TypeKind::Int64:
        return *static_cast<const i64*>(object);
    case TypeKind::UInt8:
        return *static_cast<const u8*>(object);
    case TypeKind::UInt16:
        return *static_cast<const u16*>(object);
    case TypeKind::UInt32:
        return *static_cast<const u32*>(object);
    case TypeKind::UInt64:
        return i64(*static_cast<const u64*>(object));
    default:
        return 0;
    }
}

void TypeInfo::WriteEnumValue(void* object, i64 value) const
{
    const TypeInfo* t = kind == TypeKind::Enum ? underlying : this;
    switch (t->kind) {
    case TypeKind::Int8:
        *static_cast<i8*>(object) = i8(value);
        break;
    case TypeKind::Int16:
        *static_cast<i16*>(object) = i16(value);
        break;
    case TypeKind::Int32:
        *static_cast<i32*>(object) = i32(value);
        break;
    case TypeKind::Int64:
        *static_cast<i64*>(object) = value;
        break;
    case TypeKind::UInt8:
        *static_cast<u8*>(object) = u8(value);
        break;
    case TypeKind::UInt16:
        *static_cast<u16*>(object) = u16(value);
        break;
    case TypeKind::UInt32:
        *static_cast<u32*>(object) = u32(value);
        break;
    case TypeKind::UInt64:
        *static_cast<u64*>(object) = u64(value);
        break;
    default:
        break;
    }
}

// --- Registry & building --------------------------------------------------------------------------------------------

namespace {

struct Registry
{
    CRITICAL_SECTION lock; // recursive: type building may resolve other types
    HashMap<Name, const TypeInfo*> byName;
    Array<const TypeInfo*> all;

    Registry() { ::InitializeCriticalSection(&lock); }
};

Registry& GetRegistry()
{
    // Leaked on purpose: types are referenced until process exit.
    static Registry* r = [] {
        MemTagScope tag(MemTag::Reflection);
        LeakIgnoreScope permanent;
        return new Registry();
    }();
    return *r;
}

struct RegistryLock
{
    RegistryLock() { ::EnterCriticalSection(&GetRegistry().lock); }
    ~RegistryLock() { ::LeaveCriticalSection(&GetRegistry().lock); }
};

TypeInfo* MakeBuiltin(TypeKind kind, StringView name, u32 size, u32 align)
{
    TypeInfo* t = new TypeInfo();
    detail::InitTypeName(*t, name, kind);
    t->size = size;
    t->alignment = align;
    return t;
}

template <typename T>
TypeInfo* MakeBuiltinT(TypeKind kind, StringView name)
{
    TypeInfo* t = MakeBuiltin(kind, name, sizeof(T), alignof(T));
    detail::FillLifecycle<T>(*t);
    return t;
}

} // namespace

namespace detail {

void InitTypeName(TypeInfo& info, StringView name, TypeKind kind)
{
    info.name = Name(name);
    info.displayName = Reflection::MakeDisplayName(name);
    info.kind = kind;
    info.id = HashXX64(name);
}

void ApplyFieldMeta(FieldInfo& f)
{
    if (f.meta.Has(Meta::KeyHidden()))
        f.flags |= FieldFlags::Hidden;
    if (f.meta.Has(Meta::KeyReadOnly()))
        f.flags |= FieldFlags::ReadOnly;
    if (f.meta.Has(Meta::KeyTransient()))
        f.flags |= FieldFlags::Transient;
    if (const MetaValue* dn = f.meta.Find(Meta::KeyDisplayName()))
        f.displayName = dn->text;
}

const TypeInfo* GetBuiltinType(TypeKind kind)
{
    LeakIgnoreScope permanent;
    static const TypeInfo* table[] = {
        [] {
            TypeInfo* t = MakeBuiltin(TypeKind::Void, "void", 0, 1);
            return t;
        }(),
        MakeBuiltinT<bool>(TypeKind::Bool, "bool"),
        MakeBuiltinT<i8>(TypeKind::Int8, "i8"),
        MakeBuiltinT<i16>(TypeKind::Int16, "i16"),
        MakeBuiltinT<i32>(TypeKind::Int32, "i32"),
        MakeBuiltinT<i64>(TypeKind::Int64, "i64"),
        MakeBuiltinT<u8>(TypeKind::UInt8, "u8"),
        MakeBuiltinT<u16>(TypeKind::UInt16, "u16"),
        MakeBuiltinT<u32>(TypeKind::UInt32, "u32"),
        MakeBuiltinT<u64>(TypeKind::UInt64, "u64"),
        MakeBuiltinT<f32>(TypeKind::Float, "f32"),
        MakeBuiltinT<f64>(TypeKind::Double, "f64"),
        MakeBuiltinT<String>(TypeKind::String, "String"),
        MakeBuiltinT<Name>(TypeKind::Name, "Name"),
        MakeBuiltinT<Guid>(TypeKind::Guid, "Guid"),
    };
    static const bool registered = [] {
        for (const TypeInfo* t : table)
            Reflection::RegisterType(t);
        return true;
    }();
    ZX_UNUSED(registered);
    ZX_ASSERT(usize(kind) < ZX_ARRAY_COUNT(table), "Not a builtin type kind");
    return table[usize(kind)];
}

const TypeInfo& GetOrBuildType(LazyType& lazy, TypeFillFn fill)
{
    if (TypeInfo* t = lazy.ready.load(std::memory_order_acquire))
        return *t;
    RegistryLock lock;
    if (TypeInfo* t = lazy.ready.load(std::memory_order_acquire))
        return *t;
    if (lazy.building)
        return *lazy.building; // recursive reference while building (same thread holds the lock)
    MemTagScope tag(MemTag::Reflection);
    LeakIgnoreScope permanent;
    TypeInfo* t = new TypeInfo();
    lazy.building = t;
    fill(*t);
    Reflection::RegisterType(t);
    lazy.ready.store(t, std::memory_order_release);
    lazy.building = nullptr;
    return *t;
}

TypeRegistrar::TypeRegistrar(const TypeInfo& (*getter)()) { getter(); }

} // namespace detail

namespace Reflection {

void RegisterType(const TypeInfo* type)
{
    RegistryLock lock;
    Registry& r = GetRegistry();
    MemTagScope tag(MemTag::Reflection);
    LeakIgnoreScope permanent;
    if (const TypeInfo** existing = r.byName.Find(type->name)) {
        ZX_ASSERT(*existing == type, "Duplicate reflected type name '{}'", type->name);
        return;
    }
    r.byName.Insert(type->name, type);
    r.all.PushBack(type);
}

const TypeInfo* FindType(Name name)
{
    detail::GetBuiltinType(TypeKind::Void); // ensure builtins are registered
    RegistryLock lock;
    const TypeInfo** t = GetRegistry().byName.Find(name);
    return t ? *t : nullptr;
}

const TypeInfo* FindType(StringView name)
{
    const Name n = Name::Find(name);
    return n.IsNone() ? nullptr : FindType(n);
}

Array<const TypeInfo*> GetAllTypes()
{
    RegistryLock lock;
    return GetRegistry().all;
}

Array<const TypeInfo*> GetDerivedTypes(const TypeInfo& base)
{
    RegistryLock lock;
    Array<const TypeInfo*> out;
    for (const TypeInfo* t : GetRegistry().all)
        if (t != &base && t->IsStruct() && t->IsA(base))
            out.PushBack(t);
    return out;
}

String MakeDisplayName(StringView id)
{
    if (id.StartsWith("m_"))
        id = id.DropLeft(2);
    else if (id.Size() > 1 && (id[0] == 'b' || id[0] == 'k') && id[1] >= 'A' && id[1] <= 'Z')
        id = id.DropLeft(1); // bEnabled -> Enabled, kValue -> Value
    String out;
    out.Reserve(id.Size() + 8);
    for (usize i = 0; i < id.Size(); ++i) {
        char c = id[i];
        if (c == '_') {
            if (!out.Empty() && out.Back() != ' ')
                out += ' ';
            continue;
        }
        const bool upper = c >= 'A' && c <= 'Z';
        const bool digit = IsDigitAscii(c);
        if (i > 0 && !out.Empty() && out.Back() != ' ') {
            const char prev = id[i - 1];
            const bool prevLower = prev >= 'a' && prev <= 'z';
            const bool prevUpper = prev >= 'A' && prev <= 'Z';
            const bool prevDigit = IsDigitAscii(prev);
            const bool nextLower = i + 1 < id.Size() && id[i + 1] >= 'a' && id[i + 1] <= 'z';
            if ((upper && (prevLower || prevDigit)) || (upper && prevUpper && nextLower) || (digit && !prevDigit && !prevUpper))
                out += ' ';
        }
        if (out.Empty() || out.Back() == ' ')
            c = ToUpperAscii(c); // capitalize the first letter of every word
        out += c;
    }
    return out;
}

bool ValuesEqual(const void* a, const void* b, const TypeInfo& type)
{
    switch (type.kind) {
    case TypeKind::Void:
        return true;
    case TypeKind::Bool:
        return *static_cast<const bool*>(a) == *static_cast<const bool*>(b);
    case TypeKind::Float:
        return *static_cast<const f32*>(a) == *static_cast<const f32*>(b);
    case TypeKind::Double:
        return *static_cast<const f64*>(a) == *static_cast<const f64*>(b);
    case TypeKind::String:
        return *static_cast<const String*>(a) == *static_cast<const String*>(b);
    case TypeKind::Name:
        return *static_cast<const Name*>(a) == *static_cast<const Name*>(b);
    case TypeKind::Guid:
        return *static_cast<const Guid*>(a) == *static_cast<const Guid*>(b);
    case TypeKind::Enum:
        return type.ReadEnumValue(a) == type.ReadEnumValue(b);
    case TypeKind::Array: {
        const usize n = type.arrayOps->size(a);
        if (n != type.arrayOps->size(b))
            return false;
        for (usize i = 0; i < n; ++i)
            if (!ValuesEqual(type.arrayOps->atConst(a, i), type.arrayOps->atConst(b, i), *type.element))
                return false;
        return true;
    }
    case TypeKind::Struct: {
        bool equal = true;
        type.ForEachField([&](const FieldInfo& f) {
            if (equal && !ValuesEqual(f.Ptr(a), f.Ptr(b), *f.type))
                equal = false;
        });
        return equal;
    }
    default:
        if (IsIntegerKind(type.kind))
            return MemCompare(a, b, type.size) == 0;
        return false;
    }
}

namespace {
u64 SchemaHashImpl(const TypeInfo& type, SmallArray<const TypeInfo*, 16>& visiting)
{
    u64 h = HashCombine(type.id, u64(type.kind));
    if (visiting.Contains(&type))
        return h; // recursive reference (e.g. Node containing Array<Node>): identified by name only
    visiting.PushBack(&type);
    switch (type.kind) {
    case TypeKind::Struct:
        type.ForEachField([&](const FieldInfo& f) {
            if (f.IsTransient())
                return;
            h = HashCombine(h, HashXX64(f.name.View()));
            h = HashCombine(h, SchemaHashImpl(*f.type, visiting));
        });
        break;
    case TypeKind::Array:
        h = HashCombine(h, SchemaHashImpl(*type.element, visiting));
        break;
    case TypeKind::Enum:
        h = HashCombine(h, u64(type.underlying->kind));
        break;
    default:
        break;
    }
    visiting.PopBack();
    return h;
}
} // namespace

u64 ComputeSchemaHash(const TypeInfo& type)
{
    SmallArray<const TypeInfo*, 16> visiting;
    return SchemaHashImpl(type, visiting);
}

String EnumToString(const TypeInfo& enumType, i64 value)
{
    if (const EnumEntry* e = enumType.FindEnumByValue(value))
        return String(e->name.View());
    if (enumType.isFlags) {
        String out;
        i64 remaining = value;
        for (const EnumEntry& e : enumType.enumEntries) {
            if (e.value != 0 && (remaining & e.value) == e.value) {
                if (!out.Empty())
                    out += '|';
                out += e.name.View();
                remaining &= ~e.value;
            }
        }
        if (remaining == 0 && !out.Empty())
            return out;
    }
    return String::Format("{}", value);
}

bool EnumFromString(const TypeInfo& enumType, StringView text, i64& outValue)
{
    text = text.Trim();
    if (const EnumEntry* e = enumType.FindEnumByName(text)) {
        outValue = e->value;
        return true;
    }
    if (auto n = ParseInt(text)) {
        outValue = *n;
        return true;
    }
    if (enumType.isFlags && text.Contains('|')) {
        i64 v = 0;
        for (StringView part : SplitString(text, '|', true)) {
            const EnumEntry* e = enumType.FindEnumByName(part.Trim());
            if (!e)
                return false;
            v |= e->value;
        }
        outValue = v;
        return true;
    }
    return false;
}

} // namespace Reflection
} // namespace zx

namespace {
// Build builtin types during static initialization (single-threaded), avoiding lazy-init races later.
const bool g_builtinsReady = (zx::detail::GetBuiltinType(zx::TypeKind::Void), true);
} // namespace
