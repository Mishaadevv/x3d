// ZeqouScript front end: lexer -> AST (recursive descent) -> bytecode.

#include "Bytecode.h"
#include "Script/ZeqouScript.h"
#include "Core/Memory/Pointers.h"
#include "Core/String/Parse.h"

namespace zx::script {

namespace {

// --- Lexer -------------------------------------------------------------------------------------------------------------

enum class Tok : u8
{
    End,
    Ident,
    Number,
    String,
    // punctuation
    LParen, RParen, LBrace, RBrace, LBracket, RBracket, Comma, Dot, DotDot, Colon, Semicolon, At,
    Plus, Minus, Star, Slash, Percent, Bang,
    Assign, PlusAssign, MinusAssign, StarAssign, SlashAssign,
    Eq, Ne, Lt, Le, Gt, Ge, AndAnd, OrOr,
    // keywords
    KwScript, KwProp, KwVar, KwLet, KwFn, KwIf, KwElif, KwElse, KwWhile, KwFor, KwIn, KwBreak, KwContinue, KwReturn,
    KwTrue, KwFalse, KwNil, KwAnd, KwOr, KwNot, KwSelf,
};

struct Token
{
    Tok type = Tok::End;
    StringView text;
    f64 number = 0.0;
    String string; // unescaped
    u32 line = 1, column = 1;
    bool newlineBefore = false;
};

struct Keyword
{
    StringView text;
    Tok tok;
};
const Keyword Keywords[] = {
    {"script", Tok::KwScript}, {"prop", Tok::KwProp},   {"var", Tok::KwVar},   {"let", Tok::KwLet},
    {"fn", Tok::KwFn},         {"if", Tok::KwIf},       {"elif", Tok::KwElif}, {"else", Tok::KwElse},
    {"while", Tok::KwWhile},   {"for", Tok::KwFor},     {"in", Tok::KwIn},     {"break", Tok::KwBreak},
    {"continue", Tok::KwContinue}, {"return", Tok::KwReturn}, {"true", Tok::KwTrue}, {"false", Tok::KwFalse},
    {"nil", Tok::KwNil},       {"and", Tok::KwAnd},     {"or", Tok::KwOr},     {"not", Tok::KwNot},
    {"self", Tok::KwSelf},
};

struct Lexer
{
    StringView src;
    usize pos = 0;
    u32 line = 1, col = 1;
    String error;
    u32 errorLine = 0, errorCol = 0;

    char Peek(usize o = 0) const { return pos + o < src.Size() ? src[pos + o] : '\0'; }
    char Advance()
    {
        const char c = src[pos++];
        if (c == '\n') {
            ++line;
            col = 1;
        } else {
            ++col;
        }
        return c;
    }
    bool Fail(StringView msg)
    {
        if (error.Empty()) {
            error = msg;
            errorLine = line;
            errorCol = col;
        }
        return false;
    }

