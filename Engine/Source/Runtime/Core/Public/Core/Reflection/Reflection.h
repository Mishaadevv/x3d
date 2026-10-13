#pragma once

// ZRefl registration API.
//
// Structs/classes (header):
//     struct LightComponent
//     {
//         ZX_REFLECT_STRUCT(LightComponent)
//         LinearColor color = LinearColor::White();
//         f32 intensity = 10.0f;
//         LightType type = LightType::Point;
//         void Flicker(f32 amount);
//     };
// (.cpp, in the same namespace as the type):
//     ZX_REFLECT_STRUCT_BEGIN(LightComponent)
//         ZX_FIELD(color, Meta::Color(), Meta::HDR())
//         ZX_FIELD(intensity, Meta::Range(0, 100000), Meta::Units("lm"))
//         ZX_FIELD(type)
//         ZX_METHOD(Flicker, "amount")
//     ZX_REFLECT_STRUCT_END()
//
// Derived types: ZX_REFLECT_STRUCT_BEGIN(Derived) ZX_BASE(Base) ... (Base must be the first base class).
// Types you don't own (header, same namespace as the type): ZX_DECLARE_TYPE(Vec3); then ZX_REFLECT_STRUCT_BEGIN.
//
// Enums (header, same namespace as the enum):   ZX_DECLARE_ENUM(LightType);
// (.cpp):  ZX_REFLECT_ENUM_BEGIN(LightType)  ZX_ENUM_VALUE(Point)  ZX_ENUM_VALUE(Spot)  ZX_REFLECT_ENUM_END()
//
// Lookup:  const TypeInfo& t = TypeOf<LightComponent>();   Reflection::FindType("LightComponent");
// Types are found through argument-dependent lookup of ZxTypeOf(T*), so they work in any namespace.

#include "Core/Hash/Guid.h"
#include "Core/Reflection/TypeInfo.h"
#include "Core/String/String.h"

#include <atomic>

