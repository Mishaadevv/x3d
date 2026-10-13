#pragma once

// ZeqouScript bytecode. Operands follow the opcode: u16 (constant / slot / member name), u8 (argument count),
// i32 (relative jump from the end of the instruction).

#include "Core/Types.h"

namespace zx::script {

enum class Op : u8
{
    Const,       // u16 constant
    Nil,
    True,
    False,
    Pop,
    Dup,
    LoadLocal,   // u16 slot
    StoreLocal,  // u16 slot (pops)
    LoadField,   // u16 field
    StoreField,  // u16 field (pops)
    LoadGlobal,  // u16 name constant
    GetMember,   // u16 name constant: [obj] -> [value]
    SetMember,   // u16 name constant: [obj, value] -> [obj'] (value types are modified copies)
    GetIndex,    // [obj, index] -> [value]
    SetIndex,    // [obj, index, value] -> [obj']
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Neg,
    Not,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Jump,        // i32
    JumpIfFalse, // i32 (pops)
    AndJump,     // i32: if top is false jump (keep), else pop
    OrJump,      // i32: if top is true jump (keep), else pop
    Call,        // u8 argc: [callee, args...] -> [result]
    CallFn,      // u16 function, u8 argc: [args...] -> [result] (same program, same self)
    CallMethod,  // u16 name constant, u8 argc: [obj, args...] -> [result]
    MakeList,    // u16 count
    Return,      // [value]
    ReturnNil,
};

} // namespace zx::script
