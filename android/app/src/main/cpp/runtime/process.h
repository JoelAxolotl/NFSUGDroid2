#pragma once
// Guest process model: boot, PEB/TEB, guest threads and status reporting.
#include <cstdint>
#include <functional>
#include <string>

#include "cpu/cpu.h"
#include "hle/kobjects.h"

namespace proc {

struct Config {
    std::string game_dir;  // host path that appears to the guest as kGameRootWin
    std::string user_dir;  // saves, registry, logs
};

// Windows-side location of the game. Short and space-free on purpose.
constexpr const char* kGameRootWin = "C:\\NFSU2";
constexpr const char* kExeName = "SPEED2.EXE";

// Waitable guest thread object.
struct Thread : kobj::Object {
    Thread() : kobj::Object(kobj::Type::Thread) {}
    bool signaled(uint32_t) const override { return exited; }
    cpu::Cpu ctx{};
    uint32_t tid = 0;
    uint32_t teb = 0;
    uint32_t stack_base = 0;  // lowest address
    uint32_t stack_size = 0;
    uint32_t start = 0, param = 0;
    int32_t priority = 0;
    uint32_t suspend_count = 0;  // guarded by kobj::wait_mutex()
    bool parked = false;         // stopped at a safe point (guarded by kobj::wait_mutex())
    bool exited = false;
    uint32_t exit_code = 0x103;  // STILL_ACTIVE
};

bool boot(const Config& cfg, std::string& error);
const Config& config();

void set_status(const std::string& s);
std::string status();
bool running();

uint32_t peb();
uint32_t process_heap_handle();
uint32_t command_line_a();

// Current guest thread (null on host-only threads).
Thread* current_thread();
uint32_t current_tid();

// CreateThread. Returns the handle (0 on failure).
uint32_t create_thread(uint32_t start, uint32_t param, uint32_t stack_size, bool suspended, uint32_t* tid_out);
uint32_t resume_thread(Thread* t);   // returns previous suspend count
uint32_t suspend_thread(Thread* t);  // returns previous suspend count

// ExitThread from inside an HLE call on the current thread.
void exit_current_thread(cpu::Cpu& c, uint32_t code);
[[noreturn]] void exit_process(uint32_t code);

// Starts a host thread that owns a fresh guest context (TEB + stack) so it
// can call guest callbacks (winmm timers, audio notifications, ...).
void spawn_helper(const char* name, std::function<void(cpu::Cpu&)> body);

}  // namespace proc