namespace zx {

template <typename T, typename = void>
struct TypeResolver
{
    static const TypeInfo* Get() { return &ZxTypeOf(static_cast<T*>(nullptr)); }
};

template <typename T>
const TypeInfo& TypeOf()
{
    return *TypeResolver<std::remove_cv_t<std::remove_reference_t<T>>>::Get();
}

template <typename T>
concept Reflected = requires { TypeResolver<std::remove_cv_t<T>>::Get(); };

namespace detail {

const TypeInfo* GetBuiltinType(TypeKind kind);

template <typename T>
constexpr TypeKind IntegerKind()
{
    if constexpr (std::is_signed_v<T>)
        return sizeof(T) == 1 ? TypeKind::Int8 : sizeof(T) == 2 ? TypeKind::Int16 : sizeof(T) == 4 ? TypeKind::Int32 : TypeKind::Int64;
    else
        return sizeof(T) == 1 ? TypeKind::UInt8 : sizeof(T) == 2 ? TypeKind::UInt16 : sizeof(T) == 4 ? TypeKind::UInt32 : TypeKind::UInt64;
}

template <typename T>
void FillLifecycle(TypeInfo& t)
{
    t.size = u32(sizeof(T));
    t.alignment = u32(alignof(T));
    t.construct = [](void* p) { ::new (p) T(); };
    t.destruct = [](void* p) { static_cast<T*>(p)->~T(); };
    t.copyAssign = [](void* d, const void* s) { *static_cast<T*>(d) = *static_cast<const T*>(s); };
}

/// Lazily-built type storage (constant-initialized). Supports recursive references during building.
struct LazyType
{
    std::atomic<TypeInfo*> ready{nullptr};
    TypeInfo* building = nullptr;
};

using TypeFillFn = void (*)(TypeInfo& info);
const TypeInfo& GetOrBuildType(LazyType& lazy, TypeFillFn fill);

/// Registers a type at static-initialization time.
struct TypeRegistrar
{
    explicit TypeRegistrar(const TypeInfo& (*getter)());
};

template <typename T>
const TypeInfo* MakeArrayType();

} // namespace detail

// --- Builtin resolvers ----------------------------------------------------------------------------------------------
template <>
struct TypeResolver<bool>
{
    static const TypeInfo* Get() { return detail::GetBuiltinType(TypeKind::Bool); }
};
template <typename T>
struct TypeResolver<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>>>
{
    static const TypeInfo* Get() { return detail::GetBuiltinType(detail::IntegerKind<T>()); }
};
template <>
struct TypeResolver<f32>
{
    static const TypeInfo* Get() { return detail::GetBuiltinType(TypeKind::Float); }
};
template <>
struct TypeResolver<f64>
{
    static const TypeInfo* Get() { return detail::GetBuiltinType(TypeKind::Double); }
};
template <>
struct TypeResolver<String>
{
    static const TypeInfo* Get() { return detail::GetBuiltinType(TypeKind::String); }
};
template <>
struct TypeResolver<Name>
{
    static const TypeInfo* Get() { return detail::GetBuiltinType(TypeKind::Name); }
};
template <>
struct TypeResolver<Guid>
{
    static const TypeInfo* Get() { return detail::GetBuiltinType(TypeKind::Guid); }
};
template <>
struct TypeResolver<void>
{
    static const TypeInfo* Get() { return detail::GetBuiltinType(TypeKind::Void); }
};
template <typename T>
struct TypeResolver<Array<T>>
{
    static const TypeInfo* Get()
    {
        static const TypeInfo* type = detail::MakeArrayType<T>();
        return type;
    }
};

// --- Builders -------------------------------------------------------------------------------------------------------

namespace detail {

void InitTypeName(TypeInfo& info, StringView name, TypeKind kind);
void ApplyFieldMeta(FieldInfo& field);

template <typename T>
u32 MemberOffset(auto T::*member)
{
    // Address computation on uninitialized storage (no constructor side effects).
    alignas(T) static u8 storage[sizeof(T)];
    const T* object = reinterpret_cast<const T*>(storage);
    return u32(reinterpret_cast<const u8*>(&(object->*member)) - storage);
}

template <typename T>
decltype(auto) ArgFrom(void* p)
{
    return *static_cast<std::remove_cvref_t<T>*>(p);
}

template <auto Fn>
struct MethodTraits;

template <typename C, typename R, typename... A, R (C::*Fn)(A...)>
struct MethodTraits<Fn>
{
    using Class = C;
    using Return = R;
    static constexpr bool IsConst = false;
    static constexpr usize ArgCount = sizeof...(A);
    static void Collect(Array<const TypeInfo*>& out) { (out.PushBack(&TypeOf<std::remove_cvref_t<A>>()), ...); }
    static void Invoke(void* obj, void* const* args, void* result)
    {
        InvokeImpl(obj, args, result, std::index_sequence_for<A...>());
    }
    template <usize... I>
    static void InvokeImpl(void* obj, void* const* args, void* result, std::index_sequence<I...>)
    {
        C* self = static_cast<C*>(obj);
        if constexpr (std::is_void_v<R>) {
            ZX_UNUSED(result);
            (self->*Fn)(ArgFrom<A>(args[I])...);
        } else {
            R value = (self->*Fn)(ArgFrom<A>(args[I])...);
            if (result)
                *static_cast<std::remove_cvref_t<R>*>(result) = move(value);
        }
    }
};

template <typename C, typename R, typename... A, R (C::*Fn)(A...) const>
struct MethodTraits<Fn>
{
    using Class = C;
    using Return = R;
    static constexpr bool IsConst = true;
    static constexpr usize ArgCount = sizeof...(A);
    static void Collect(Array<const TypeInfo*>& out) { (out.PushBack(&TypeOf<std::remove_cvref_t<A>>()), ...); }
    static void Invoke(void* obj, void* const* args, void* result)
    {
        InvokeImpl(obj, args, result, std::index_sequence_for<A...>());
    }
    template <usize... I>
    static void InvokeImpl(void* obj, void* const* args, void* result, std::index_sequence<I...>)
    {
        const C* self = static_cast<const C*>(obj);
        if constexpr (std::is_void_v<R>) {
            ZX_UNUSED(result);
            (self->*Fn)(ArgFrom<A>(args[I])...);
        } else {
            R value = (self->*Fn)(ArgFrom<A>(args[I])...);
            if (result)
                *static_cast<std::remove_cvref_t<R>*>(result) = move(value);
        }
    }
};

} // namespace detail

template <typename T>
class StructBuilder
{
public:
    using ObjectType = T;

