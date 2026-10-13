#include "Script/ZeqouScript.h"
#include "Core/Testing/Test.h"

using namespace zx;
using namespace zx::script;

namespace {

struct Runner
{
    VM vm;
    Program* program = nullptr;
    String log;
    CompileError error;

    Runner() { RegisterStandardLibrary(vm); vm.onLog = [this](StringView s) { log += s; log += '\n'; }; }
    ~Runner() { DestroyProgram(program); }

    bool Compile(StringView src)
    {
        Result<Program*> p = script::Compile(src, "Test.zs", &error);
        if (!p)
            return false;
        program = *p;
        return true;
    }
};

/// Compiles `script T { fn run() { <body> } }` and returns run()'s result.
Value Eval(StringView body, String* errorOut = nullptr, String* logOut = nullptr)
{
    Runner r;
    String src = "script T {\n fn run() {\n";
    src += body;
    src += "\n }\n}\n";
    if (!r.Compile(src)) {
        if (errorOut)
            *errorOut = r.error.message;
        return Value();
    }
    Instance inst(r.vm, r.program);
    Value result;
    if (!inst.Call("run", {}, &result) && errorOut)
        *errorOut = r.vm.LastError();
    if (logOut)
        *logOut = r.log;
    return result;
}

f64 Num(StringView body) { return Eval(body).AsNumber(-12345.0); }

} // namespace

ZX_TEST(Script, Arithmetic)
{
    ZX_EXPECT_EQ(Num("return 1 + 2 * 3"), 7.0);
    ZX_EXPECT_EQ(Num("return (1 + 2) * 3"), 9.0);
    ZX_EXPECT_EQ(Num("return -2 * -3 - 1"), 5.0);
    ZX_EXPECT_EQ(Num("return 7 % 3"), 1.0);
    ZX_EXPECT_EQ(Num("return -1 % 3"), 2.0); // modulo is always positive for positive divisors
    ZX_EXPECT_EQ(Num("return 1_000 / 4"), 250.0);
    ZX_EXPECT_EQ(Num("return 2.5e1"), 25.0);
    ZX_EXPECT(Eval("return 1 < 2 and not (3 <= 2)").AsBool());
    ZX_EXPECT(Eval("return 1 == 1 && 2 != 3 || false").AsBool());
    ZX_EXPECT(Eval("return \"a\" < \"b\"").AsBool());
    ZX_EXPECT(Eval("return \"x\" + 1 + true").AsString() == "x1true");
    ZX_EXPECT(Eval("return nil or 5").AsNumber() == 5.0);   // or returns the first truthy operand
    ZX_EXPECT(Eval("return 0 and 5").AsNumber() == 0.0);
}

ZX_TEST(Script, ControlFlow)
{
    ZX_EXPECT_EQ(Num("var s = 0\n for i in 0..10 { s += i }\n return s"), 45.0);
    ZX_EXPECT_EQ(Num("var s = 0\n for i in 0..10 { if i % 2 == 0 { continue }\n if i > 7 { break }\n s += i }\n return s"), 16.0);
    ZX_EXPECT_EQ(Num("var n = 0\n while true { n += 1\n if n >= 5 { break } }\n return n"), 5.0);
    ZX_EXPECT_EQ(Num("let x = 5\n if x > 10 { return 1 } elif x > 3 { return 2 } else { return 3 }"), 2.0);
    ZX_EXPECT_EQ(Num("let x = 1\n if x > 10 { return 1 } else if x > 3 { return 2 } else { return 3 }"), 3.0);
    ZX_EXPECT_EQ(Num("var s = 0\n for v in [1, 2, 3] { s = s * 10 + v }\n return s"), 123.0);
    ZX_EXPECT_EQ(Num("var x = 1\n { var x = 2 }\n return x"), 1.0); // block scope (shadowing)
}

