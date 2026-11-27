#pragma once
// Host replacements for specific SPEED2.EXE code (see game_patches.def): 16:9 support.
#include <cstdint>

#include "cpu/cpu.h"

namespace game {

// Each patch runs with the guest registers in `c` and leaves EIP at the resume address
// (or at the caller's return address for whole-function patches).
#define PATCH(addr, resume, fn, bytes) void fn(cpu::Cpu& c);
#include "runtime/game_patches.def"
#undef PATCH

// Registers the patches for the interpreter after checking the original bytes in guest
// memory. Call after the executable is mapped. Returns the number of patches applied.
int install_patches();

}  // namespace game
