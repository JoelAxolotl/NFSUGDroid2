#pragma once
// Ahead-of-time translated code registry.
//
// tools/recomp translates guest functions of SPEED2.EXE to C++ that operates on
// cpu::Cpu (generated/*.cpp, linked into this library). A translated function
// runs until the guest function returns (EIP = return address) or until it
// reaches control flow it cannot follow (it then leaves EIP there and returns),
// so interpreted and translated code hand off at instruction boundaries.
#include <cstdint>

#include "cpu/cpu.h"

namespace aot {

using Fn = void (*)(cpu::Cpu& c);

struct Entry {
    uint32_t guest;
    Fn fn;
};

// Covers the executable image; must run before any guest code.
void init(uint32_t code_start, uint32_t code_end);

// Registers a generated table (called from generated code at load time).
void register_table(const Entry* entries, uint32_t count);

namespace detail {
extern Fn* g_table;
extern uint8_t* g_seen;
extern uint32_t g_start, g_size;
}  // namespace detail

// Translated function for this guest address, or null.
inline Fn lookup(uint32_t eip) {
    uint32_t off = eip - detail::g_start;
    return off < detail::g_size ? detail::g_table[off] : nullptr;
}

// Profiling: function entries executed by the interpreter (call targets).
inline void note_call(uint32_t target) {
    uint32_t off = target - detail::g_start;
    if (off < detail::g_size) detail::g_seen[off] = 1;
}

// Writes the profile (one hex address per line), merged with the existing file.
void dump_profile(const char* path);

uint32_t translated_count();

}  // namespace aot