    bool Next(Token& t)
    {
        t = Token();
        // Whitespace and comments.
        while (pos < src.Size()) {
            const char c = Peek();
            if (c == '\n') {
                t.newlineBefore = true;
                Advance();
            } else if (c == ' ' || c == '\t' || c == '\r') {
                Advance();
            } else if (c == '/' && Peek(1) == '/') {
                while (pos < src.Size() && Peek() != '\n')
                    Advance();
            } else if (c == '/' && Peek(1) == '*') {
                Advance();
                Advance();
                while (pos < src.Size() && !(Peek() == '*' && Peek(1) == '/'))
                    Advance();
                if (pos >= src.Size())
                    return Fail("Unterminated /* comment");
                Advance();
                Advance();
            } else {
                break;
            }
        }
        t.line = line;
        t.column = col;
        if (pos >= src.Size()) {
            t.type = Tok::End;
            return true;
        }
        const usize start = pos;
        const char c = Advance();
        auto two = [&](char next, Tok a, Tok b) {
            if (Peek() == next) {
                Advance();
                return a;
            }
            return b;
        };
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            while ((Peek() >= 'a' && Peek() <= 'z') || (Peek() >= 'A' && Peek() <= 'Z') || (Peek() >= '0' && Peek() <= '9') || Peek() == '_')
                Advance();
            t.text = src.SubStr(start, pos - start);
            t.type = Tok::Ident;
            for (const Keyword& k : Keywords)
                if (k.text == t.text)
                    t.type = k.tok;
            return true;
        }
        if ((c >= '0' && c <= '9') || (c == '.' && Peek() >= '0' && Peek() <= '9')) {
            while ((Peek() >= '0' && Peek() <= '9') || Peek() == '_')
                Advance();
            if (Peek() == '.' && Peek(1) != '.') {
                Advance();
                while (Peek() >= '0' && Peek() <= '9')
                    Advance();
            }
            if (Peek() == 'e' || Peek() == 'E') {
                Advance();
                if (Peek() == '+' || Peek() == '-')
                    Advance();
                while (Peek() >= '0' && Peek() <= '9')
                    Advance();
            }
            t.text = src.SubStr(start, pos - start);
            String digits;
            for (char ch : t.text)
                if (ch != '_')
                    digits += ch;
            const Optional<f64> v = ParseDouble(digits);
            if (!v)
                return Fail(String::Format("Invalid number '{}'", t.text));
            t.number = *v;
            t.type = Tok::Number;
            return true;
        }
        if (c == '"') {
            while (pos < src.Size() && Peek() != '"' && Peek() != '\n') {
                char ch = Advance();
                if (ch == '\\') {
                    const char e = Advance();
                    ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == '0' ? '\0' : e;
                }
                t.string += ch;
            }
            if (Peek() != '"')
                return Fail("Unterminated string");
            Advance();
            t.text = src.SubStr(start, pos - start);
            t.type = Tok::String;
            return true;
        }
        switch (c) {
        case '(': t.type = Tok::LParen; break;
        case ')': t.type = Tok::RParen; break;
        case '{': t.type = Tok::LBrace; break;
        case '}': t.type = Tok::RBrace; break;
        case '[': t.type = Tok::LBracket; break;
        case ']': t.type = Tok::RBracket; break;
        case ',': t.type = Tok::Comma; break;
        case ':': t.type = Tok::Colon; break;
        case ';': t.type = Tok::Semicolon; break;
        case '@': t.type = Tok::At; break;
        case '%': t.type = Tok::Percent; break;
        case '.': t.type = two('.', Tok::DotDot, Tok::Dot); break;
        case '+': t.type = two('=', Tok::PlusAssign, Tok::Plus); break;
        case '-': t.type = two('=', Tok::MinusAssign, Tok::Minus); break;
        case '*': t.type = two('=', Tok::StarAssign, Tok::Star); break;
        case '/': t.type = two('=', Tok::SlashAssign, Tok::Slash); break;
        case '!': t.type = two('=', Tok::Ne, Tok::Bang); break;
        case '=': t.type = two('=', Tok::Eq, Tok::Assign); break;
        case '<': t.type = two('=', Tok::Le, Tok::Lt); break;
        case '>': t.type = two('=', Tok::Ge, Tok::Gt); break;
        case '&':
            if (Peek() != '&')
                return Fail("Expected '&&'");
            Advance();
            t.type = Tok::AndAnd;
            break;
        case '|':
            if (Peek() != '|')
                return Fail("Expected '||'");
            Advance();
            t.type = Tok::OrOr;
            break;
        default: return Fail(String::Format("Unexpected character '{}'", c));
        }
        t.text = src.SubStr(start, pos - start);
        return true;
    }
};

// --- AST -------------------------------------------------------------------------------------------------------------------

enum class NodeKind : u8
{
    // expressions
    Number, String, Bool, Nil, Ident, Self, List, Unary, Binary, And, Or, Member, Index, Call,
    // statements
    ExprStmt, VarDecl, Assign, If, While, ForRange, ForEach, Break, Continue, Return, Block,
};

struct Node
{
    NodeKind kind;
    u32 line = 0, column = 0;
    Tok op = Tok::End;    // unary/binary/assign operator
    f64 number = 0.0;
    bool boolean = false;
    String text;          // identifier / member name / string literal
    Node* a = nullptr;    // operand / target / condition / callee
    Node* b = nullptr;    // operand / value / then-block / loop end
    Node* c = nullptr;    // else / loop body
    Array<Node*> list;    // args, list items, block statements
};

struct FieldDecl
{
    String name;
    bool isProp = false;
    Node* init = nullptr;
    u32 line = 0;
    f64 rangeMin = 0.0, rangeMax = 0.0;
    String tooltip;
    bool color = false;
};

struct FnDecl
{
    String name;
    Array<String> params;
    Node* body = nullptr;
    u32 line = 0;
};

struct Parser
{
    Lexer lex;
    Token cur, prev;
    Array<UniquePtr<Node>> nodes;
    String error;
    u32 errorLine = 0, errorCol = 0;
    String scriptName;
    Array<FieldDecl> fields;
    Array<FnDecl> fns;

    bool Fail(StringView msg, const Token& at)
    {
        if (error.Empty()) {
            error = msg;
            errorLine = at.line;
            errorCol = at.column;
        }
        return false;
    }
    bool Advance()
    {
        prev = move(cur);
        if (!lex.Next(cur)) {
            if (error.Empty()) {
                error = lex.error;
                errorLine = lex.errorLine;
                errorCol = lex.errorCol;
            }
            cur.type = Tok::End;
            return false;
        }
        return true;
    }
    bool Check(Tok t) const { return cur.type == t; }
    bool Match(Tok t)
    {
        if (!Check(t))
            return false;
        Advance();
        return true;
    }
    bool Expect(Tok t, StringView what)
    {
        if (Match(t))
            return true;
        return Fail(String::Format("Expected {}", what), cur);
    }
    Node* Make(NodeKind k, const Token& at)
    {
        nodes.PushBack(MakeUnique<Node>());
        Node* n = nodes.Back().Get();
        n->kind = k;
        n->line = at.line;
        n->column = at.column;
        return n;
    }

