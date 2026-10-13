#pragma once

// Type-safe "{}"-style formatting without heap allocation.
//
//   FormatToBuffer(buf, sizeof(buf), "x = {}, y = {:.2f}, hex = {:#010x}", 1, 2.5f, 255u);
//
// Spec grammar (subset of std::format):  {[index][:[[fill]align][sign][#][0][width][.precision][type]]}
//   align: < > ^      sign: + - space      type: d x X b o f e g p s c
// Custom types: specialize zx::Formatter<T> with
//   static void Format(FormatSink& sink, const T& value, const FormatSpec& spec);

#include "Core/String/StringView.h"

namespace zx {

/// Destination for formatted characters.
class FormatSink
{
public:
    virtual void Append(const char* s, usize n) = 0;
    void Append(StringView s) { Append(s.Data(), s.Size()); }
    void AppendChar(char c, usize count = 1);

protected:
    ~FormatSink() = default;
};

/// Writes into a fixed char buffer, always null-terminates, silently truncates.
class FixedBufferSink final : public FormatSink
{
public:
    FixedBufferSink(char* buffer, usize capacity) : m_buffer(buffer), m_capacity(capacity)
    {
        if (m_capacity > 0)
            m_buffer[0] = '\0';
    }
    using FormatSink::Append;
    void Append(const char* s, usize n) override;
    usize Length() const { return m_length; }
    bool Truncated() const { return m_truncated; }
    StringView View() const { return StringView(m_buffer, m_length); }

private:
    char* m_buffer;
    usize m_capacity;
    usize m_length = 0;
    bool m_truncated = false;
};

struct FormatSpec
{
    char fill = ' ';
    char align = 0;   // '<', '>', '^' or 0 (type default)
    char sign = '-';  // '+', '-', ' '
    char type = 0;    // 0 = default
    bool alternate = false;
    bool zeroPad = false;
    i32 width = 0;
    i32 precision = -1;
};

template <typename T, typename = void>
struct Formatter; // specialize for custom types

struct FormatArg
{
    enum class Kind : u8
    {
        None,
        Bool,
        Char,
        Int,
        UInt,
        Float,
        Double,
        String,
        Pointer,
        Custom
    };

    using CustomFn = void (*)(FormatSink&, const void*, const FormatSpec&);

    Kind kind = Kind::None;
    union {
        bool b;
        char c;
        i64 i;
        u64 u;
        f32 f;
        f64 d;
        struct
        {
            const char* data;
            usize size;
        } str;
        const void* ptr;
        struct
        {
            const void* object;
            CustomFn fn;
        } custom;
    };

    constexpr FormatArg() : i(0) {}
};

namespace detail {

template <typename T>
concept HasFormatter = requires(FormatSink& sink, const T& v, const FormatSpec& spec) {
    Formatter<T>::Format(sink, v, spec);
};

template <typename T>
concept HasStringViewConversion = requires(const T& v) {
    { v.View() } -> std::convertible_to<StringView>;
};

template <typename T>
FormatArg MakeFormatArg(const T& v)
{
    FormatArg a;
    using D = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<D, bool>) {
        a.kind = FormatArg::Kind::Bool;
        a.b = v;
    } else if constexpr (std::is_same_v<D, char>) {
        a.kind = FormatArg::Kind::Char;
        a.c = v;
    } else if constexpr (std::is_enum_v<D> && !HasFormatter<D>) {
        using U = std::underlying_type_t<D>;
        if constexpr (std::is_signed_v<U>) {
            a.kind = FormatArg::Kind::Int;
            a.i = i64(v);
        } else {
            a.kind = FormatArg::Kind::UInt;
            a.u = u64(v);
        }
    } else if constexpr (std::is_integral_v<D> && std::is_signed_v<D>) {
        a.kind = FormatArg::Kind::Int;
        a.i = i64(v);
    } else if constexpr (std::is_integral_v<D>) {
        a.kind = FormatArg::Kind::UInt;
        a.u = u64(v);
    } else if constexpr (std::is_same_v<D, f32>) {
        a.kind = FormatArg::Kind::Float;
        a.f = v;
    } else if constexpr (std::is_floating_point_v<D>) {
        a.kind = FormatArg::Kind::Double;
        a.d = f64(v);
    } else if constexpr (std::is_same_v<D, StringView>) {
        a.kind = FormatArg::Kind::String;
        a.str.data = v.Data();
        a.str.size = v.Size();
    } else if constexpr (std::is_same_v<D, const char*> || std::is_same_v<D, char*>) {
        const StringView sv(v);
        a.kind = FormatArg::Kind::String;
        a.str.data = sv.Data();
        a.str.size = sv.Size();
    } else if constexpr (std::is_array_v<D> && std::is_same_v<std::remove_extent_t<D>, char>) {
        const StringView sv(v);
        a.kind = FormatArg::Kind::String;
        a.str.data = sv.Data();
        a.str.size = sv.Size();
    } else if constexpr (HasFormatter<D>) {
        a.kind = FormatArg::Kind::Custom;
        a.custom.object = &v;
        a.custom.fn = [](FormatSink& sink, const void* obj, const FormatSpec& spec) {
            Formatter<D>::Format(sink, *static_cast<const D*>(obj), spec);
        };
    } else if constexpr (HasStringViewConversion<D>) {
        const StringView sv = v.View();
        a.kind = FormatArg::Kind::String;
        a.str.data = sv.Data();
        a.str.size = sv.Size();
    } else if constexpr (std::is_pointer_v<D> || std::is_null_pointer_v<D>) {
        a.kind = FormatArg::Kind::Pointer;
        a.ptr = static_cast<const void*>(v);
    } else {
        static_assert(sizeof(D) == 0, "Type is not formattable: specialize zx::Formatter<T>");
    }
    return a;
}

} // namespace detail

/// Core formatting routine (type-erased).
void VFormatTo(FormatSink& sink, StringView fmt, const FormatArg* args, usize argCount);

template <typename... Args>
void FormatTo(FormatSink& sink, StringView fmt, const Args&... args)
{
    if constexpr (sizeof...(Args) == 0) {
        VFormatTo(sink, fmt, nullptr, 0);
    } else {
        const FormatArg list[] = {detail::MakeFormatArg(args)...};
        VFormatTo(sink, fmt, list, sizeof...(Args));
    }
}

/// Formats into buffer (null-terminated, truncates). Returns number of chars written (excluding '\0').
template <typename... Args>
usize FormatToBuffer(char* buffer, usize capacity, StringView fmt, const Args&... args)
{
    FixedBufferSink sink(buffer, capacity);
    FormatTo(sink, fmt, args...);
    return sink.Length();
}

/// Formatting helpers usable by custom Formatter<T> specializations.
void FormatInteger(FormatSink& sink, u64 magnitude, bool negative, const FormatSpec& spec);
void FormatFloat(FormatSink& sink, f64 value, const FormatSpec& spec, bool isSinglePrecision);
void FormatString(FormatSink& sink, StringView s, const FormatSpec& spec);

} // namespace zx
