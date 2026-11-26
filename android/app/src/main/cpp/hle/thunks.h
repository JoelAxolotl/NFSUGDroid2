#pragma once
// Guest-visible entry points for host (HLE) functions.
//
// Every imported Win32 function and every COM method the runtime implements
// gets a 4-byte slot in [kThunkBase, kThunkBase + kThunkSpan). Guest code
// reaches a slot through the IAT or a COM vtable; the CPU loop sees EIP in the
// range and calls hle::dispatch instead of decoding bytes.
#include <cstdint>
#include <initializer_list>

#include "cpu/cpu.h"

namespace hle {

constexpr uint32_t kThunkBase = 0x7F000000;
constexpr uint32_t kThunkSpan = 0x00010000;
constexpr uint32_t kReturnToHostThunk = kThunkBase;  // slot 0

using HleFn = void (*)(cpu::Cpu& c);

// Registers a host function and returns its guest address. `arg_bytes` is
// popped after the call (stdcall); pass 0 for cdecl/varargs functions.
uint32_t make_thunk(const char* name, HleFn fn, uint16_t arg_bytes);

// Called by the CPU loop when EIP is inside the thunk range.
void dispatch(cpu::Cpu& c);

// Calls a guest function (stdcall or cdecl) from host code on the current
// guest thread and returns EAX. Nested calls are supported.
uint32_t call_guest(cpu::Cpu& c, uint32_t fn, std::initializer_list<uint32_t> args);

// Argument access inside an HLE function: arg(c, 0) is the first argument.
inline uint32_t arg(const cpu::Cpu& c, int i) { return mem::rd32(c.r[cpu::ESP] + 4 + 4 * i); }

const char* thunk_name(uint32_t addr);

}  // namespace hle