    // --- Expressions ---
    Node* Expression() { return OrExpr(); }
    Node* OrExpr()
    {
        Node* left = AndExpr();
        while (left && (Check(Tok::KwOr) || Check(Tok::OrOr))) {
            const Token t = cur;
            Advance();
            Node* n = Make(NodeKind::Or, t);
            n->a = left;
            n->b = AndExpr();
            if (!n->b)
                return nullptr;
            left = n;
        }
        return left;
    }
    Node* AndExpr()
    {
        Node* left = Equality();
        while (left && (Check(Tok::KwAnd) || Check(Tok::AndAnd))) {
            const Token t = cur;
            Advance();
            Node* n = Make(NodeKind::And, t);
            n->a = left;
            n->b = Equality();
            if (!n->b)
                return nullptr;
            left = n;
        }
        return left;
    }
    Node* Binary(Node* (Parser::*next)(), std::initializer_list<Tok> ops)
    {
        Node* left = (this->*next)();
        while (left) {
            bool found = false;
            for (Tok op : ops)
                found |= Check(op);
            if (!found)
                break;
            const Token t = cur;
            Advance();
            Node* n = Make(NodeKind::Binary, t);
            n->op = t.type;
            n->a = left;
            n->b = (this->*next)();
            if (!n->b)
                return nullptr;
            left = n;
        }
        return left;
    }
    Node* Equality() { return Binary(&Parser::Comparison, {Tok::Eq, Tok::Ne}); }
    Node* Comparison() { return Binary(&Parser::Term, {Tok::Lt, Tok::Le, Tok::Gt, Tok::Ge}); }
    Node* Term() { return Binary(&Parser::Factor, {Tok::Plus, Tok::Minus}); }
    Node* Factor() { return Binary(&Parser::Unary, {Tok::Star, Tok::Slash, Tok::Percent}); }
    Node* Unary()
    {
        if (Check(Tok::Minus) || Check(Tok::Bang) || Check(Tok::KwNot)) {
            const Token t = cur;
            Advance();
            Node* n = Make(NodeKind::Unary, t);
            n->op = t.type == Tok::Minus ? Tok::Minus : Tok::Bang;
            n->a = Unary();
            return n->a ? n : nullptr;
        }
        return Postfix();
    }
    Node* Postfix()
    {
        Node* e = Primary();
        while (e) {
            // A '(' or '[' on a new line starts a new statement, not a call/index.
            if (Check(Tok::LParen) && !cur.newlineBefore) {
                const Token t = cur;
                Advance();
                Node* call = Make(NodeKind::Call, t);
                call->a = e;
                if (!Check(Tok::RParen)) {
                    do {
                        Node* arg = Expression();
                        if (!arg)
                            return nullptr;
                        call->list.PushBack(arg);
                    } while (Match(Tok::Comma));
                }
                if (!Expect(Tok::RParen, "')' after arguments"))
                    return nullptr;
                e = call;
            } else if (Check(Tok::Dot)) {
                const Token t = cur;
                Advance();
                if (!Check(Tok::Ident) && !(cur.type >= Tok::KwScript)) {
                    Fail("Expected a member name after '.'", cur);
                    return nullptr;
                }
                Node* m = Make(NodeKind::Member, t);
                m->a = e;
                m->text = cur.text;
                Advance();
                e = m;
            } else if (Check(Tok::LBracket) && !cur.newlineBefore) {
                const Token t = cur;
                Advance();
                Node* ix = Make(NodeKind::Index, t);
                ix->a = e;
                ix->b = Expression();
                if (!ix->b || !Expect(Tok::RBracket, "']'"))
                    return nullptr;
                e = ix;
            } else {
                break;
            }
        }
        return e;
    }
    Node* Primary()
    {
        const Token t = cur;
        switch (cur.type) {
        case Tok::Number: {
            Advance();
            Node* n = Make(NodeKind::Number, t);
            n->number = t.number;
            return n;
        }
        case Tok::String: {
            Advance();
            Node* n = Make(NodeKind::String, t);
            n->text = t.string;
            return n;
        }
        case Tok::KwTrue:
        case Tok::KwFalse: {
            Advance();
            Node* n = Make(NodeKind::Bool, t);
            n->boolean = t.type == Tok::KwTrue;
            return n;
        }
        case Tok::KwNil: Advance(); return Make(NodeKind::Nil, t);
        case Tok::KwSelf: Advance(); return Make(NodeKind::Self, t);
        case Tok::Ident: {
            Advance();
            Node* n = Make(NodeKind::Ident, t);
            n->text = t.text;
            return n;
        }
        case Tok::LParen: {
            Advance();
            Node* e = Expression();
            if (!e || !Expect(Tok::RParen, "')'"))
                return nullptr;
            return e;
        }
        case Tok::LBracket: {
            Advance();
            Node* n = Make(NodeKind::List, t);
            if (!Check(Tok::RBracket)) {
                do {
                    if (Check(Tok::RBracket))
                        break; // trailing comma
                    Node* item = Expression();
                    if (!item)
                        return nullptr;
                    n->list.PushBack(item);
                } while (Match(Tok::Comma));
            }
            if (!Expect(Tok::RBracket, "']'"))
                return nullptr;
            return n;
        }
        default:
            Fail(t.type == Tok::End ? String("Unexpected end of file") : String::Format("Unexpected '{}'", t.text), t);
            return nullptr;
        }
    }

