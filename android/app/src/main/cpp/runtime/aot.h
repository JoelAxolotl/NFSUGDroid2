#pragma once
// Ahead-of-time translated code registry.
//
// tools/recomp translates guest functions of SPEED2.EXE to C++ that operates on
// cpu::Cpu and links them into this library. Each translated function runs
// until the guest function returns (EIP = return address) or until it reaches
// code it could not translate (it then sets EIP there and returns), so the
// interpreter and translated code can hand off at any instruction boundary.
#include <cstdint>

#include "cpu/cpu.h"

namespace aot {

using Fn = void (*)(cpu::Cpu& c);

struct Entry {
    uint32_t guest;
    Fn fn;
};

// Registers a generated table (called by the generated translation unit).
void register_table(const Entry* entries, uint32_t count);

// Translated function for this guest address, or null.
Fn lookup(uint32_t guest_eip);

// Diagnostics: counts interpreter entries at addresses without translations
// so the recompiler can be seeded with them (written to <user>/aot_misses.txt).
void note_miss(uint32_t guest_eip);
void dump_misses(const char* path);

bool enabled();

}  // namespace aot
