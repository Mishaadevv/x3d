// ZeqouScript values, stack VM and standard library.

#include "Bytecode.h"
#include "Script/ZeqouScript.h"
#include "Core/String/Parse.h"

namespace zx::script {

namespace {
constexpr u32 MaxCallDepth = 64;
constexpr usize StackCapacity = 16 * 1024;
} // namespace

const char* TypeName(ValueType t)
{
    switch (t) {
    case ValueType::Nil: return "nil";
    case ValueType::Bool: return "bool";
    case ValueType::Number: return "number";
    case ValueType::Vec3: return "vec3";
    case ValueType::Quat: return "quat";
    case ValueType::String: return "string";
    case ValueType::List: return "list";
    case ValueType::Function:
    case ValueType::NativeFunc: return "function";
    case ValueType::Native: return "object";
    }
    return "?";
}

// --- Value -------------------------------------------------------------------------------------------------------------------

Value::Value(Vec3 v) : m_type(ValueType::Vec3)
{
    m_v[0] = v.x;
    m_v[1] = v.y;
    m_v[2] = v.z;
    m_v[3] = 0.0f;
}

Value::Value(Quat q) : m_type(ValueType::Quat)
{
    m_v[0] = q.x;
    m_v[1] = q.y;
    m_v[2] = q.z;
    m_v[3] = q.w;
}

Value::Value(StringView s) : m_type(ValueType::String)
{
    StringObject* o = New<StringObject>();
    o->text = s;
    m_obj = o;
}

Value Value::FromObject(Object* o)
{
    Value v;
    if (o) {
        v.m_type = o->type;
        v.m_obj = o;
    }
    return v;
}

Value Value::Function(const Program* program, u32 index)
{
    Value v;
    v.m_type = ValueType::Function;
    v.m_fn.program = program;
    v.m_fn.index = index;
    return v;
}

Value::Value(const Value& o) : m_type(o.m_type)
{
    MemCopy(m_v, o.m_v, sizeof(m_v));
    if (IsObject())
        ++m_obj->refCount;
}

Value::Value(Value&& o) noexcept : m_type(o.m_type)
{
    MemCopy(m_v, o.m_v, sizeof(m_v));
    o.m_type = ValueType::Nil;
}

Value& Value::operator=(const Value& o)
{
    if (this != &o) {
        if (o.IsObject())
            ++o.m_obj->refCount;
        Release();
        m_type = o.m_type;
        MemCopy(m_v, o.m_v, sizeof(m_v));
    }
    return *this;
}

Value& Value::operator=(Value&& o) noexcept
{
    if (this != &o) {
        Release();
        m_type = o.m_type;
        MemCopy(m_v, o.m_v, sizeof(m_v));
        o.m_type = ValueType::Nil;
    }
    return *this;
}

void Value::Release()
{
    if (IsObject() && --m_obj->refCount == 0)
        Delete(m_obj);
    m_type = ValueType::Nil;
}

bool Value::AsBool() const
{
    switch (m_type) {
    case ValueType::Nil: return false;
    case ValueType::Bool: return m_b;
    case ValueType::Number: return m_n != 0.0;
    default: return true;
    }
}

Vec3 Value::AsVec3() const
{
    if (m_type == ValueType::Vec3)
        return Vec3(m_v[0], m_v[1], m_v[2]);
    if (m_type == ValueType::Number)
        return Vec3(f32(m_n));
    return Vec3(0.0f);
}

Quat Value::AsQuat() const { return m_type == ValueType::Quat ? Quat(m_v[0], m_v[1], m_v[2], m_v[3]) : Quat::Identity(); }

StringView Value::AsString() const
{
    return m_type == ValueType::String ? static_cast<const StringObject*>(m_obj)->text.View() : StringView();
}

namespace {

void AppendNumber(String& out, f64 n)
{
    if (n == std::floor(n) && Abs(n) < 1e15)
        out.Appendf("{}", i64(n));
    else
        out.Appendf("{}", n);
}

void AppendValue(String& out, const Value& v, u32 depth)
{
    switch (v.Type()) {
    case ValueType::Nil: out += "nil"; break;
    case ValueType::Bool: out += v.AsBool() ? "true" : "false"; break;
    case ValueType::Number: AppendNumber(out, v.AsNumber()); break;
    case ValueType::Vec3: {
        const Vec3 a = v.AsVec3();
        out += "vec3(";
        AppendNumber(out, a.x);
        out += ", ";
        AppendNumber(out, a.y);
        out += ", ";
        AppendNumber(out, a.z);
        out += ")";
        break;
    }
    case ValueType::Quat: {
        const Vec3 e = v.AsQuat().ToEulerDegrees();
        out += "quat.euler(";
        AppendNumber(out, f64(Round(e.x * 100.0f)) / 100.0);
        out += ", ";
        AppendNumber(out, f64(Round(e.y * 100.0f)) / 100.0);
        out += ", ";
        AppendNumber(out, f64(Round(e.z * 100.0f)) / 100.0);
        out += ")";
        break;
    }
    case ValueType::String:
        if (depth > 0) {
            out += '"';
            out += v.AsString();
            out += '"';
        } else {
            out += v.AsString();
        }
        break;
    case ValueType::List: {
        if (depth > 8) {
            out += "[...]";
            break;
        }
        out += "[";
        const auto* list = static_cast<const ListObject*>(v.AsObject());
        for (usize i = 0; i < list->items.Size(); ++i) {
            if (i)
                out += ", ";
            AppendValue(out, list->items[i], depth + 1);
        }
        out += "]";
        break;
    }
    case ValueType::Function:
        out += "fn ";
        out += v.FunctionProgram()->functions[v.FunctionIndex()].name;
        break;
    case ValueType::NativeFunc:
        out += "fn ";
        out += static_cast<const NativeFuncObject*>(v.AsObject())->name;
        break;
    case ValueType::Native: out += static_cast<const NativeObject*>(v.AsObject())->nativeType->name; break;
    }
}

} // namespace

String Value::ToString() const
{
    String s;
    AppendValue(s, *this, 0);
    return s;
}

bool operator==(const Value& a, const Value& b)
{
    if (a.m_type != b.m_type)
        return false;
    switch (a.m_type) {
    case ValueType::Nil: return true;
    case ValueType::Bool: return a.m_b == b.m_b;
    case ValueType::Number: return a.m_n == b.m_n;
    case ValueType::Vec3: return a.m_v[0] == b.m_v[0] && a.m_v[1] == b.m_v[1] && a.m_v[2] == b.m_v[2];
    case ValueType::Quat: return a.AsQuat() == b.AsQuat();
    case ValueType::String: return a.AsString() == b.AsString();
    case ValueType::Function: return a.m_fn.program == b.m_fn.program && a.m_fn.index == b.m_fn.index;
    case ValueType::Native: {
        const auto* na = static_cast<const NativeObject*>(a.m_obj);
        const auto* nb = static_cast<const NativeObject*>(b.m_obj);
        return na == nb || (na->nativeType == nb->nativeType && na->handle == nb->handle && na->id == nb->id);
    }
    default: return a.m_obj == b.m_obj;
    }
}

Value MakeNative(const NativeType& type, void* handle, u64 id)
{
    NativeObject* o = New<NativeObject>();
    o->nativeType = &type;
    o->handle = handle;
    o->id = id;
    return Value::FromObject(o);
}

Value MakeNativeFunction(StringView name, NativeFn fn)
{
    NativeFuncObject* o = New<NativeFuncObject>();
    o->name = name;
    o->fn = move(fn);
    return Value::FromObject(o);
}

namespace {
Value MakeList(Span<const Value> items = {})
{
    ListObject* l = New<ListObject>();
    l->items.Append(items);
    return Value::FromObject(l);
}
ListObject* AsList(const Value& v) { return v.Type() == ValueType::List ? static_cast<ListObject*>(v.AsObject()) : nullptr; }
} // namespace

// --- VM ----------------------------------------------------------------------------------------------------------------------

VM::VM() { m_stack.Reserve(StackCapacity); }

VM::~VM()
{
    m_stack.Clear();
    m_globals.Clear(); // values may reference m_types
}

void VM::SetGlobal(StringView name, Value value) { m_globals.InsertOrAssign(String(name), move(value)); }

const Value* VM::FindGlobal(StringView name) const { return m_globals.Find(name); }

NativeType& VM::AddType(StringView name)
{
    m_types.PushBack(MakeUnique<NativeType>());
    m_types.Back()->name = name;
    return *m_types.Back();
}

bool VM::Fail(StringView message)
{
    if (m_error.Empty())
        m_error = message;
    return false;
}

bool VM::Call(const Program& program, u32 function, Array<Value>& fields, Span<const Value> args, Value* result)
{
    if (m_depth == 0) {
        m_error.Clear();
        m_errorLocated = false;
        m_executed = 0;
    }
    if (function >= program.functions.Size())
        return Fail("Invalid function");
    return Run(program, function, fields, args, result);
}

bool VM::CallValue(const Value& fn, Span<const Value> args, Value* result)
{
    if (m_depth == 0) {
        m_error.Clear();
        m_errorLocated = false;
        m_executed = 0;
    }
    if (fn.Type() == ValueType::NativeFunc) {
        Value r;
        if (!static_cast<NativeFuncObject*>(fn.AsObject())->fn(*this, args, r))
            return Fail("Native function failed");
        if (result)
            *result = move(r);
        return true;
    }
    if (fn.Type() == ValueType::Function) {
        if (m_fieldStack.Empty())
            return Fail("A script function can only be called from a running script");
        const FunctionCode& code = fn.FunctionProgram()->functions[fn.FunctionIndex()];
        if (code.params != args.Size())
            return Fail(String::Format("'{}' takes {} argument(s), got {}", code.name, code.params, args.Size()));
        return Run(*fn.FunctionProgram(), fn.FunctionIndex(), *m_fieldStack.Back(), args, result);
    }
    return Fail(String::Format("Can't call a {}", TypeName(fn.Type())));
}

namespace {
bool ResolveIndex(VM& vm, const Value& index, usize size, usize& out)
{
    if (!index.IsNumber())
        return vm.Fail(String::Format("Index must be a number, got {}", TypeName(index.Type())));
    f64 i = index.AsNumber();
    if (i != std::floor(i))
        return vm.Fail(String::Format("Index must be a whole number, got {}", i));
    if (i < 0)
        i += f64(size); // list[-1] = last item
    if (i < 0 || i >= f64(size))
        return vm.Fail(String::Format("Index {} is out of range (size {})", index.AsNumber(), size));
    out = usize(i);
    return true;
}

i32 Vec3Component(StringView n) { return n == "x" ? 0 : n == "y" ? 1 : n == "z" ? 2 : -1; }
} // namespace

bool VM::GetMember(const Value& obj, StringView name, Value& out)
{
    switch (obj.Type()) {
    case ValueType::Vec3:
        if (const i32 c = Vec3Component(name); c >= 0) {
            out = Value(obj.AsVec3()[usize(c)]);
            return true;
        }
        break;
    case ValueType::Quat: {
        const Quat q = obj.AsQuat();
        if (name == "x" || name == "y" || name == "z" || name == "w") {
            out = Value(name == "x" ? q.x : name == "y" ? q.y : name == "z" ? q.z : q.w);
            return true;
        }
        break;
    }
    case ValueType::Native: {
        auto* n = static_cast<NativeObject*>(obj.AsObject());
        if (n->nativeType->get && n->nativeType->get(*this, *n, name, out))
            return true;
        if (!m_error.Empty())
            return false;
        return Fail(String::Format("'{}' has no member '{}'", n->nativeType->name, name));
    }
    default: break;
    }
    return Fail(String::Format("A {} has no member '{}'", TypeName(obj.Type()), name));
}

bool VM::SetMember(Value& obj, StringView name, const Value& v)
{
    switch (obj.Type()) {
    case ValueType::Vec3:
        if (const i32 c = Vec3Component(name); c >= 0) {
            if (!v.IsNumber())
                return Fail(String::Format("vec3.{} must be a number, got {}", name, TypeName(v.Type())));
            Vec3 a = obj.AsVec3();
            a[usize(c)] = v.AsFloat();
            obj = Value(a);
            return true;
        }
        break;
    case ValueType::Native: {
        auto* n = static_cast<NativeObject*>(obj.AsObject());
        if (n->nativeType->set && n->nativeType->set(*this, *n, name, v))
            return true;
        if (!m_error.Empty())
            return false;
        return Fail(String::Format("Can't set '{}' on '{}'", name, n->nativeType->name));
    }
    default: break;
    }
    return Fail(String::Format("Can't set member '{}' on a {}", name, TypeName(obj.Type())));
}

bool VM::CallMethod(Value& obj, StringView name, Span<const Value> args, Value& out)
{
    auto argc = [&](usize n) {
        if (args.Size() == n)
            return true;
        return Fail(String::Format("{}.{}() takes {} argument(s), got {}", TypeName(obj.Type()), name, n, args.Size()));
    };
    auto vecArg = [&](usize i, Vec3& v) {
        if (args[i].Type() != ValueType::Vec3 && !args[i].IsNumber())
            return Fail(String::Format("{}.{}(): argument {} must be a vec3", TypeName(obj.Type()), name, i + 1));
        v = args[i].AsVec3();
        return true;
    };
    auto numArg = [&](usize i, f32& f) {
        if (!args[i].IsNumber())
            return Fail(String::Format("{}.{}(): argument {} must be a number", TypeName(obj.Type()), name, i + 1));
        f = args[i].AsFloat();
        return true;
    };

    switch (obj.Type()) {
    case ValueType::Vec3: {
        const Vec3 a = obj.AsVec3();
        Vec3 b;
        f32 t;
        if (name == "length")
            return argc(0) && (out = Value(Length(a)), true);
        if (name == "lengthSquared")
            return argc(0) && (out = Value(LengthSq(a)), true);
        if (name == "normalized")
            return argc(0) && (out = Value(SafeNormalize(a)), true);
        if (name == "dot")
            return argc(1) && vecArg(0, b) && (out = Value(Dot(a, b)), true);
        if (name == "cross")
            return argc(1) && vecArg(0, b) && (out = Value(Cross(a, b)), true);
        if (name == "distance")
            return argc(1) && vecArg(0, b) && (out = Value(Distance(a, b)), true);
        if (name == "lerp")
            return argc(2) && vecArg(0, b) && numArg(1, t) && (out = Value(Lerp(a, b, t)), true);
        break;
    }
    case ValueType::Quat: {
        const Quat q = obj.AsQuat();
        Vec3 v;
        f32 t;
        if (name == "forward")
            return argc(0) && (out = Value(q.GetForward()), true);
        if (name == "right")
            return argc(0) && (out = Value(q.GetRight()), true);
        if (name == "up")
            return argc(0) && (out = Value(q.GetUp()), true);
        if (name == "inverse")
            return argc(0) && (out = Value(Inverse(q)), true);
        if (name == "euler")
            return argc(0) && (out = Value(q.ToEulerDegrees()), true);
        if (name == "rotate")
            return argc(1) && vecArg(0, v) && (out = Value(q * v), true);
        if (name == "slerp") {
            if (!argc(2) || !numArg(1, t))
                return false;
            if (args[0].Type() != ValueType::Quat)
                return Fail("quat.slerp(): argument 1 must be a quat");
            out = Value(Slerp(q, args[0].AsQuat(), t));
            return true;
        }
        break;
    }
    case ValueType::String: {
        const StringView s = obj.AsString();
        if (name == "len")
            return argc(0) && (out = Value(f64(s.Size())), true);
        if (name == "upper")
            return argc(0) && (out = Value(String(s).ToUpper().View()), true);
        if (name == "lower")
            return argc(0) && (out = Value(String(s).ToLower().View()), true);
        if (name == "contains")
            return argc(1) && (out = Value(s.Contains(args[0].AsString())), true);
        if (name == "startsWith")
            return argc(1) && (out = Value(s.StartsWith(args[0].AsString())), true);
        break;
    }
    case ValueType::List: {
        ListObject* l = AsList(obj);
        usize i;
        if (name == "len")
            return argc(0) && (out = Value(f64(l->items.Size())), true);
        if (name == "push") {
            if (!argc(1))
                return false;
            l->items.PushBack(args[0]);
            return true;
        }
        if (name == "pop") {
            if (!argc(0))
                return false;
            if (l->items.Empty())
                return Fail("pop() on an empty list");
            out = l->items.PopBackValue();
            return true;
        }
        if (name == "insert") {
            if (!argc(2))
                return false;
            if (args[0].AsNumber(-1) == f64(l->items.Size())) {
                l->items.PushBack(args[1]);
                return true;
            }
            if (!ResolveIndex(*this, args[0], l->items.Size(), i))
                return false;
            l->items.Insert(i, args[1]);
            return true;
        }
        if (name == "remove") {
            if (!argc(1) || !ResolveIndex(*this, args[0], l->items.Size(), i))
                return false;
            out = l->items[i];
            l->items.RemoveAt(i);
            return true;
        }
        if (name == "contains" || name == "indexOf") {
            if (!argc(1))
                return false;
            f64 found = -1;
            for (usize k = 0; k < l->items.Size(); ++k)
                if (l->items[k] == args[0]) {
                    found = f64(k);
                    break;
                }
            out = name == "contains" ? Value(found >= 0) : Value(found);
            return true;
        }
        if (name == "clear") {
            if (!argc(0))
                return false;
            l->items.Clear();
            return true;
        }
        break;
    }
    case ValueType::Native: {
        auto* n = static_cast<NativeObject*>(obj.AsObject());
        if (n->nativeType->call && n->nativeType->call(*this, *n, name, args, out))
            return true;
        if (!m_error.Empty())
            return false;
        // A member holding a function (e.g. a namespace object).
        Value member;
        if (n->nativeType->get && n->nativeType->get(*this, *n, name, member) &&
            (member.Type() == ValueType::NativeFunc || member.Type() == ValueType::Function))
            return CallValue(member, args, &out);
        if (!m_error.Empty())
            return false;
        return Fail(String::Format("'{}' has no function '{}'", n->nativeType->name, name));
    }
    default: break;
    }
    return Fail(String::Format("A {} has no function '{}'", TypeName(obj.Type()), name));
}

namespace {

bool Arith(VM& vm, Op op, const Value& a, const Value& b, Value& out)
{
    const ValueType ta = a.Type(), tb = b.Type();
    if (ta == ValueType::Number && tb == ValueType::Number) {
        const f64 x = a.AsNumber(), y = b.AsNumber();
        switch (op) {
        case Op::Add: out = Value(x + y); return true;
        case Op::Sub: out = Value(x - y); return true;
        case Op::Mul: out = Value(x * y); return true;
        case Op::Div: out = Value(x / y); return true;
        case Op::Mod: out = Value(y == 0.0 ? 0.0 : x - y * std::floor(x / y)); return true;
        default: break;
        }
    }
    if (op == Op::Add && (ta == ValueType::String || tb == ValueType::String)) {
        String s = a.ToString();
        s += b.ToString();
        out = Value(s.View());
        return true;
    }
    if (op == Op::Add && ta == ValueType::List && tb == ValueType::List) {
        out = MakeList(AsList(a)->items.View());
        AsList(out)->items.Append(AsList(b)->items.View());
        return true;
    }
    const bool va = ta == ValueType::Vec3, vb = tb == ValueType::Vec3;
    if ((va || ta == ValueType::Number) && (vb || tb == ValueType::Number) && (va || vb)) {
        const Vec3 x = a.AsVec3(), y = b.AsVec3();
        switch (op) {
        case Op::Add: out = Value(x + y); return true;
        case Op::Sub: out = Value(x - y); return true;
        case Op::Mul: out = Value(x * y); return true;
        case Op::Div: out = Value(x / y); return true;
        default: break;
        }
    }
    if (op == Op::Mul && ta == ValueType::Quat && tb == ValueType::Quat) {
        out = Value(a.AsQuat() * b.AsQuat());
        return true;
    }
    if (op == Op::Mul && ta == ValueType::Quat && vb) {
        out = Value(a.AsQuat() * b.AsVec3());
        return true;
    }
    static const char* const Names[] = {"+", "-", "*", "/", "%"};
    return vm.Fail(String::Format("Can't use '{}' on {} and {}", Names[u8(op) - u8(Op::Add)], TypeName(ta), TypeName(tb)));
}

bool Compare(VM& vm, Op op, const Value& a, const Value& b, bool& out)
{
    i32 c;
    if (a.IsNumber() && b.IsNumber()) {
        const f64 x = a.AsNumber(), y = b.AsNumber();
        switch (op) {
        case Op::Lt: out = x < y; return true;
        case Op::Le: out = x <= y; return true;
        case Op::Gt: out = x > y; return true;
        default: out = x >= y; return true;
        }
    }
    if (a.Type() == ValueType::String && b.Type() == ValueType::String) {
        const StringView x = a.AsString(), y = b.AsString();
        const usize n = x.Size() < y.Size() ? x.Size() : y.Size();
        c = n ? MemCompare(x.Data(), y.Data(), n) : 0;
        if (c == 0)
            c = x.Size() < y.Size() ? -1 : x.Size() > y.Size() ? 1 : 0;
    } else {
        return vm.Fail(String::Format("Can't compare {} and {}", TypeName(a.Type()), TypeName(b.Type())));
    }
    switch (op) {
    case Op::Lt: out = c < 0; break;
    case Op::Le: out = c <= 0; break;
    case Op::Gt: out = c > 0; break;
    default: out = c >= 0; break;
    }
    return true;
}

} // namespace

bool VM::Run(const Program& program, u32 function, Array<Value>& fields, Span<const Value> args, Value* result)
{
    if (m_depth >= MaxCallDepth)
        return Fail("Stack overflow: too many nested calls (endless recursion?)");
    const FunctionCode& fn = program.functions[function];
    if (m_stack.Size() + fn.locals + 256 > StackCapacity)
        return Fail("Stack overflow");

    const usize base = m_stack.Size();
    for (u32 i = 0; i < fn.locals; ++i)
        m_stack.PushBack(i < args.Size() && i < fn.params ? args[i] : Value());
    ++m_depth;
    m_fieldStack.PushBack(&fields);

    const u8* code = fn.code.Data();
    usize ip = 0;
    usize opStart = 0;
    bool ok = true;
    Value ret;

    auto u16At = [&]() {
        const u16 v = u16(code[ip] | (u16(code[ip + 1]) << 8));
        ip += 2;
        return v;
    };
    auto pop = [&]() { return m_stack.PopBackValue(); };
    auto push = [&](Value v) {
        if (m_stack.Size() >= StackCapacity)
            return Fail("Stack overflow");
        m_stack.PushBack(move(v));
        return true;
    };
    auto popN = [&](usize n) {
        for (usize i = 0; i < n; ++i)
            m_stack.PopBack();
    };

    while (ok) {
        if (m_instructionLimit && ++m_executed > m_instructionLimit) {
            ok = Fail(String::Format("Script took too long (more than {} steps) - endless loop?", m_instructionLimit));
            break;
        }
        opStart = ip;
        const Op op = Op(code[ip++]);
        switch (op) {
        case Op::Const: ok = push(program.constants[u16At()]); break;
        case Op::Nil: ok = push(Value()); break;
        case Op::True: ok = push(Value(true)); break;
        case Op::False: ok = push(Value(false)); break;
        case Op::Pop: m_stack.PopBack(); break;
        case Op::Dup: ok = push(m_stack.Back()); break;
        case Op::LoadLocal: ok = push(m_stack[base + u16At()]); break;
        case Op::StoreLocal: {
            const u16 slot = u16At();
            m_stack[base + slot] = pop();
            break;
        }
        case Op::LoadField: ok = push(fields[u16At()]); break;
        case Op::StoreField: {
            const u16 f = u16At();
            fields[f] = pop();
            break;
        }
        case Op::LoadGlobal: {
            const Value& name = program.constants[u16At()];
            const Value* g = FindGlobal(name.AsString());
            ok = g ? push(*g) : Fail(String::Format("Unknown name '{}'", name.AsString()));
            break;
        }
        case Op::GetMember: {
            const Value& name = program.constants[u16At()];
            const Value obj = pop();
            Value out;
            ok = GetMember(obj, name.AsString(), out) && push(move(out));
            break;
        }
        case Op::SetMember: {
            const Value& name = program.constants[u16At()];
            const Value v = pop();
            Value obj = pop();
            ok = SetMember(obj, name.AsString(), v) && push(move(obj));
            break;
        }
        case Op::GetIndex: {
            const Value index = pop();
            const Value obj = pop();
            usize i = 0;
            if (const ListObject* l = AsList(obj)) {
                ok = ResolveIndex(*this, index, l->items.Size(), i) && push(l->items[i]);
            } else if (obj.Type() == ValueType::String) {
                ok = ResolveIndex(*this, index, obj.AsString().Size(), i) && push(Value(obj.AsString().SubStr(i, 1)));
            } else if (obj.Type() == ValueType::Vec3) {
                ok = ResolveIndex(*this, index, 3, i) && push(Value(obj.AsVec3()[i]));
            } else {
                ok = Fail(String::Format("Can't index a {}", TypeName(obj.Type())));
            }
            break;
        }
        case Op::SetIndex: {
            const Value v = pop();
            const Value index = pop();
            Value obj = pop();
            usize i = 0;
            if (ListObject* l = AsList(obj)) {
                ok = ResolveIndex(*this, index, l->items.Size(), i);
                if (ok)
                    l->items[i] = v;
            } else if (obj.Type() == ValueType::Vec3) {
                ok = ResolveIndex(*this, index, 3, i);
                if (ok && !v.IsNumber())
                    ok = Fail("vec3 components must be numbers");
                if (ok) {
                    Vec3 a = obj.AsVec3();
                    a[i] = v.AsFloat();
                    obj = Value(a);
                }
            } else {
                ok = Fail(String::Format("Can't set an index on a {}", TypeName(obj.Type())));
            }
            ok = ok && push(move(obj));
            break;
        }
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod: {
            const Value b = pop();
            const Value a = pop();
            Value out;
            ok = Arith(*this, op, a, b, out) && push(move(out));
            break;
        }
        case Op::Neg: {
            const Value a = pop();
            if (a.IsNumber())
                ok = push(Value(-a.AsNumber()));
            else if (a.Type() == ValueType::Vec3)
                ok = push(Value(-a.AsVec3()));
            else
                ok = Fail(String::Format("Can't negate a {}", TypeName(a.Type())));
            break;
        }
        case Op::Not: ok = push(Value(!pop().AsBool())); break;
        case Op::Eq:
        case Op::Ne: {
            const Value b = pop();
            const Value a = pop();
            ok = push(Value((a == b) == (op == Op::Eq)));
            break;
        }
        case Op::Lt:
        case Op::Le:
        case Op::Gt:
        case Op::Ge: {
            const Value b = pop();
            const Value a = pop();
            bool r = false;
            ok = Compare(*this, op, a, b, r) && push(Value(r));
            break;
        }
        case Op::Jump:
        case Op::JumpIfFalse:
        case Op::AndJump:
        case Op::OrJump: {
            i32 rel;
            MemCopy(&rel, code + ip, 4);
            ip += 4;
            bool jump = true;
            if (op == Op::JumpIfFalse) {
                jump = !pop().AsBool();
            } else if (op == Op::AndJump || op == Op::OrJump) {
                jump = m_stack.Back().AsBool() == (op == Op::OrJump);
                if (!jump)
                    m_stack.PopBack();
            }
            if (jump)
                ip = usize(i64(ip) + rel);
            break;
        }
        case Op::Call: {
            const u8 argc = code[ip++];
            const usize calleeAt = m_stack.Size() - argc - 1;
            const Value callee = m_stack[calleeAt];
            Value out;
            if (callee.Type() == ValueType::Function && callee.FunctionProgram() != &program) {
                ok = Fail("Can't call a function of another script");
            } else {
                ok = CallValue(callee, Span<const Value>(m_stack.Data() + calleeAt + 1, argc), &out);
            }
            popN(usize(argc) + 1);
            ok = ok && push(move(out));
            break;
        }
        case Op::CallFn: {
            const u16 fi = u16At();
            const u8 argc = code[ip++];
            Value out;
            ok = Run(program, fi, fields, Span<const Value>(m_stack.Data() + m_stack.Size() - argc, argc), &out);
            popN(argc);
            ok = ok && push(move(out));
            break;
        }
        case Op::CallMethod: {
            const Value& name = program.constants[u16At()];
            const u8 argc = code[ip++];
            const usize objAt = m_stack.Size() - argc - 1;
            Value obj = m_stack[objAt];
            Value out;
            ok = CallMethod(obj, name.AsString(), Span<const Value>(m_stack.Data() + objAt + 1, argc), out);
            popN(usize(argc) + 1);
            ok = ok && push(move(out));
            break;
        }
        case Op::MakeList: {
            const u16 count = u16At();
            Value list = MakeList(Span<const Value>(m_stack.Data() + m_stack.Size() - count, count));
            popN(count);
            ok = push(move(list));
            break;
        }
        case Op::Return: ret = pop(); goto done;
        case Op::ReturnNil: goto done;
        default: ok = Fail("Corrupt bytecode"); break;
        }
    }
done:
    if (!ok && !m_errorLocated) {
        const u32 line = opStart < fn.lines.Size() ? fn.lines[opStart] : 0;
        m_error = String::Format("{}:{}: {} (in {}())", program.source, line, m_error, fn.name);
        m_errorLocated = true;
    }
    while (m_stack.Size() > base)
        m_stack.PopBack();
    m_fieldStack.PopBack();
    --m_depth;
    if (ok && result)
        *result = move(ret);
    return ok;
}

// --- Instance ----------------------------------------------------------------------------------------------------------------

Instance::Instance(VM& vm, Program* program, Span<const Value> presetFields) : m_vm(vm), m_program(program)
{
    ++m_program->refCount;
    m_fields.Resize(program->fields.Size());
    for (const Property& p : program->properties)
        if (p.field < presetFields.Size() && !presetFields[p.field].IsNil())
            m_fields[p.field] = presetFields[p.field];
    m_ok = m_vm.Call(*m_program, 0, m_fields, {});
}

Instance::~Instance()
{
    m_fields.Clear();
    --m_program->refCount;
}

bool Instance::Call(StringView function, Span<const Value> args, Value* result)
{
    if (!m_ok)
        return false;
    const i32 fi = m_program->FindFunction(function);
    if (fi < 0)
        return true;
    const FunctionCode& fn = m_program->functions[usize(fi)];
    if (args.Size() > fn.params)
        args = Span<const Value>(args.Data(), fn.params); // the script may ignore engine-provided arguments
    m_ok = m_vm.Call(*m_program, u32(fi), m_fields, args, result);
    return m_ok;
}

Value* Instance::Field(StringView name)
{
    for (usize i = 0; i < m_program->fields.Size(); ++i)
        if (m_program->fields[i] == name)
            return &m_fields[i];
    return nullptr;
}

// --- Standard library --------------------------------------------------------------------------------------------------------

namespace {

bool Numbers(VM& vm, StringView fn, Span<const Value> args, usize count, f64* out)
{
    if (args.Size() != count)
        return vm.Fail(String::Format("{}() takes {} argument(s), got {}", fn, count, args.Size()));
    for (usize i = 0; i < count; ++i) {
        if (!args[i].IsNumber())
            return vm.Fail(String::Format("{}(): argument {} must be a number, got {}", fn, i + 1, TypeName(args[i].Type())));
        out[i] = args[i].AsNumber();
    }
    return true;
}

using Unary = f64 (*)(f64);
struct UnaryFn
{
    const char* name;
    Unary fn;
};

} // namespace

void RegisterStandardLibrary(VM& vm)
{
    static const UnaryFn Unaries[] = {
        {"sin", [](f64 x) { return std::sin(x); }},
        {"cos", [](f64 x) { return std::cos(x); }},
        {"tan", [](f64 x) { return std::tan(x); }},
        {"asin", [](f64 x) { return std::asin(x); }},
        {"acos", [](f64 x) { return std::acos(x); }},
        {"atan", [](f64 x) { return std::atan(x); }},
        {"sqrt", [](f64 x) { return std::sqrt(x); }},
        {"abs", [](f64 x) { return std::fabs(x); }},
        {"floor", [](f64 x) { return std::floor(x); }},
        {"ceil", [](f64 x) { return std::ceil(x); }},
        {"round", [](f64 x) { return std::round(x); }},
        {"exp", [](f64 x) { return std::exp(x); }},
        {"sign", [](f64 x) { return x > 0 ? 1.0 : x < 0 ? -1.0 : 0.0; }},
        {"fract", [](f64 x) { return x - std::floor(x); }},
        {"deg", [](f64 x) { return x * (180.0 / 3.14159265358979323846); }},
        {"rad", [](f64 x) { return x * (3.14159265358979323846 / 180.0); }},
    };
    for (const UnaryFn& u : Unaries) {
        const char* name = u.name;
        const Unary f = u.fn;
        vm.SetGlobal(name, MakeNativeFunction(name, [name, f](VM& v, Span<const Value> a, Value& r) {
                         f64 x;
                         if (!Numbers(v, name, a, 1, &x))
                             return false;
                         r = Value(f(x));
                         return true;
                     }));
    }
    vm.SetGlobal("PI", Value(3.14159265358979323846));

    vm.SetGlobal("atan2", MakeNativeFunction("atan2", [](VM& v, Span<const Value> a, Value& r) {
                     f64 x[2];
                     return Numbers(v, "atan2", a, 2, x) && (r = Value(std::atan2(x[0], x[1])), true);
                 }));
    vm.SetGlobal("pow", MakeNativeFunction("pow", [](VM& v, Span<const Value> a, Value& r) {
                     f64 x[2];
                     return Numbers(v, "pow", a, 2, x) && (r = Value(std::pow(x[0], x[1])), true);
                 }));
    vm.SetGlobal("min", MakeNativeFunction("min", [](VM& v, Span<const Value> a, Value& r) {
                     f64 x[2];
                     return Numbers(v, "min", a, 2, x) && (r = Value(x[0] < x[1] ? x[0] : x[1]), true);
                 }));
    vm.SetGlobal("max", MakeNativeFunction("max", [](VM& v, Span<const Value> a, Value& r) {
                     f64 x[2];
                     return Numbers(v, "max", a, 2, x) && (r = Value(x[0] > x[1] ? x[0] : x[1]), true);
                 }));
    vm.SetGlobal("clamp", MakeNativeFunction("clamp", [](VM& v, Span<const Value> a, Value& r) {
                     f64 x[3];
                     return Numbers(v, "clamp", a, 3, x) && (r = Value(x[0] < x[1] ? x[1] : x[0] > x[2] ? x[2] : x[0]), true);
                 }));
    vm.SetGlobal("smoothstep", MakeNativeFunction("smoothstep", [](VM& v, Span<const Value> a, Value& r) {
                     f64 x[3];
                     if (!Numbers(v, "smoothstep", a, 3, x))
                         return false;
                     f64 t = x[1] == x[0] ? 0.0 : (x[2] - x[0]) / (x[1] - x[0]);
                     t = t < 0 ? 0 : t > 1 ? 1 : t;
                     r = Value(t * t * (3.0 - 2.0 * t));
                     return true;
                 }));
    vm.SetGlobal("lerp", MakeNativeFunction("lerp", [](VM& v, Span<const Value> a, Value& r) {
                     if (a.Size() == 3 && a[0].Type() == ValueType::Vec3 && a[2].IsNumber()) {
                         r = Value(Lerp(a[0].AsVec3(), a[1].AsVec3(), a[2].AsFloat()));
                         return true;
                     }
                     if (a.Size() == 3 && a[0].Type() == ValueType::Quat && a[1].Type() == ValueType::Quat && a[2].IsNumber()) {
                         r = Value(Slerp(a[0].AsQuat(), a[1].AsQuat(), a[2].AsFloat()));
                         return true;
                     }
                     f64 x[3];
                     return Numbers(v, "lerp", a, 3, x) && (r = Value(x[0] + (x[1] - x[0]) * x[2]), true);
                 }));
    vm.SetGlobal("random", MakeNativeFunction("random", [](VM& v, Span<const Value> a, Value& r) {
                     if (a.Empty()) {
                         r = Value(v.Rng().NextFloat());
                         return true;
                     }
                     f64 x[2];
                     return Numbers(v, "random", a, 2, x) && (r = Value(x[0] + (x[1] - x[0]) * v.Rng().NextFloat()), true);
                 }));
    vm.SetGlobal("randomInt", MakeNativeFunction("randomInt", [](VM& v, Span<const Value> a, Value& r) {
                     f64 x[2];
                     if (!Numbers(v, "randomInt", a, 2, x))
                         return false;
                     if (x[1] < x[0])
                         return v.Fail("randomInt(a, b): b must be >= a");
                     r = Value(v.Rng().Range(i32(x[0]), i32(x[1])));
                     return true;
                 }));

    vm.SetGlobal("vec3", MakeNativeFunction("vec3", [](VM& v, Span<const Value> a, Value& r) {
                     if (a.Empty()) {
                         r = Value(Vec3(0.0f));
                         return true;
                     }
                     if (a.Size() == 1 && a[0].IsNumber()) {
                         r = Value(Vec3(a[0].AsFloat()));
                         return true;
                     }
                     f64 x[3];
                     return Numbers(v, "vec3", a, 3, x) && (r = Value(Vec3(f32(x[0]), f32(x[1]), f32(x[2]))), true);
                 }));

    // quat namespace: quat.euler(pitch, yaw, roll) in degrees, quat.axisAngle(axis, degrees), quat.lookAt(dir[, up]),
    // quat.identity.
    NativeType& quatType = vm.AddType("quat");
    quatType.get = [](VM&, NativeObject&, StringView member, Value& out) {
        if (member == "identity") {
            out = Value(Quat::Identity());
            return true;
        }
        return false;
    };
    quatType.call = [](VM& v, NativeObject&, StringView method, Span<const Value> a, Value& out) {
        if (method == "euler") {
            if (a.Size() == 1 && a[0].Type() == ValueType::Vec3) {
                out = Value(Quat::FromEulerDegrees(a[0].AsVec3()));
                return true;
            }
            f64 x[3];
            return Numbers(v, "quat.euler", a, 3, x) &&
                   (out = Value(Quat::FromEulerDegrees(Vec3(f32(x[0]), f32(x[1]), f32(x[2])))), true);
        }
        if (method == "axisAngle") {
            if (a.Size() != 2 || a[0].Type() != ValueType::Vec3 || !a[1].IsNumber())
                return v.Fail("quat.axisAngle(axis: vec3, degrees)");
            out = Value(Quat::FromAxisAngle(SafeNormalize(a[0].AsVec3(), Vec3::Up()), DegToRad(a[1].AsFloat())));
            return true;
        }
        if (method == "lookAt") {
            if (a.Empty() || a.Size() > 2 || a[0].Type() != ValueType::Vec3)
                return v.Fail("quat.lookAt(direction: vec3[, up: vec3])");
            const Vec3 dir = a[0].AsVec3();
            if (LengthSq(dir) < 1e-12f) {
                out = Value(Quat::Identity());
                return true;
            }
            out = Value(Quat::LookRotation(Normalize(dir), a.Size() == 2 ? a[1].AsVec3() : Vec3::Up()));
            return true;
        }
        return false;
    };
    vm.SetGlobal("quat", MakeNative(quatType, nullptr));

    auto print = [](VM& v, Span<const Value> a, Value&) {
        String line;
        for (usize i = 0; i < a.Size(); ++i) {
            if (i)
                line += ' ';
            line += a[i].ToString();
        }
        if (v.onLog)
            v.onLog(line);
        return true;
    };
    vm.SetGlobal("print", MakeNativeFunction("print", print));
    vm.SetGlobal("log", MakeNativeFunction("log", print));
    vm.SetGlobal("str", MakeNativeFunction("str", [](VM& v, Span<const Value> a, Value& r) {
                     if (a.Size() != 1)
                         return v.Fail("str() takes 1 argument");
                     r = Value(a[0].ToString().View());
                     return true;
                 }));
    vm.SetGlobal("num", MakeNativeFunction("num", [](VM& v, Span<const Value> a, Value& r) {
                     if (a.Size() != 1)
                         return v.Fail("num() takes 1 argument");
                     if (a[0].IsNumber()) {
                         r = a[0];
                     } else if (const Optional<f64> d = ParseDouble(a[0].AsString())) {
                         r = Value(*d);
                     }
                     return true; // nil when not a number
                 }));
    vm.SetGlobal("len", MakeNativeFunction("len", [](VM& v, Span<const Value> a, Value& r) {
                     if (a.Size() != 1)
                         return v.Fail("len() takes 1 argument");
                     if (const ListObject* l = AsList(a[0]))
                         r = Value(f64(l->items.Size()));
                     else if (a[0].Type() == ValueType::String)
                         r = Value(f64(a[0].AsString().Size()));
                     else
                         return v.Fail(String::Format("len() of a {}", TypeName(a[0].Type())));
                     return true;
                 }));
    vm.SetGlobal("type", MakeNativeFunction("type", [](VM& v, Span<const Value> a, Value& r) {
                     if (a.Size() != 1)
                         return v.Fail("type() takes 1 argument");
                     r = Value(TypeName(a[0].Type()));
                     return true;
                 }));
}

} // namespace zx::script
