#pragma once

// Minimal unit-test framework.
//
//   ZX_TEST(Containers, ArrayPushBack)
//   {
//       Array<int> a;
//       a.PushBack(1);
//       ZX_EXPECT_EQ(a.Size(), 1u);
//       ZX_REQUIRE(a.Data() != nullptr); // aborts this test on failure
//   }
//
// Tests are registered statically; the runner lives in Tests/TestsMain.cpp.

#include "Core/String/Format.h"

namespace zx::test {

using TestFn = void (*)();

struct TestCase
{
    const char* suite;
    const char* name;
    TestFn fn;
    const char* file;
    i32 line;
    TestCase* next;
};

/// Registers a test (called from static registrars). Returns a dummy value.
int RegisterTest(TestCase* test);

/// Head of the intrusive registry list.
TestCase* GetRegisteredTests();

/// Records a failed expectation in the currently running test.
void ReportFailure(const char* file, i32 line, StringView message);

/// Thrown-free abort of the current test (longjmp-free: sets flag; ZX_REQUIRE returns from the test function).
bool CurrentTestFailed();

struct RunOptions
{
    StringView filter;   // "Suite.*", "*Name*", "Suite.Name" (simple '*' wildcard); empty = all
    bool verbose = false;
    bool listOnly = false;
};

/// Runs registered tests. Returns number of failed tests.
i32 RunTests(const RunOptions& options);

namespace detail {

/// Non-constexpr pass-through so test conditions that fold to constants don't trigger C4127.
template <typename T>
ZX_NOINLINE bool Check(const T& value)
{
    return static_cast<bool>(value);
}

template <typename A, typename B>
void ReportCompare(const char* file, i32 line, const char* exprA, const char* op, const char* exprB, const A& a,
                   const B& b)
{
    char buffer[1024];
    usize n;
    if constexpr (requires { zx::detail::MakeFormatArg(a); zx::detail::MakeFormatArg(b); })
        n = FormatToBuffer(buffer, sizeof(buffer), "{} {} {}  (values: {} vs {})", exprA, op, exprB, a, b);
    else
        n = FormatToBuffer(buffer, sizeof(buffer), "{} {} {}", exprA, op, exprB);
    ReportFailure(file, line, StringView(buffer, n));
}

} // namespace detail
} // namespace zx::test

#define ZX_TEST(Suite, Name)                                                                                           \
    static void ZX_CONCAT(ZxTest_##Suite##_, Name)();                                                                  \
    static ::zx::test::TestCase ZX_CONCAT(ZxTestCase_##Suite##_, Name) = {                                             \
        #Suite, #Name, &ZX_CONCAT(ZxTest_##Suite##_, Name), __FILE__, __LINE__, nullptr};                              \
    static const int ZX_CONCAT(ZxTestReg_##Suite##_, Name) =                                                           \
        ::zx::test::RegisterTest(&ZX_CONCAT(ZxTestCase_##Suite##_, Name));                                             \
    static void ZX_CONCAT(ZxTest_##Suite##_, Name)()

#define ZX_EXPECT(cond)                                                                                                \
    do {                                                                                                               \
        if (!::zx::test::detail::Check(cond))                                                                          \
            ::zx::test::ReportFailure(__FILE__, __LINE__, "Expected: " #cond);                                         \
    } while (0)

#define ZX_REQUIRE(cond)                                                                                               \
    do {                                                                                                               \
        if (!::zx::test::detail::Check(cond)) {                                                                        \
            ::zx::test::ReportFailure(__FILE__, __LINE__, "Required: " #cond);                                         \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

#define ZX_EXPECT_CMP_IMPL(a, op, b)                                                                                   \
    do {                                                                                                               \
        const auto& zxA_ = (a);                                                                                        \
        const auto& zxB_ = (b);                                                                                        \
        if (!::zx::test::detail::Check(zxA_ op zxB_))                                                                  \
            ::zx::test::detail::ReportCompare(__FILE__, __LINE__, #a, #op, #b, zxA_, zxB_);                            \
    } while (0)

#define ZX_EXPECT_EQ(a, b) ZX_EXPECT_CMP_IMPL(a, ==, b)
#define ZX_EXPECT_NE(a, b) ZX_EXPECT_CMP_IMPL(a, !=, b)
#define ZX_EXPECT_LT(a, b) ZX_EXPECT_CMP_IMPL(a, <, b)
#define ZX_EXPECT_LE(a, b) ZX_EXPECT_CMP_IMPL(a, <=, b)
#define ZX_EXPECT_GT(a, b) ZX_EXPECT_CMP_IMPL(a, >, b)
#define ZX_EXPECT_GE(a, b) ZX_EXPECT_CMP_IMPL(a, >=, b)

#define ZX_EXPECT_NEAR(a, b, eps)                                                                                      \
    do {                                                                                                               \
        const double zxA_ = double(a);                                                                                 \
        const double zxB_ = double(b);                                                                                 \
        const double zxD_ = zxA_ > zxB_ ? zxA_ - zxB_ : zxB_ - zxA_;                                                   \
        if (!::zx::test::detail::Check(zxD_ <= double(eps)))                                                           \
            ::zx::test::detail::ReportCompare(__FILE__, __LINE__, #a, "~=", #b, zxA_, zxB_);                           \
    } while (0)

/// Expects that `stmt` triggers a ZX_ASSERT failure (assert handler is temporarily set to Continue).
/// When asserts are compiled out the statement is skipped entirely (it would be undefined behavior).
#if ZX_ENABLE_ASSERTS
#define ZX_EXPECT_ASSERT(stmt)                                                                                         \
    do {                                                                                                               \
        ::zx::test::BeginExpectAssert();                                                                               \
        stmt;                                                                                                          \
        if (!::zx::test::EndExpectAssert())                                                                            \
            ::zx::test::ReportFailure(__FILE__, __LINE__, "Expected assertion: " #stmt);                               \
    } while (0)
#else
#define ZX_EXPECT_ASSERT(stmt)                                                                                         \
    do {                                                                                                               \
        (void)[&]() { stmt; };                                                                                         \
    } while (0)
#endif

namespace zx::test {
void BeginExpectAssert();
bool EndExpectAssert();
} // namespace zx::test
