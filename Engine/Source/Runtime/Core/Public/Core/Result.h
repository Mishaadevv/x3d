#pragma once

// Result<T>: value or Error, for operations that can fail on external input (files, assets, shaders).
//
//   Result<Texture> LoadTexture(StringView path)
//   {
//       Array<u8> bytes;
//       if (!fs::ReadFile(path, bytes))
//           return Error::Make(ErrorCode::NotFound, "Texture '{}' not found", path);
//       ZX_TRY_ASSIGN(image, DecodePng(bytes));   // propagates the error
//       return Texture(move(image));
//   }

#include "Core/Containers/Optional.h"
#include "Core/String/String.h"

namespace zx {

enum class ErrorCode : u16
{
    Unknown,
    NotFound,
    AccessDenied,
    InvalidData,
    Unsupported,
    OutOfMemory,
    Timeout,
    Cancelled,
    AlreadyExists,
    InvalidArgument,
    DeviceLost,
    Internal,
};

const char* ErrorCodeName(ErrorCode code);

struct Error
{
    ErrorCode code = ErrorCode::Unknown;
    String message;

    template <typename... Args>
    static Error Make(ErrorCode code, StringView fmt, const Args&... args)
    {
        return Error{code, String::Format(fmt, args...)};
    }
};

template <>
struct Formatter<Error>
{
    static void Format(FormatSink& s, const Error& e, const FormatSpec&)
    {
        FormatTo(s, "{}: {}", ErrorCodeName(e.code), e.message);
    }
};

template <typename T>
class [[nodiscard]] Result
{
public:
    Result(const T& value) : m_value(value) {}
    Result(T&& value) : m_value(move(value)) {}
    Result(Error error) : m_error(move(error)), m_failed(true) {}

    bool IsOk() const { return !m_failed; }
    explicit operator bool() const { return !m_failed; }

    T& Value()
    {
        ZX_ASSERT(!m_failed, "Result has an error: {}", m_error.message);
        return *m_value;
    }
    const T& Value() const
    {
        ZX_ASSERT(!m_failed, "Result has an error: {}", m_error.message);
        return *m_value;
    }
    T& operator*() { return Value(); }
    const T& operator*() const { return Value(); }
    T* operator->() { return &Value(); }
    const T* operator->() const { return &Value(); }
    T ValueOr(T fallback) const { return m_failed ? move(fallback) : *m_value; }
    /// Moves the value out.
    T Take() { return move(Value()); }

    const Error& GetError() const
    {
        ZX_ASSERT(m_failed, "Result has no error");
        return m_error;
    }
    Error TakeError() { return move(m_error); }

private:
    Optional<T> m_value;
    Error m_error;
    bool m_failed = false;
};

/// Result without a value.
template <>
class [[nodiscard]] Result<void>
{
public:
    Result() = default;
    Result(Error error) : m_error(move(error)), m_failed(true) {}
    static Result Ok() { return Result(); }

    bool IsOk() const { return !m_failed; }
    explicit operator bool() const { return !m_failed; }
    const Error& GetError() const
    {
        ZX_ASSERT(m_failed, "Result has no error");
        return m_error;
    }
    Error TakeError() { return move(m_error); }

private:
    Error m_error;
    bool m_failed = false;
};

} // namespace zx

/// Propagates the error of a Result<void>-like expression.
#define ZX_TRY(expr)                                                                                                   \
    do {                                                                                                               \
        auto zxTryResult_ = (expr);                                                                                    \
        if (!zxTryResult_)                                                                                             \
            return zxTryResult_.TakeError();                                                                           \
    } while (0)

/// Declares `var` initialized with the value of a Result expression, or returns its error.
#define ZX_TRY_ASSIGN(var, expr)                                                                                       \
    auto ZX_CONCAT(zxTryAssign_, __LINE__) = (expr);                                                                   \
    if (!ZX_CONCAT(zxTryAssign_, __LINE__))                                                                            \
        return ZX_CONCAT(zxTryAssign_, __LINE__).TakeError();                                                          \
    auto var = ZX_CONCAT(zxTryAssign_, __LINE__).Take()
