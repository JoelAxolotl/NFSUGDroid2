#pragma once
// Allocator for guest heap memory (HeapAlloc / GlobalAlloc / LocalAlloc).
// Bookkeeping lives on the host so guest heap corruption cannot break it.
#include <cstdint>

namespace gheap {

uint32_t alloc(uint32_t size, bool zero);
bool free(uint32_t addr);
uint32_t realloc(uint32_t addr, uint32_t size, bool zero_tail, bool in_place_only);
uint32_t size(uint32_t addr);  // 0xFFFFFFFF if not a live block

}  // namespace gheap
