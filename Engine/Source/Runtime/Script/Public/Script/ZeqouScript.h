#pragma once

// ZeqouScript: the engine's own scripting language (.zs). Small, readable, dynamically typed; compiled to bytecode and
// run by a stack VM. Language reference: docs/ZeqouScript.md.
//
//   script Spinner {
//       @range(0, 720) prop speed = 90.0     // shown in the Details panel, saved per entity
//       var angle = 0.0                       // per-instance state
//       fn update(dt) {
//           angle += speed * dt
//           transform.rotation = quat.euler(0, angle, 0)
//       }
//   }
//
// Host usage:
//   script::VM vm;                                    // + vm.SetGlobal(...) for engine objects / functions
//   Result<Program*> p = script::Compile(source, "Spinner.zs");  // host owns it (DestroyProgram)
//   script::Instance inst(vm, *p);                    // runs the field initializers
//   inst.Call("update", {Value(dt)});
//
// Values: nil, bool, number (f64), string, vec3, quat, list, function, native object. vec3/quat are values (copied);
// strings, lists and native objects are reference counted.

#include "Core/Containers/Function.h"
#include "Core/Containers/HashMap.h"
#include "Core/Math/Math.h"
#include "Core/Memory/Pointers.h"
#include "Core/Result.h"

namespace zx::script {

class VM;
struct Program;
struct NativeType;

enum class ValueType : u8
{
    Nil,
    Bool,
    Number,
    Vec3,
    Quat,
    String,
    List,
    Function,   // script function (index into its program)
    NativeFunc, // host function
    Native,     // host object (entity, transform, input, ...)
};

const char* TypeName(ValueType t);

/// Heap objects (strings, lists, natives, host functions); reference counted, no cycle collection.
struct Object
{
    u32 refCount = 1;
    ValueType type;
    explicit Object(ValueType t) : type(t) {}
    virtual ~Object() = default;
};

class Value
{
public:
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : m_type(ValueType::Bool) { m_b = b; }
    Value(f64 n) : m_type(ValueType::Number) { m_n = n; }
    Value(f32 n) : Value(f64(n)) {}
    Value(i32 n) : Value(f64(n)) {}
    Value(Vec3 v);
    Value(Quat q);
    Value(StringView s); // new string object
    Value(const char* s) : Value(StringView(s)) {}
    static Value FromObject(Object* o); // takes one reference
    static Value Function(const Program* program, u32 index);
    Value(const Value& o);
    Value(Value&& o) noexcept;
    Value& operator=(const Value& o);
    Value& operator=(Value&& o) noexcept;
    ~Value() { Release(); }

    ValueType Type() const { return m_type; }
    bool IsNil() const { return m_type == ValueType::Nil; }
    bool IsNumber() const { return m_type == ValueType::Number; }
    bool AsBool() const; // truthiness: nil/false/0 are false
    f64 AsNumber(f64 fallback = 0.0) const { return m_type == ValueType::Number ? m_n : fallback; }
    f32 AsFloat(f32 fallback = 0.0f) const { return f32(AsNumber(fallback)); }
    Vec3 AsVec3() const;
    Quat AsQuat() const;
    StringView AsString() const; // empty unless a string
    Object* AsObject() const { return IsObject() ? m_obj : nullptr; }
    bool IsObject() const { return m_type >= ValueType::String && m_type != ValueType::Function; }
    const Program* FunctionProgram() const { return m_type == ValueType::Function ? m_fn.program : nullptr; }
    u32 FunctionIndex() const { return m_fn.index; }
    /// Display text ("1.5", "vec3(1, 2, 3)", "[1, 2]", "nil", ...).
    String ToString() const;
    friend bool operator==(const Value& a, const Value& b);

private:
    void Release();
    ValueType m_type = ValueType::Nil;
    union {
        bool m_b;
        f64 m_n;
        f32 m_v[4];
        Object* m_obj;
        struct
        {
            const Program* program;
            u32 index;
        } m_fn;
    };
};

struct StringObject final : Object
{
    String text;
    StringObject() : Object(ValueType::String) {}
};

struct ListObject final : Object
{
    Array<Value> items;
    ListObject() : Object(ValueType::List) {}
};

using NativeFn = Function<bool(VM& vm, Span<const Value> args, Value& result), 32>;

struct NativeFuncObject final : Object
{
    String name;
    NativeFn fn;
    NativeFuncObject() : Object(ValueType::NativeFunc) {}
};

/// A host object: `type` describes its members, `handle`/`id` identify the host data (e.g. an entity).
struct NativeObject final : Object
{
    const NativeType* nativeType = nullptr;
    void* handle = nullptr;
    u64 id = 0;
    NativeObject() : Object(ValueType::Native) {}
};

/// Member access on native objects. Return false for unknown members (the VM reports an error).
struct NativeType
{
    String name;
    Function<bool(VM&, NativeObject&, StringView member, Value& out), 32> get;
    Function<bool(VM&, NativeObject&, StringView member, const Value& value), 32> set;
    Function<bool(VM&, NativeObject&, StringView method, Span<const Value> args, Value& out), 32> call;
};

Value MakeNative(const NativeType& type, void* handle, u64 id = 0);
Value MakeNativeFunction(StringView name, NativeFn fn);

// --- Compiled programs ----------------------------------------------------------------------------------------------------

enum class PropertyKind : u8
{
    Number,
    Bool,
    String,
    Vec3,
    Color, // vec3 shown as a color picker (@color)
};

/// A `prop` field: editable per entity.
struct Property
{
    String name;
    PropertyKind kind = PropertyKind::Number;
    Value defaultValue;
    f64 rangeMin = 0.0, rangeMax = 0.0; // @range(min, max); equal = unbounded
    String tooltip;                     // @tooltip("...")
    u32 field = 0;
};

struct FunctionCode
{
    String name;
    u32 params = 0;
    u32 locals = 0; // including params
    Array<u8> code;
    Array<u32> lines; // source line per code byte (errors)
};

struct Program
{
    String name;   // script name
    String source; // file name for messages
    Array<String> fields; // instance fields (props and vars), in declaration order
    Array<Property> properties;
    Array<Value> constants;
    Array<FunctionCode> functions; // [0] = field initializers
    HashMap<String, u32> functionIndex;
    u32 refCount = 0;
    i32 FindFunction(StringView name) const;
};

struct CompileError
{
    String message;
    u32 line = 0, column = 0;
};

/// Compiles one script file. On failure the error says where.
Result<Program*> Compile(StringView source, StringView fileName, CompileError* error = nullptr);
void DestroyProgram(Program* program);

// --- VM ---------------------------------------------------------------------------------------------------------------------

class Instance;

class VM
{
public:
    VM();
    ~VM();
    ZX_NON_COPYABLE(VM)