    StructBuilder(TypeInfo& info, StringView name) : m_info(info)
    {
        detail::InitTypeName(info, name, TypeKind::Struct);
        detail::FillLifecycle<T>(info);
    }

    template <typename F, typename... M>
    StructBuilder& Field(const char* fieldName, F T::*member, M&&... meta)
    {
        static_assert(Reflected<F>, "Field type is not reflected (declare it with ZX_DECLARE_TYPE / ZX_REFLECT_STRUCT)");
        FieldInfo f;
        f.name = Name(fieldName);
        f.displayName = Reflection::MakeDisplayName(fieldName);
        f.type = &TypeOf<F>();
        f.offset = detail::MemberOffset<T>(member);
        (f.meta.Add(MetaEntry(forward<M>(meta))), ...);
        detail::ApplyFieldMeta(f);
        m_info.fields.PushBack(move(f));
        return *this;
    }

    template <typename B>
    StructBuilder& Base()
    {
        static_assert(std::is_base_of_v<B, T>, "ZX_BASE: not a base class");
        // Base must be at offset 0 so field offsets of the base apply to the derived object.
        alignas(T) static u8 storage[sizeof(T)];
        T* d = reinterpret_cast<T*>(storage);
        ZX_ASSERT(static_cast<void*>(static_cast<B*>(d)) == static_cast<void*>(d), "ZX_BASE must be the first base");
        m_info.base = &TypeOf<B>();
        return *this;
    }

    template <auto Fn, typename... ParamNames>
    StructBuilder& Method(const char* methodName, ParamNames... paramNames)
    {
        using Traits = detail::MethodTraits<Fn>;
        static_assert(sizeof...(ParamNames) == 0 || sizeof...(ParamNames) == Traits::ArgCount,
                      "Parameter name count must match the method arity");
        MethodInfo m;
        m.name = Name(methodName);
        m.displayName = Reflection::MakeDisplayName(methodName);
        m.returnType = &TypeOf<std::remove_cvref_t<typename Traits::Return>>();
        Traits::Collect(m.parameterTypes);
        (m.parameterNames.PushBack(Name(paramNames)), ...);
        m.isConst = Traits::IsConst;
        m.invoke = &Traits::Invoke;
        m_info.methods.PushBack(move(m));
        return *this;
    }

    StructBuilder& Meta(MetaEntry e)
    {
        m_info.meta.Add(move(e));
        return *this;
    }

private:
    TypeInfo& m_info;
};

template <typename E>
class EnumBuilder
{
public:
    using EnumType = E;

    EnumBuilder(TypeInfo& info, StringView name) : m_info(info)
    {
        static_assert(std::is_enum_v<E>);
        detail::InitTypeName(info, name, TypeKind::Enum);
        detail::FillLifecycle<E>(info);
        info.underlying = &TypeOf<std::underlying_type_t<E>>();
    }

