#pragma once

// Platform / compiler / build-configuration macros. Included (directly or indirectly) by every engine file.

#if defined(_WIN32) && defined(_WIN64)
    #define ZX_PLATFORM_WINDOWS 1
#else
    #error "ZeqouX3D: unsupported platform (only Win64 for now)"
#endif

#if defined(_MSC_VER) && !defined(__clang__)
    #define ZX_COMPILER_MSVC 1
#endif

#ifndef ZX_BUILD_DEBUG
    #define ZX_BUILD_DEBUG 0
#endif
#ifndef ZX_BUILD_DEVELOPMENT
    #define ZX_BUILD_DEVELOPMENT 0
#endif
#ifndef ZX_BUILD_SHIPPING
    #define ZX_BUILD_SHIPPING 0
#endif

#if (ZX_BUILD_DEBUG + ZX_BUILD_DEVELOPMENT + ZX_BUILD_SHIPPING) != 1
    #error "Exactly one of ZX_BUILD_DEBUG / ZX_BUILD_DEVELOPMENT / ZX_BUILD_SHIPPING must be 1"
#endif

// Feature switches derived from the configuration.
#define ZX_ENABLE_ASSERTS (!ZX_BUILD_SHIPPING)
#define ZX_ENABLE_SLOW_CHECKS (ZX_BUILD_DEBUG)
#define ZX_ENABLE_PROFILER (!ZX_BUILD_SHIPPING)
#define ZX_ENABLE_MEMORY_TRACKING (!ZX_BUILD_SHIPPING)
#define ZX_ENABLE_LEAK_CALLSTACKS (ZX_BUILD_DEBUG)
#define ZX_ENABLE_LOG_VERBOSE (!ZX_BUILD_SHIPPING)

#define ZX_FORCEINLINE __forceinline
#define ZX_NOINLINE __declspec(noinline)
#define ZX_NORETURN [[noreturn]]
#define ZX_RESTRICT __restrict
#define ZX_DEBUGBREAK() __debugbreak()
#define ZX_ASSUME(x) __assume(x)
#define ZX_ALIGNAS(n) alignas(n)

#define ZX_STRINGIFY_IMPL(x) #x
#define ZX_STRINGIFY(x) ZX_STRINGIFY_IMPL(x)
#define ZX_CONCAT_IMPL(a, b) a##b
#define ZX_CONCAT(a, b) ZX_CONCAT_IMPL(a, b)
#define ZX_UNIQUE_NAME(base) ZX_CONCAT(base, __COUNTER__)
#define ZX_UNUSED(x) ((void)(x))
#define ZX_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#define ZX_NON_COPYABLE(Type)                                                                                          \
    Type(const Type&) = delete;                                                                                        \
    Type& operator=(const Type&) = delete;

#define ZX_NON_MOVABLE(Type)                                                                                           \
    Type(Type&&) = delete;                                                                                             \
    Type& operator=(Type&&) = delete;

// Bitwise operators for `enum class` flag types (requires <type_traits>, included by Core/Types.h).
#define ZX_ENUM_FLAGS(E)                                                                                               \
    constexpr E operator|(E a, E b) { return E(std::underlying_type_t<E>(a) | std::underlying_type_t<E>(b)); }         \
    constexpr E operator&(E a, E b) { return E(std::underlying_type_t<E>(a) & std::underlying_type_t<E>(b)); }         \
    constexpr E operator^(E a, E b) { return E(std::underlying_type_t<E>(a) ^ std::underlying_type_t<E>(b)); }         \
    constexpr E operator~(E a) { return E(~std::underlying_type_t<E>(a)); }                                            \
    constexpr E& operator|=(E& a, E b) { return a = a | b; }                                                           \
    constexpr E& operator&=(E& a, E b) { return a = a & b; }                                                           \
    constexpr E& operator^=(E& a, E b) { return a = a ^ b; }                                                           \
    constexpr bool HasAnyFlags(E a, E b) { return (std::underlying_type_t<E>(a) & std::underlying_type_t<E>(b)) != 0; } \
    constexpr bool HasAllFlags(E a, E b) { return (std::underlying_type_t<E>(a) & std::underlying_type_t<E>(b)) == std::underlying_type_t<E>(b); }