ZX_TEST(Script, FunctionsAndFields)
{
    Runner r;
    ZX_REQUIRE(r.Compile(R"zs(
        script Counter {
            @range(0, 10) @tooltip("Step size") prop step = 2.0
            prop label = "count"
            @color prop tint = vec3(1, 0.5, 0)
            var total = step * 10
            fn add(n) { total += n * step; return total }
            fn fib(n) { if n < 2 { return n } return fib(n - 1) + fib(n - 2) }
            fn twice(x) { return self.add(x) + 0 }
        }
    )zs"));
    ZX_EXPECT(r.program->name == "Counter");
    ZX_REQUIRE(r.program->properties.Size() == 3);
    ZX_EXPECT(r.program->properties[0].kind == PropertyKind::Number);
    ZX_EXPECT_EQ(r.program->properties[0].rangeMax, 10.0);
    ZX_EXPECT(r.program->properties[0].tooltip == "Step size");
    ZX_EXPECT(r.program->properties[1].kind == PropertyKind::String);
    ZX_EXPECT(r.program->properties[2].kind == PropertyKind::Color);

    Instance inst(r.vm, r.program);
    ZX_REQUIRE(inst.Ok());
    ZX_EXPECT_EQ(inst.Field("total")->AsNumber(), 20.0);
    Value out;
    ZX_EXPECT(inst.Call("add", {Value(3.0)}, &out));
    ZX_EXPECT_EQ(out.AsNumber(), 26.0);
    ZX_EXPECT(inst.Call("twice", {Value(1.0)}, &out));
    ZX_EXPECT_EQ(out.AsNumber(), 28.0);
    ZX_EXPECT(inst.Call("fib", {Value(15.0)}, &out));
    ZX_EXPECT_EQ(out.AsNumber(), 610.0);
    ZX_EXPECT(inst.Call("missing")); // missing functions are fine (start/update are optional)

    // A second instance has its own fields; a host override of a prop is visible to the script.
    Instance other(r.vm, r.program);
    *other.Field("step") = Value(1.0);
    ZX_EXPECT(other.Call("add", {Value(3.0)}, &out));
    ZX_EXPECT_EQ(out.AsNumber(), 23.0);
    ZX_EXPECT_EQ(inst.Field("total")->AsNumber(), 28.0);
}

ZX_TEST(Script, VectorsAndWriteBack)
{
    ZX_EXPECT_EQ(Num("var v = vec3(1, 2, 3)\n v.y = 5\n return v.x + v.y + v.z"), 9.0);
    ZX_EXPECT_EQ(Num("var v = vec3(1, 2, 3)\n v.y += 5\n v[2] = 10\n return v.y + v.z"), 17.0);
    ZX_EXPECT_EQ(Num("return (vec3(1, 0, 0) + vec3(0, 2, 0) * 2).length() * 0 + vec3(3, 4, 0).length()"), 5.0);
    ZX_EXPECT_EQ(Num("return vec3(1, 0, 0).dot(vec3(0, 1, 0)) + vec3(1, 0, 0).cross(vec3(0, 1, 0)).z"), 1.0);
    // Nested value types inside lists: list[0].x = 7 writes the modified vec3 back into the list.
    ZX_EXPECT_EQ(Num("var l = [vec3(1), vec3(2)]\n l[0].x = 7\n return l[0].x + l[1].x"), 9.0);
    // Rotations: yaw 90 degrees turns forward (+Z) into +X.
    const Value v = Eval("return quat.euler(0, 90, 0) * vec3(0, 0, 1)");
    ZX_EXPECT(IsNearlyEqual(v.AsVec3(), Vec3(1, 0, 0), 1e-5f));
    ZX_EXPECT(IsNearlyEqual(Eval("return quat.lookAt(vec3(1, 0, 0)).forward()").AsVec3(), Vec3(1, 0, 0), 1e-5f));
    ZX_EXPECT(Eval("return str(vec3(1, 2.5, -3))").AsString() == "vec3(1, 2.5, -3)");
}

ZX_TEST(Script, ListsAndStrings)
{
    ZX_EXPECT_EQ(Num("var l = []\n l.push(1)\n l.push(2)\n l.push(3)\n l.remove(0)\n return l.len() * 10 + l[-1]"), 23.0);
    ZX_EXPECT_EQ(Num("var a = [1]\n var b = a\n b.push(2)\n return len(a)"), 2.0); // lists are references
    ZX_EXPECT(Eval("return str([1, \"a\", [true]])").AsString() == "[1, \"a\", [true]]");
    ZX_EXPECT(Eval("return \"Hello\".upper() + \"!\"[0]").AsString() == "HELLO!");
    ZX_EXPECT(Eval("return [1, 2, 3].contains(2) and not [1].contains(5)").AsBool());
    String log;
    Eval("print(\"a\", 1, nil)\n log(1.5)", nullptr, &log);
    ZX_EXPECT(log == "a 1 nil\n1.5\n");
}

ZX_TEST(Script, NativeObjects)
{
    struct Thing
    {
        Vec3 position;
        i32 hits = 0;
    } thing;
    Runner r;
    NativeType& type = r.vm.AddType("Thing");
    type.get = [](VM&, NativeObject& o, StringView m, Value& out) {
        if (m != "position")
            return false;
        out = Value(static_cast<Thing*>(o.handle)->position);
        return true;
    };
    type.set = [](VM&, NativeObject& o, StringView m, const Value& v) {
        if (m != "position")
            return false;
        static_cast<Thing*>(o.handle)->position = v.AsVec3();
        return true;
    };
    type.call = [](VM&, NativeObject& o, StringView m, Span<const Value>, Value&) {
        if (m != "hit")
            return false;
        ++static_cast<Thing*>(o.handle)->hits;
        return true;
    };
    r.vm.SetGlobal("thing", MakeNative(type, &thing));
    ZX_REQUIRE(r.Compile("script S { fn update(dt) { thing.position.y += dt * 2\n thing.hit() } }"));
    Instance inst(r.vm, r.program);
    ZX_EXPECT(inst.Call("update", {Value(0.5)}));
    ZX_EXPECT(inst.Call("update", {Value(0.25), Value(99.0)})); // extra host arguments are ignored
    ZX_EXPECT_NEAR(thing.position.y, 1.5f, 1e-6f);
    ZX_EXPECT_EQ(thing.hits, 2);
}

ZX_TEST(Script, CompileErrors)
{
    auto error = [](StringView src, u32 line) {
        CompileError e;
        Result<Program*> p = Compile(src, "Bad.zs", &e);
        if (p) {
            DestroyProgram(*p);
            return String("compiled");
        }
        return e.line == line ? e.message : String::Format("line {} != {}: {}", e.line, line, e.message);
    };
    ZX_EXPECT(error("fn x() {}", 1).Contains("'script'"));
    ZX_EXPECT(error("script A {\n fn f() {\n  let x = \n }\n}", 4).Contains("Unexpected"));
    ZX_EXPECT(error("script A {\n prop p\n}", 3).Contains("default value"));
    ZX_EXPECT(error("script A {\n fn f() { break }\n}", 2).Contains("outside of a loop"));
    ZX_EXPECT(error("script A {\n fn f() { g(1) }\n fn g() {}\n}", 2).Contains("takes 0 argument"));
    ZX_EXPECT(error("script A {\n fn f() { 1 = 2 }\n}", 2).Contains("Can't assign"));
    ZX_EXPECT(error("script A {\n fn f() { var s = \"abc }\n}", 2).Contains("Unterminated string"));
    ZX_EXPECT(error("script A {\n @range(0, 1) prop p = speed * 2\n}", 2).Contains("literal"));
    ZX_EXPECT(error("script A {\n @wat prop p = 1\n}", 2).Contains("Unknown annotation"));
}

ZX_TEST(Script, RuntimeErrors)
{
    String err;
    Eval("var x = nil\n return x.y", &err);
    ZX_EXPECT(err.Contains("Test.zs:4") && err.Contains("has no member 'y'"));
    Eval("return [1, 2][5]", &err);
    ZX_EXPECT(err.Contains("out of range"));
    Eval("return undefinedThing + 1", &err);
    ZX_EXPECT(err.Contains("Unknown name 'undefinedThing'"));
    Eval("return vec3(1) + \"\" - 1", &err);
    ZX_EXPECT(err.Contains("Can't use '-'"));

    // Endless loops and recursion are stopped; the instance stays stopped afterwards.
    Runner r;
    r.vm.SetInstructionLimit(100'000);
    ZX_REQUIRE(r.Compile("script L { fn spin() { while true { } }\n fn deep(n) { return deep(n + 1) } }"));
    Instance inst(r.vm, r.program);
    ZX_EXPECT(!inst.Call("spin"));
    ZX_EXPECT(r.vm.LastError().Contains("took too long"));
    ZX_EXPECT(!inst.Call("deep", {Value(0.0)}));
    Instance inst2(r.vm, r.program);
    ZX_EXPECT(!inst2.Call("deep", {Value(0.0)}));
    ZX_EXPECT(r.vm.LastError().Contains("Stack overflow"));
}