    void SetGlobal(StringView name, Value value);
    /// Native types live as long as the VM (objects reference them).
    NativeType& AddType(StringView name);
    Random& Rng() { return m_random; }
    const Value* FindGlobal(StringView name) const;

    /// Runs `function` of `program` with `self` fields. Returns false on a runtime error (see LastError()).
    bool Call(const Program& program, u32 function, Array<Value>& fields, Span<const Value> args, Value* result = nullptr);
    /// Calls a function value (script or native) - used by natives that take callbacks.
    bool CallValue(const Value& fn, Span<const Value> args, Value* result = nullptr);

    StringView LastError() const { return m_error.View(); }
    /// Reports a runtime error from a native function (the current call fails).
    bool Fail(StringView message);
    /// Instruction budget per call (endless loops are stopped). 0 = unlimited.
    void SetInstructionLimit(u64 limit) { m_instructionLimit = limit; }
    /// Receives log(...) / print(...) output.
    Function<void(StringView), 32> onLog;

private:
    friend class Instance;
    struct Frame;
    bool Run(const Program& program, u32 function, Array<Value>& fields, Span<const Value> args, Value* result);
    bool GetMember(const Value& obj, StringView name, Value& out);
    bool SetMember(Value& obj, StringView name, const Value& v);
    bool CallMethod(Value& obj, StringView name, Span<const Value> args, Value& out);

    HashMap<String, Value> m_globals;
    String m_error;
    u64 m_instructionLimit = 50'000'000;
    u64 m_executed = 0;
    u32 m_depth = 0;
    bool m_errorLocated = false;
    Array<Value> m_stack; // fixed capacity: spans into it stay valid during native calls
    Array<Array<Value>*> m_fieldStack; // `self` fields of the running frames
    Array<UniquePtr<NativeType>> m_types;
    Random m_random;
};

/// A script attached to something: its field values + the program.
class Instance
{
public:
    /// Runs the field initializers. Non-nil `presetFields` (indexed like Program::fields) replace prop defaults, so
    /// later initializers already see the per-entity values.
    Instance(VM& vm, Program* program, Span<const Value> presetFields = {});
    ~Instance();
    ZX_NON_COPYABLE(Instance)

    bool Ok() const { return m_ok; }
    bool HasFunction(StringView name) const { return m_program->FindFunction(name) >= 0; }
    /// Calls a script function if it exists (missing = success). Errors are logged once by the host (LastError()).
    bool Call(StringView function, Span<const Value> args = {}, Value* result = nullptr);
    Value* Field(StringView name);
    Array<Value>& Fields() { return m_fields; }
    const Program& GetProgram() const { return *m_program; }

private:
    VM& m_vm;
    Program* m_program;
    Array<Value> m_fields;
    bool m_ok = true;
};

/// Built-in globals: math (sin, cos, ..., lerp, clamp, random), vec3(), quat.*, log/print, str(), len().
void RegisterStandardLibrary(VM& vm);

} // namespace zx::script