    // --- Statements ---
    Node* Block()
    {
        const Token t = cur;
        if (!Expect(Tok::LBrace, "'{'"))
            return nullptr;
        Node* b = Make(NodeKind::Block, t);
        while (!Check(Tok::RBrace) && !Check(Tok::End)) {
            Node* s = Statement();
            if (!s)
                return nullptr;
            b->list.PushBack(s);
        }
        if (!Expect(Tok::RBrace, "'}'"))
            return nullptr;
        return b;
    }
    Node* Statement()
    {
        while (Match(Tok::Semicolon)) {
        }
        const Token t = cur;
        switch (cur.type) {
        case Tok::LBrace: return Block();
        case Tok::KwVar:
        case Tok::KwLet: {
            Advance();
            if (!Check(Tok::Ident)) {
                Fail("Expected a variable name", cur);
                return nullptr;
            }
            Node* n = Make(NodeKind::VarDecl, t);
            n->text = cur.text;
            Advance();
            if (Match(Tok::Colon) && !Match(Tok::Ident)) { // optional type annotation (documentation only)
                Fail("Expected a type name", cur);
                return nullptr;
            }
            if (Match(Tok::Assign)) {
                n->a = Expression();
                if (!n->a)
                    return nullptr;
            }
            return n;
        }
        case Tok::KwIf: {
            Advance();
            Node* n = Make(NodeKind::If, t);
            n->a = Expression();
            if (!n->a || !(n->b = Block()))
                return nullptr;
            if (Check(Tok::KwElif)) {
                n->c = Statement(); // parsed as a nested if
                if (!n->c)
                    return nullptr;
            } else if (Match(Tok::KwElse)) {
                n->c = Check(Tok::KwIf) ? Statement() : Block();
                if (!n->c)
                    return nullptr;
            }
            return n;
        }
        case Tok::KwElif: {
            // "elif" = else-if (handled like "if" for the nested branch).
            Advance();
            Node* n = Make(NodeKind::If, t);
            n->a = Expression();
            if (!n->a || !(n->b = Block()))
                return nullptr;
            if (Check(Tok::KwElif)) {
                n->c = Statement();
            } else if (Match(Tok::KwElse)) {
                n->c = Check(Tok::KwIf) ? Statement() : Block();
            }
            return (Check(Tok::End) && !error.Empty()) ? nullptr : n;
        }
        case Tok::KwWhile: {
            Advance();
            Node* n = Make(NodeKind::While, t);
            n->a = Expression();
            if (!n->a || !(n->c = Block()))
                return nullptr;
            return n;
        }
        case Tok::KwFor: {
            Advance();
            if (!Check(Tok::Ident)) {
                Fail("Expected a loop variable", cur);
                return nullptr;
            }
            const String var(cur.text);
            Advance();
            if (!Expect(Tok::KwIn, "'in'"))
                return nullptr;
            Node* from = Expression();
            if (!from)
                return nullptr;
            Node* n;
            if (Match(Tok::DotDot)) {
                n = Make(NodeKind::ForRange, t);
                n->a = from;
                n->b = Expression();
                if (!n->b)
                    return nullptr;
            } else {
                n = Make(NodeKind::ForEach, t);
                n->a = from;
            }
            n->text = var;
            n->c = Block();
            return n->c ? n : nullptr;
        }
        case Tok::KwBreak: Advance(); return Make(NodeKind::Break, t);
        case Tok::KwContinue: Advance(); return Make(NodeKind::Continue, t);
        case Tok::KwReturn: {
            Advance();
            Node* n = Make(NodeKind::Return, t);
            if (!Check(Tok::RBrace) && !Check(Tok::Semicolon) && !cur.newlineBefore) {
                n->a = Expression();
                if (!n->a)
                    return nullptr;
            }
            return n;
        }
        default: break;
        }
        Node* e = Expression();
        if (!e)
            return nullptr;
        if (Check(Tok::Assign) || Check(Tok::PlusAssign) || Check(Tok::MinusAssign) || Check(Tok::StarAssign) || Check(Tok::SlashAssign)) {
            const Token opTok = cur;
            Advance();
            if (e->kind != NodeKind::Ident && e->kind != NodeKind::Member && e->kind != NodeKind::Index) {
                Fail("Can't assign to this expression", opTok);
                return nullptr;
            }
            Node* n = Make(NodeKind::Assign, opTok);
            n->op = opTok.type;
            n->a = e;
            n->b = Expression();
            return n->b ? n : nullptr;
        }
        Node* s = Make(NodeKind::ExprStmt, t);
        s->a = e;
        return s;
    }

    bool Script()
    {
        Advance();
        if (!Expect(Tok::KwScript, "'script' (a file starts with: script Name { ... })"))
            return false;
        if (!Check(Tok::Ident))
            return Fail("Expected the script name", cur);
        scriptName = cur.text;
        Advance();
        if (!Expect(Tok::LBrace, "'{'"))
            return false;
        while (!Check(Tok::RBrace) && !Check(Tok::End)) {
            while (Match(Tok::Semicolon)) {
            }
            // Annotations for the next property.
            FieldDecl field;
            while (Check(Tok::At)) {
                Advance();
                if (!Check(Tok::Ident))
                    return Fail("Expected an annotation name", cur);
                const String name(cur.text);
                const Token at = cur;
                Advance();
                Array<f64> nums;
                String str;
                if (Match(Tok::LParen)) {
                    while (!Check(Tok::RParen) && !Check(Tok::End)) {
                        const bool neg = Match(Tok::Minus);
                        if (Check(Tok::Number))
                            nums.PushBack(neg ? -cur.number : cur.number);
                        else if (Check(Tok::String))
                            str = cur.string;
                        else
                            return Fail("Annotation arguments must be numbers or strings", cur);
                        Advance();
                        if (!Match(Tok::Comma))
                            break;
                    }
                    if (!Expect(Tok::RParen, "')'"))
                        return false;
                }
                if (name == "range" && nums.Size() == 2) {
                    field.rangeMin = nums[0];
                    field.rangeMax = nums[1];
                } else if (name == "tooltip") {
                    field.tooltip = str;
                } else if (name == "color") {
                    field.color = true;
                } else {
                    return Fail(String::Format("Unknown annotation '@{}' (use @range(min, max), @tooltip(\"text\"), @color)", name), at);
                }
            }
            if (Check(Tok::KwProp) || Check(Tok::KwVar) || Check(Tok::KwLet)) {
                field.isProp = Check(Tok::KwProp);
                field.line = cur.line;
                Advance();
                if (!Check(Tok::Ident))
                    return Fail("Expected a field name", cur);
                field.name = cur.text;
                for (const FieldDecl& f : fields)
                    if (f.name == field.name)
                        return Fail(String::Format("'{}' is already declared", field.name), cur);
                Advance();
                if (Match(Tok::Colon) && !Match(Tok::Ident))
                    return Fail("Expected a type name", cur);
                if (Match(Tok::Assign)) {
                    field.init = Expression();
                    if (!field.init)
                        return false;
                } else if (field.isProp) {
                    return Fail("A prop needs a default value (prop speed = 1.0)", cur);
                }
                fields.PushBack(move(field));
            } else if (Check(Tok::KwFn)) {
                FnDecl fn;
                fn.line = cur.line;
                Advance();
                if (!Check(Tok::Ident))
                    return Fail("Expected a function name", cur);
                fn.name = cur.text;
                for (const FnDecl& f : fns)
                    if (f.name == fn.name)
                        return Fail(String::Format("Function '{}' is already declared", fn.name), cur);
                Advance();
                if (!Expect(Tok::LParen, "'('"))
                    return false;
                if (!Check(Tok::RParen)) {
                    do {
                        if (!Check(Tok::Ident))
                            return Fail("Expected a parameter name", cur);
                        fn.params.PushBack(String(cur.text));
                        Advance();
                        if (Match(Tok::Colon) && !Match(Tok::Ident))
                            return Fail("Expected a type name", cur);
                    } while (Match(Tok::Comma));
                }
                if (!Expect(Tok::RParen, "')'"))
                    return false;
                fn.body = Block();
                if (!fn.body)
                    return false;
                fns.PushBack(move(fn));
            } else {
                return Fail("Expected 'prop', 'var' or 'fn' inside the script", cur);
            }
            if (!error.Empty())
                return false;
        }
        if (!Expect(Tok::RBrace, "'}' at the end of the script"))
            return false;
        if (!Check(Tok::End))
            return Fail("Only one script per file", cur);
        return error.Empty();
    }
};

// --- Code generation ---------------------------------------------------------------------------------------------------------

struct Codegen
{
    Program& prog;
    String error;
    u32 errorLine = 0, errorCol = 0;

