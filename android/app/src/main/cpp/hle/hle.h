#pragma once
// Declaration helpers for HLE (host-implemented) Win32 exports.
//
//   HLE_EXPORT(kernel32, GetTickCount, 0) { ret(c, now_ms()); }
//
// declares a host function and registers it as `kernel32.dll!GetTickCount`
// with 0 bytes of stdcall arguments. The loader binds IAT entries to these by
// name (case-insensitive DLL, case-sensitive symbol), or by "#ordinal".
#include <cstdint>
#include <string>

#include "core/log.h"
#include "core/memory.h"
#include "cpu/cpu.h"
#include "hle/thunks.h"

namespace hle {

struct Registrar {
    Registrar(const char* dll, const char* name, HleFn fn, uint16_t arg_bytes);
};

// Maps an ordinal import ("#23") to a named export of the same DLL.
struct OrdinalAlias {
    OrdinalAlias(const char* dll, uint32_t ordinal, const char* name);
};

// Guest address of an export's thunk, creating a "missing import" trap thunk
// (logs the name and faults) when the export is not implemented.
uint32_t resolve_import(const std::string& dll, const std::string& name);
// Same, but returns 0 for unknown exports (GetProcAddress semantics).
uint32_t find_export(const std::string& dll, const std::string& name);

// Fake module handles for system DLLs.
uint32_t module_handle(const std::string& dll);
bool is_known_dll(const std::string& dll);
std::string module_name(uint32_t handle);

void init();
void set_trace(bool on);
bool trace_enabled();

inline void ret(cpu::Cpu& c, uint32_t v) { c.r[cpu::EAX] = v; }

// Win32 BOOL / error helpers. LastError lives in the guest TEB.
void set_last_error(uint32_t err);
uint32_t get_last_error();

}  // namespace hle

#define HLE_EXPORT(dll, name, argbytes)                                                        \
    static void hle_##dll##_##name(cpu::Cpu& c);                                               \
    static ::hle::Registrar hle_reg_##dll##_##name(#dll ".dll", #name, hle_##dll##_##name,     \
                                                   (uint16_t)(argbytes));                      \
    static void hle_##dll##_##name([[maybe_unused]] cpu::Cpu& c)

// Win32 constants used across modules.
namespace win {
constexpr uint32_t TRUE_ = 1, FALSE_ = 0;
constexpr uint32_t INVALID_HANDLE = 0xFFFFFFFFu;
constexpr uint32_t ERROR_SUCCESS = 0;
constexpr uint32_t ERROR_FILE_NOT_FOUND = 2;
constexpr uint32_t ERROR_PATH_NOT_FOUND = 3;
constexpr uint32_t ERROR_ACCESS_DENIED = 5;
constexpr uint32_t ERROR_INVALID_HANDLE = 6;
constexpr uint32_t ERROR_NOT_ENOUGH_MEMORY = 8;
constexpr uint32_t ERROR_NO_MORE_FILES = 18;
constexpr uint32_t ERROR_HANDLE_EOF = 38;
constexpr uint32_t ERROR_NOT_SUPPORTED = 50;
constexpr uint32_t ERROR_FILE_EXISTS = 80;
constexpr uint32_t ERROR_INVALID_PARAMETER = 87;
constexpr uint32_t ERROR_ALREADY_EXISTS = 183;
constexpr uint32_t ERROR_MORE_DATA = 234;
constexpr uint32_t ERROR_NO_MORE_ITEMS = 259;
constexpr uint32_t ERROR_IO_PENDING = 997;
constexpr uint32_t WAIT_OBJECT_0 = 0, WAIT_ABANDONED = 0x80, WAIT_TIMEOUT = 0x102, WAIT_FAILED = 0xFFFFFFFFu;
constexpr uint32_t INFINITE = 0xFFFFFFFFu;
constexpr uint32_t S_OK = 0, S_FALSE = 1;
constexpr uint32_t E_NOTIMPL = 0x80004001u, E_NOINTERFACE = 0x80004002u, E_FAIL = 0x80004005u,
                   E_INVALIDARG = 0x80070057u, E_OUTOFMEMORY = 0x8007000Eu;
}  // namespace win