    template <typename... M>
    EnumBuilder& Value(const char* entryName, E value, M&&... meta)
    {
        EnumEntry e;
        e.name = Name(entryName);
        e.displayName = Reflection::MakeDisplayName(entryName);
        e.value = i64(value);
        (e.meta.Add(MetaEntry(forward<M>(meta))), ...);
        if (const MetaValue* dn = e.meta.Find(Meta::KeyDisplayName()))
            e.displayName = dn->text;
        m_info.enumEntries.PushBack(move(e));
        return *this;
    }
    EnumBuilder& Flags()
    {
        m_info.isFlags = true;
        return *this;
    }

private:
    TypeInfo& m_info;
};

namespace detail {

template <typename T>
const TypeInfo* MakeArrayType()
{
    static const ArrayOps ops = {
        [](const void* a) -> usize { return static_cast<const Array<T>*>(a)->Size(); },
        [](void* a, usize i) -> void* { return &(*static_cast<Array<T>*>(a))[i]; },
        [](const void* a, usize i) -> const void* { return &(*static_cast<const Array<T>*>(a))[i]; },
        [](void* a, usize n) { static_cast<Array<T>*>(a)->Resize(n); },
        [](void* a) { static_cast<Array<T>*>(a)->Clear(); },
    };
    LeakIgnoreScope permanent;
    TypeInfo* t = New<TypeInfo>();
    const TypeInfo& element = TypeOf<T>();
    InitTypeName(*t, String::Format("Array<{}>", element.name).View(), TypeKind::Array);
    FillLifecycle<Array<T>>(*t);
    t->element = &element;
    t->arrayOps = &ops;
    Reflection::RegisterType(t);
    return t;
}

} // namespace detail
} // namespace zx

// --- Macros ---------------------------------------------------------------------------------------------------------

#define ZX_REFLECT_STRUCT(Type) friend const ::zx::TypeInfo& ZxTypeOf(Type*);

#define ZX_DECLARE_TYPE(Type) const ::zx::TypeInfo& ZxTypeOf(Type*)
#define ZX_DECLARE_ENUM(Type) const ::zx::TypeInfo& ZxTypeOf(Type*)

#define ZX_REFLECT_STRUCT_BEGIN(Type)                                                                                  \
    static const ::zx::detail::TypeRegistrar ZX_UNIQUE_NAME(zxTypeRegistrar_)(                                        \
        []() -> const ::zx::TypeInfo& { return ZxTypeOf(static_cast<Type*>(nullptr)); });                              \
    const ::zx::TypeInfo& ZxTypeOf(Type*)                                                                              \
    {                                                                                                                  \
        static constinit ::zx::detail::LazyType zxLazy;                                                                \
        return ::zx::detail::GetOrBuildType(zxLazy, [](::zx::TypeInfo& zxInfo) {                                      \
            ::zx::StructBuilder<Type> zxBuilder(zxInfo, #Type);                                                        \
            using ZxThis = Type;

#define ZX_REFLECT_STRUCT_END()                                                                                        \
        });                                                                                                            \
    }

#define ZX_FIELD(member, ...) zxBuilder.Field(#member, &ZxThis::member __VA_OPT__(, ) __VA_ARGS__);
#define ZX_BASE(BaseType) zxBuilder.template Base<BaseType>();
#define ZX_METHOD(method, ...) zxBuilder.template Method<&ZxThis::method>(#method __VA_OPT__(, ) __VA_ARGS__);
#define ZX_TYPE_META(...) zxBuilder.Meta(__VA_ARGS__);

#define ZX_REFLECT_ENUM_BEGIN(Type)                                                                                    \
    static const ::zx::detail::TypeRegistrar ZX_UNIQUE_NAME(zxTypeRegistrar_)(                                        \
        []() -> const ::zx::TypeInfo& { return ZxTypeOf(static_cast<Type*>(nullptr)); });                              \
    const ::zx::TypeInfo& ZxTypeOf(Type*)                                                                              \
    {                                                                                                                  \
        static constinit ::zx::detail::LazyType zxLazy;                                                                \
        return ::zx::detail::GetOrBuildType(zxLazy, [](::zx::TypeInfo& zxInfo) {                                      \
            ::zx::EnumBuilder<Type> zxBuilder(zxInfo, #Type);                                                          \
            using ZxThis = Type;

#define ZX_REFLECT_ENUM_END() ZX_REFLECT_STRUCT_END()
#define ZX_ENUM_VALUE(value, ...) zxBuilder.Value(#value, ZxThis::value __VA_OPT__(, ) __VA_ARGS__);
#define ZX_ENUM_FLAGS_TYPE() zxBuilder.Flags();