    struct Local
    {
        String name;
        u32 depth;
        u32 slot;
    };
    struct Loop
    {
        Array<usize> breaks;
        Array<usize> continues;
    };
    FunctionCode* fn = nullptr;
    Array<Local> locals;
    u32 depth = 0;
    u32 nextSlot = 0;
    Array<Loop> loops;
    u32 line = 0;

    bool Fail(StringView msg, const Node* at)
    {
        if (error.Empty()) {
            error = msg;
            errorLine = at ? at->line : 0;
            errorCol = at ? at->column : 0;
        }
        return false;
    }

    void Emit(u8 b)
    {
        fn->code.PushBack(b);
        fn->lines.PushBack(line);
    }
    void EmitOp(Op op) { Emit(u8(op)); }
    void Emit16(u16 v)
    {
        Emit(u8(v & 0xFF));
        Emit(u8(v >> 8));
    }
    usize EmitJump(Op op)
    {
        EmitOp(op);
        const usize at = fn->code.Size();
        for (u32 i = 0; i < 4; ++i)
            Emit(0);
        return at;
    }
    void PatchJump(usize at) { PatchJumpTo(at, fn->code.Size()); }
    void PatchJumpTo(usize at, usize target)
    {
        const i32 rel = i32(i64(target) - i64(at + 4));
        MemCopy(fn->code.Data() + at, &rel, 4);
    }
    void EmitLoop(usize target)
    {
        const usize at = EmitJump(Op::Jump);
        PatchJumpTo(at, target);
    }

    u16 Constant(const Value& v)
    {
        for (usize i = 0; i < prog.constants.Size(); ++i)
            if (prog.constants[i].Type() == v.Type() && prog.constants[i] == v)
                return u16(i);
        prog.constants.PushBack(v);
        return u16(prog.constants.Size() - 1);
    }
    u16 Name(StringView s) { return Constant(Value(s)); }

    i32 FindLocal(StringView name) const
    {
        for (usize i = locals.Size(); i > 0; --i)
            if (locals[i - 1].name == name)
                return i32(locals[i - 1].slot);
        return -1;
    }
    i32 FindField(StringView name) const
    {
        for (usize i = 0; i < prog.fields.Size(); ++i)
            if (prog.fields[i] == name)
                return i32(i);
        return -1;
    }
    u32 NewSlot()
    {
        const u32 s = nextSlot++;
        fn->locals = Max(fn->locals, nextSlot);
        return s;
    }
    u32 Declare(StringView name)
    {
        const u32 slot = NewSlot();
        locals.PushBack(Local{String(name), depth, slot});
        return slot;
    }
    void BeginScope() { ++depth; }
    void EndScope()
    {
        while (!locals.Empty() && locals.Back().depth == depth)
            locals.PopBack();
        --depth;
    }

