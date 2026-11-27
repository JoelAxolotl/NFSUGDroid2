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

// Covers the executable image; must run before any guest code. The native code
// is only enabled when `exe_path` is the exact executable it was recompiled from
// (see set_source_identity); otherwise the game runs on the interpreter.
void init(uint32_t code_start, uint32_t code_end, const char* exe_path);

// Recorded by the generated manifest (generated/aot_manifest.cpp).
void set_source_identity(uint32_t size, uint64_t fnv1a64);

// Result of the identity check, for the UI.
bool native_enabled();

// Registers a generated table (called from generated code at load time).
void register_table(const Entry* entries, uint32_t count);

// Routes execution reaching `guest` to a host replacement (runtime/game_patches.cpp).
void register_patch(uint32_t guest, Fn fn);

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

// Interpreter hot spots (16-byte buckets over the code range), to find code
// that still runs outside the recompiled functions.
namespace detail {
extern uint32_t* g_hot;
extern uint32_t* g_entry;
}
inline void note_interpreted(uint32_t eip) {
    uint32_t off = eip - detail::g_start;
    if (off < detail::g_size) ++detail::g_hot[off >> 4];
}
// Where an interpreted run begins (the address control was handed to the interpreter).
inline void note_interp_entry(uint32_t eip) {
    uint32_t off = eip - detail::g_start;
    if (off < detail::g_size) ++detail::g_entry[off >> 4];
}
// Logs the top buckets since the last call and resets the counters.
void log_hot_spots(int top);

uint32_t translated_count();

}  // namespace aot