    // --- Expressions ---
    bool Expr(const Node* n)
    {
        line = n->line;
        switch (n->kind) {
        case NodeKind::Number:
            EmitOp(Op::Const);
            Emit16(Constant(Value(n->number)));
            return true;
        case NodeKind::String:
            EmitOp(Op::Const);
            Emit16(Constant(Value(n->text.View())));
            return true;
        case NodeKind::Bool: EmitOp(n->boolean ? Op::True : Op::False); return true;
        case NodeKind::Nil: EmitOp(Op::Nil); return true;
        case NodeKind::Self: return Fail("'self' can only be used as self.field or self.method()", n);
        case NodeKind::Ident: {
            if (const i32 l = FindLocal(n->text); l >= 0) {
                EmitOp(Op::LoadLocal);
                Emit16(u16(l));
            } else if (const i32 f = FindField(n->text); f >= 0) {
                EmitOp(Op::LoadField);
                Emit16(u16(f));
            } else if (const i32 fi = prog.FindFunction(n->text); fi >= 0) {
                EmitOp(Op::Const);
                Emit16(Constant(Value::Function(&prog, u32(fi))));
            } else {
                EmitOp(Op::LoadGlobal);
                Emit16(Name(n->text));
            }
            return true;
        }
        case NodeKind::List:
            for (const Node* item : n->list)
                if (!Expr(item))
                    return false;
            EmitOp(Op::MakeList);
            Emit16(u16(n->list.Size()));
            return true;
        case NodeKind::Unary:
            if (!Expr(n->a))
                return false;
            EmitOp(n->op == Tok::Minus ? Op::Neg : Op::Not);
            return true;
        case NodeKind::Binary: {
            if (!Expr(n->a) || !Expr(n->b))
                return false;
            line = n->line;
            switch (n->op) {
            case Tok::Plus: EmitOp(Op::Add); break;
            case Tok::Minus: EmitOp(Op::Sub); break;
            case Tok::Star: EmitOp(Op::Mul); break;
            case Tok::Slash: EmitOp(Op::Div); break;
            case Tok::Percent: EmitOp(Op::Mod); break;
            case Tok::Eq: EmitOp(Op::Eq); break;
            case Tok::Ne: EmitOp(Op::Ne); break;
            case Tok::Lt: EmitOp(Op::Lt); break;
            case Tok::Le: EmitOp(Op::Le); break;
            case Tok::Gt: EmitOp(Op::Gt); break;
            case Tok::Ge: EmitOp(Op::Ge); break;
            default: return Fail("Unknown operator", n);
            }
            return true;
        }
        case NodeKind::And:
        case NodeKind::Or: {
            if (!Expr(n->a))
                return false;
            const usize j = EmitJump(n->kind == NodeKind::And ? Op::AndJump : Op::OrJump);
            if (!Expr(n->b))
                return false;
            PatchJump(j);
            return true;
        }
        case NodeKind::Member:
            if (n->a->kind == NodeKind::Self) {
                const i32 f = FindField(n->text);
                if (f < 0)
                    return Fail(String::Format("'{}' has no field '{}'", prog.name, n->text), n);
                EmitOp(Op::LoadField);
                Emit16(u16(f));
                return true;
            }
            if (!Expr(n->a))
                return false;
            line = n->line;
            EmitOp(Op::GetMember);
            Emit16(Name(n->text));
            return true;
        case NodeKind::Index:
            if (!Expr(n->a) || !Expr(n->b))
                return false;
            EmitOp(Op::GetIndex);
            return true;
        case NodeKind::Call: {
            const Node* callee = n->a;
            if (n->list.Size() > 255)
                return Fail("Too many arguments", n);
            // Script functions: direct call with the same self.
            const bool selfCall = callee->kind == NodeKind::Member && callee->a->kind == NodeKind::Self;
            if ((callee->kind == NodeKind::Ident && FindLocal(callee->text) < 0 && FindField(callee->text) < 0) || selfCall) {
                const i32 fi = prog.FindFunction(callee->text);
                if (fi >= 0) {
                    if (prog.functions[usize(fi)].params != n->list.Size())
                        return Fail(String::Format("'{}' takes {} argument(s), got {}", callee->text, prog.functions[usize(fi)].params,
                                                   n->list.Size()),
                                    n);
                    for (const Node* arg : n->list)
                        if (!Expr(arg))
                            return false;
                    line = n->line;
                    EmitOp(Op::CallFn);
                    Emit16(u16(fi));
                    Emit(u8(n->list.Size()));
                    return true;
                }
                if (selfCall)
                    return Fail(String::Format("'{}' has no function '{}'", prog.name, callee->text), n);
            }
            if (callee->kind == NodeKind::Member) {
                if (!Expr(callee->a))
                    return false;
                for (const Node* arg : n->list)
                    if (!Expr(arg))
                        return false;
                line = n->line;
                EmitOp(Op::CallMethod);
                Emit16(Name(callee->text));
                Emit(u8(n->list.Size()));
                return true;
            }
            if (!Expr(callee))
                return false;
            for (const Node* arg : n->list)
                if (!Expr(arg))
                    return false;
            line = n->line;
            EmitOp(Op::Call);
            Emit(u8(n->list.Size()));
            return true;
        }
        default: return Fail("Expected an expression", n);
        }
    }

    /// Stores the value on top of the stack into `target`, writing modified value types back up the chain
    /// (transform.position.y = 1 sets transform.position).
    bool Store(const Node* target)
    {
        switch (target->kind) {
        case NodeKind::Ident: {
            if (const i32 l = FindLocal(target->text); l >= 0) {
                EmitOp(Op::StoreLocal);
                Emit16(u16(l));
            } else if (const i32 f = FindField(target->text); f >= 0) {
                EmitOp(Op::StoreField);
                Emit16(u16(f));
            } else if (prog.FindFunction(target->text) >= 0) {
                return Fail(String::Format("Can't assign to function '{}'", target->text), target);
            } else {
                // Globals (engine objects) are references: nothing to write back.
                EmitOp(Op::Pop);
            }
            return true;
        }
        case NodeKind::Member: {
            if (target->a->kind == NodeKind::Self) {
                const i32 f = FindField(target->text);
                if (f < 0)
                    return Fail(String::Format("'{}' has no field '{}'", prog.name, target->text), target);
                EmitOp(Op::StoreField);
                Emit16(u16(f));
                return true;
            }
            const u32 tmp = NewSlot();
            EmitOp(Op::StoreLocal);
            Emit16(u16(tmp));
            if (!Expr(target->a))
                return false;
            EmitOp(Op::LoadLocal);
            Emit16(u16(tmp));
            EmitOp(Op::SetMember);
            Emit16(Name(target->text));
            return WriteBack(target->a);
        }
        case NodeKind::Index: {
            const u32 tmp = NewSlot();
            EmitOp(Op::StoreLocal);
            Emit16(u16(tmp));
            if (!Expr(target->a) || !Expr(target->b))
                return false;
            EmitOp(Op::LoadLocal);
            Emit16(u16(tmp));
            EmitOp(Op::SetIndex);
            return WriteBack(target->a);
        }
        default: EmitOp(Op::Pop); return true;
        }
    }
    /// After SetMember/SetIndex the (possibly modified) container is on the stack: store it where it came from.
    bool WriteBack(const Node* container)
    {
        if (container->kind == NodeKind::Ident || container->kind == NodeKind::Member || container->kind == NodeKind::Index)
            return Store(container);
        EmitOp(Op::Pop); // a temporary (call result): nothing to update
        return true;
    }

    bool Assign(const Node* n)
    {
        const Node* t = n->a;
        if (n->op == Tok::Assign) {
            if (!Expr(n->b))
                return false;
            line = n->line;
            return Store(t);
        }
        // Compound: target = target op value (the target is evaluated twice; fine for side-effect-free paths).
        if (!Expr(t) || !Expr(n->b))
            return false;
        line = n->line;
        EmitOp(n->op == Tok::PlusAssign ? Op::Add : n->op == Tok::MinusAssign ? Op::Sub : n->op == Tok::StarAssign ? Op::Mul : Op::Div);
        return Store(t);
    }

    // --- Statements ---
    bool Stmt(const Node* n)
    {
        line = n->line;
        switch (n->kind) {
        case NodeKind::ExprStmt:
            if (!Expr(n->a))
                return false;
            EmitOp(Op::Pop);
            return true;
        case NodeKind::VarDecl: {
            if (n->a) {
                if (!Expr(n->a))
                    return false;
            } else {
                EmitOp(Op::Nil);
            }
            for (usize i = locals.Size(); i > 0 && locals[i - 1].depth == depth; --i)
                if (locals[i - 1].name == n->text)
                    return Fail(String::Format("'{}' is already declared in this block", n->text), n);
            const u32 slot = Declare(n->text);
            EmitOp(Op::StoreLocal);
            Emit16(u16(slot));
            return true;
        }
        case NodeKind::Assign: return Assign(n);
        case NodeKind::Block: {
            BeginScope();
            for (const Node* s : n->list)
                if (!Stmt(s))
                    return false;
            EndScope();
            return true;
        }
        case NodeKind::If: {
            if (!Expr(n->a))
                return false;
            const usize skipThen = EmitJump(Op::JumpIfFalse);
            if (!Stmt(n->b))
                return false;
            if (n->c) {
                const usize skipElse = EmitJump(Op::Jump);
                PatchJump(skipThen);
                if (!Stmt(n->c))
                    return false;
                PatchJump(skipElse);
            } else {
                PatchJump(skipThen);
            }
            return true;
        }
        case NodeKind::While: {
            const usize top = fn->code.Size();
            if (!Expr(n->a))
                return false;
            const usize exit = EmitJump(Op::JumpIfFalse);
            loops.PushBack(Loop{});
            if (!Stmt(n->c))
                return false;
            for (usize c : loops.Back().continues)
                PatchJumpTo(c, top);
            EmitLoop(top);
            PatchJump(exit);
            for (usize b : loops.Back().breaks)
                PatchJump(b);
            loops.PopBack();
            return true;
        }
        case NodeKind::ForRange:
        case NodeKind::ForEach: {
            BeginScope();
            const bool range = n->kind == NodeKind::ForRange;
            // range:  i = a; end = b; while i < end { body; i += 1 }
            // list:   list = x; idx = 0; while idx < list.len() { item = list[idx]; body; idx += 1 }
            const u32 hidden = NewSlot(); // end value or the list
            const u32 counter = range ? Declare(n->text) : NewSlot();
            if (range) {
                if (!Expr(n->a))
                    return false;
                EmitOp(Op::StoreLocal);
                Emit16(u16(counter));
                if (!Expr(n->b))
                    return false;
            } else {
                if (!Expr(n->a))
                    return false;
                EmitOp(Op::Const);
                Emit16(Constant(Value(0.0)));
                EmitOp(Op::StoreLocal);
                Emit16(u16(counter));
            }
            EmitOp(Op::StoreLocal);
            Emit16(u16(hidden));
            const u32 item = range ? 0 : Declare(n->text);
            const usize top = fn->code.Size();
            EmitOp(Op::LoadLocal);
            Emit16(u16(counter));
            EmitOp(Op::LoadLocal);
            Emit16(u16(hidden));
            if (!range) {
                EmitOp(Op::CallMethod);
                Emit16(Name("len"));
                Emit(0);
            }
            EmitOp(Op::Lt);
            const usize exit = EmitJump(Op::JumpIfFalse);
            if (!range) {
                EmitOp(Op::LoadLocal);
                Emit16(u16(hidden));
                EmitOp(Op::LoadLocal);
                Emit16(u16(counter));
                EmitOp(Op::GetIndex);
                EmitOp(Op::StoreLocal);
                Emit16(u16(item));
            }
            loops.PushBack(Loop{});
            if (!Stmt(n->c))
                return false;
            const usize step = fn->code.Size();
            for (usize c : loops.Back().continues)
                PatchJumpTo(c, step);
            EmitOp(Op::LoadLocal);
            Emit16(u16(counter));
            EmitOp(Op::Const);
            Emit16(Constant(Value(1.0)));
            EmitOp(Op::Add);
            EmitOp(Op::StoreLocal);
            Emit16(u16(counter));
            EmitLoop(top);
            PatchJump(exit);
            for (usize b : loops.Back().breaks)
                PatchJump(b);
            loops.PopBack();
            EndScope();
            return true;
        }
        case NodeKind::Break:
        case NodeKind::Continue:
            if (loops.Empty())
                return Fail(String::Format("'{}' outside of a loop", n->kind == NodeKind::Break ? "break" : "continue"), n);
            (n->kind == NodeKind::Break ? loops.Back().breaks : loops.Back().continues).PushBack(EmitJump(Op::Jump));
            return true;
        case NodeKind::Return:
            if (n->a) {
                if (!Expr(n->a))
                    return false;
                EmitOp(Op::Return);
            } else {
                EmitOp(Op::ReturnNil);
            }
            return true;
        default: return Fail("Expected a statement", n);
        }
    }

    void BeginFunction(u32 index)
    {
        fn = &prog.functions[index];
        locals.Clear();
        depth = 0;
        nextSlot = 0;
        loops.Clear();
    }
};

/// Constant value of a property default (literals, negation, vec3(...) of literals).
bool ConstantValue(const Node* n, Value& out)
{
    switch (n->kind) {
    case NodeKind::Number: out = Value(n->number); return true;
    case NodeKind::String: out = Value(n->text.View()); return true;
    case NodeKind::Bool: out = Value(n->boolean); return true;
    case NodeKind::Unary:
        if (n->op == Tok::Minus && n->a->kind == NodeKind::Number) {
            out = Value(-n->a->number);
            return true;
        }
        return false;
    case NodeKind::Call:
        if (n->a->kind == NodeKind::Ident && n->a->text == "vec3") {
            f32 c[3] = {0, 0, 0};
            for (usize i = 0; i < n->list.Size() && i < 3; ++i) {
                Value v;
                if (!ConstantValue(n->list[i], v) || !v.IsNumber())
                    return false;
                c[i] = v.AsFloat();
            }
            if (n->list.Size() == 1)
                c[1] = c[2] = c[0];
            out = Value(Vec3(c[0], c[1], c[2]));
            return true;
        }
        return false;
    default: return false;
    }
}

} // namespace

i32 Program::FindFunction(StringView functionName) const
{
    const u32* i = functionIndex.Find(String(functionName));
    return i ? i32(*i) : -1;
}

Result<Program*> Compile(StringView source, StringView fileName, CompileError* errorOut)
{
    Parser p;
    p.lex.src = source;
    auto fail = [&](StringView msg, u32 line, u32 col) -> Result<Program*> {
        if (errorOut) {
            errorOut->message = msg;
            errorOut->line = line;
            errorOut->column = col;
        }
        return Error::Make(ErrorCode::InvalidData, "{}({}:{}): {}", fileName, line, col, msg);
    };
    if (!p.Script())
        return fail(p.error.Empty() ? String("Syntax error") : p.error, p.errorLine, p.errorLine ? p.errorCol : 0);

    Program* prog = New<Program>();
    prog->name = p.scriptName;
    prog->source = fileName;
    // Function table: [0] = field initializers, then declared functions (indices known before code generation).
    prog->functions.AddDefaulted().name = "<fields>";
    for (const FnDecl& f : p.fns) {
        prog->functionIndex.InsertOrAssign(f.name, u32(prog->functions.Size()));
        FunctionCode& fc = prog->functions.AddDefaulted();
        fc.name = f.name;
        fc.params = u32(f.params.Size());
    }
    for (const FieldDecl& f : p.fields)
        prog->fields.PushBack(f.name);

    Codegen g{*prog};
    // Field initializers (in order; later ones may use earlier ones).
    g.BeginFunction(0);
    for (usize i = 0; i < p.fields.Size(); ++i) {
        const FieldDecl& f = p.fields[i];
        g.line = f.line;
        // A prop the host already set (per-entity value) keeps it: if field == nil { field = default }.
        usize skipPreset = 0;
        if (f.isProp) {
            g.EmitOp(Op::LoadField);
            g.Emit16(u16(i));
            g.EmitOp(Op::Nil);
            g.EmitOp(Op::Eq);
            skipPreset = g.EmitJump(Op::JumpIfFalse);
        }
        if (f.init) {
            if (!g.Expr(f.init))
                break;
        } else {
            g.EmitOp(Op::Nil);
        }
        g.EmitOp(Op::StoreField);
        g.Emit16(u16(i));
        if (f.isProp)
            g.PatchJump(skipPreset);
        if (f.isProp) {
            Property prop;
            prop.name = f.name;
            prop.field = u32(i);
            prop.rangeMin = f.rangeMin;
            prop.rangeMax = f.rangeMax;
            prop.tooltip = f.tooltip;
            if (!ConstantValue(f.init, prop.defaultValue)) {
                g.Fail(String::Format("The default of prop '{}' must be a number, bool, string or vec3(...) literal", f.name), f.init);
                break;
            }
            switch (prop.defaultValue.Type()) {
            case ValueType::Number: prop.kind = PropertyKind::Number; break;
            case ValueType::Bool: prop.kind = PropertyKind::Bool; break;
            case ValueType::String: prop.kind = PropertyKind::String; break;
            default: prop.kind = f.color ? PropertyKind::Color : PropertyKind::Vec3; break;
            }
            prog->properties.PushBack(move(prop));
        }
    }
    g.EmitOp(Op::ReturnNil);
    for (usize i = 0; i < p.fns.Size() && g.error.Empty(); ++i) {
        const FnDecl& f = p.fns[i];
        g.BeginFunction(u32(i + 1));
        g.line = f.line;
        for (const String& param : f.params)
            g.Declare(param);
        if (!g.Stmt(f.body))
            break;
        g.EmitOp(Op::ReturnNil);
    }
    if (!g.error.Empty()) {
        DestroyProgram(prog);
        return fail(g.error, g.errorLine, g.errorCol);
    }
    return prog;
}

void DestroyProgram(Program* program)
{
    if (program)
        Delete(program);
}

} // namespace zx::script
