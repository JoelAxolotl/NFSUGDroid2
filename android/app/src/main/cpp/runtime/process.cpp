#include "runtime/process.h"

#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "core/log.h"
#include "core/memory.h"
#include "hle/hle.h"
#include "loader/pe_loader.h"
#include "runtime/aot.h"
#include "runtime/vfs.h"

namespace proc {

namespace {
Config g_cfg;
std::mutex g_status_mu;
std::string g_status = "not started";
std::atomic<bool> g_running{false};
std::atomic<uint32_t> g_next_tid{0x1000};
uint32_t g_peb = 0;
uint32_t g_cmdline = 0;
thread_local Thread* t_thread = nullptr;

constexpr uint32_t kTebSize = 0x1000;
constexpr uint32_t kDefaultStack = 1 << 20;

// Thunk the guest returns into when a thread routine returns normally.
void thread_return_hle(cpu::Cpu& c) { exit_current_thread(c, c.r[cpu::EAX]); }
uint32_t g_thread_return_thunk = 0;

uint32_t alloc_teb(Thread* t) {
    uint32_t teb = mem::alloc_system(kTebSize);
    mem::wr32(teb + 0x00, 0xFFFFFFFFu);  // SEH chain end
    mem::wr32(teb + 0x04, t->stack_base + t->stack_size);
    mem::wr32(teb + 0x08, t->stack_base);
    mem::wr32(teb + 0x18, teb);
    mem::wr32(teb + 0x20, (uint32_t)getpid());
    mem::wr32(teb + 0x24, t->tid);
    mem::wr32(teb + 0x2C, teb + 0xE10);  // ThreadLocalStoragePointer (unused by the game)
    mem::wr32(teb + 0x30, g_peb);
    return teb;
}

bool setup_thread(Thread* t, uint32_t stack_size) {
    t->tid = g_next_tid.fetch_add(4);
    if (!stack_size) stack_size = kDefaultStack;
    stack_size = (stack_size + 0xFFFF) & ~0xFFFFu;
    t->stack_size = stack_size;
    t->stack_base = mem::reserve(0, stack_size, true);
    if (!t->stack_base) return false;
    t->teb = alloc_teb(t);
    cpu::reset(t->ctx);
    t->ctx.fs_base = t->teb;
    t->ctx.thread_id = t->tid;
    t->ctx.r[cpu::ESP] = t->stack_base + stack_size - 16;
    return true;
}

void name_host_thread(const char* name) { pthread_setname_np(pthread_self(), name); }

void report_stop(Thread* t, cpu::Stop s) {
    if (s == cpu::Stop::Fault) {
        char d[512];
        cpu::dump(t->ctx, d, sizeof d);
        LOGE("guest thread %x faulted: %s", t->tid, d);
        if (status().rfind("Unimplemented", 0) != 0) {
            char buf[96];
            snprintf(buf, sizeof buf, "CPU fault at %08x (thread %x)", t->ctx.eip, t->tid);
            set_status(buf);
        }
        logx::flush();
        g_running = false;
    }
}

// Guest threads run translated code, which maps guest calls to host calls, so
// they need far deeper host stacks than Android's ~1 MiB default.
void start_host_thread(std::function<void()> fn) {
    constexpr size_t kHostStack = 16u << 20;
    auto* heap_fn = new std::function<void()>(std::move(fn));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kHostStack);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    int rc = pthread_create(&th, &attr, [](void* p) -> void* {
        auto* f = static_cast<std::function<void()>*>(p);
        (*f)();
        delete f;
        return nullptr;
    }, heap_fn);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        LOGE("pthread_create failed: %d", rc);
        delete heap_fn;
    }
}

void thread_main(Thread* t) {
    name_host_thread("guest");
    t_thread = t;
    cpu::set_current(&t->ctx);
    {
        // CREATE_SUSPENDED: wait for ResumeThread.
        std::unique_lock<std::mutex> lk(kobj::wait_mutex());
        kobj::wait_cv().wait(lk, [t] { return t->suspend_count == 0; });
    }
    cpu::Cpu& c = t->ctx;
    cpu::push32(c, t->param);
    cpu::push32(c, g_thread_return_thunk);
    c.eip = t->start;
    __atomic_store_n(&c.in_host, 0, __ATOMIC_SEQ_CST);  // from here on it runs guest code
    LOGI("thread %x start %08x(%08x)", t->tid, t->start, t->param);
    cpu::Stop s = cpu::run(c);
    report_stop(t, s);
    {
        std::lock_guard<std::mutex> lk(kobj::wait_mutex());
        t->exited = true;
    }
    kobj::notify();
}

// SIGSEGV/SIGBUS inside the guest window are guest faults: report them with
// the guest context, then park the thread. Other faults chain to ART.
struct sigaction g_old_segv, g_old_bus;

void fault_handler(int sig, siginfo_t* si, void* uc) {
    uint8_t* a = (uint8_t*)si->si_addr;
    if (mem::base && a >= mem::base && a < mem::base + (1ull << 32)) {
        cpu::Cpu* c = cpu::current();
        char d[512] = "no guest context";
        if (c) cpu::dump(*c, d, sizeof d);
        LOGE("guest memory fault (sig %d) at guest %08x: %s", sig, (uint32_t)(a - mem::base), d);
        if (c && mem::is_committed(c->r[cpu::ESP], 64)) {
            uint32_t sp = c->r[cpu::ESP];
            for (int row = 0; row < 4; ++row) {
                uint32_t p = sp + row * 16;
                LOGE("  stack %08x: %08x %08x %08x %08x", p, mem::rd32(p), mem::rd32(p + 4), mem::rd32(p + 8),
                     mem::rd32(p + 12));
            }
        }
        char buf[96];
        snprintf(buf, sizeof buf, "Memory fault @%08x (EIP %08x)", (uint32_t)(a - mem::base), c ? c->eip : 0);
        set_status(buf);
        logx::flush();
        g_running = false;
        for (;;) pause();
    }
    struct sigaction* old = sig == SIGSEGV ? &g_old_segv : &g_old_bus;
    if (old->sa_flags & SA_SIGINFO) {
        old->sa_sigaction(sig, si, uc);
    } else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        old->sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

void install_fault_handler() {
    struct sigaction sa {};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS, &sa, &g_old_bus);
}

void setup_peb(const loader::Image& img) {
    g_peb = mem::alloc_system(0x1000);
    mem::wr8(g_peb + 0x02, 0);  // BeingDebugged
    mem::wr32(g_peb + 0x08, img.base);
    mem::wr32(g_peb + 0x18, 0x00150000);  // ProcessHeap handle value (see kernel32 heap)
    std::string cmd = std::string("\"") + kGameRootWin + "\\" + kExeName + "\"";
    g_cmdline = mem::alloc_system((uint32_t)cmd.size() + 1);
    mem::write_cstr(g_cmdline, cmd.c_str(), (uint32_t)cmd.size() + 1);
}
}  // namespace

const Config& config() { return g_cfg; }

void set_status(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_status_mu);
    g_status = s;
}
std::string status() {
    std::lock_guard<std::mutex> lk(g_status_mu);
    return g_status;
}
bool running() { return g_running; }

uint32_t peb() { return g_peb; }
uint32_t process_heap_handle() { return 0x00150000; }
uint32_t command_line_a() { return g_cmdline; }

Thread* current_thread() { return t_thread; }
uint32_t current_tid() { return t_thread ? t_thread->tid : 0; }

uint32_t create_thread(uint32_t start, uint32_t param, uint32_t stack_size, bool suspended, uint32_t* tid_out) {
    Thread* t = new Thread();
    if (!setup_thread(t, stack_size)) {
        delete t;
        return 0;
    }
    t->start = start;
    t->param = param;
    t->suspend_count = suspended ? 1 : 0;
    t->ctx.in_host = 1;  // not running guest code until thread_main starts it
    t->refs.fetch_add(1);  // the running host thread holds a reference
    uint32_t h = kobj::add(t);
    if (tid_out) *tid_out = t->tid;
    start_host_thread([t] { thread_main(t); });
    return h;
}

uint32_t resume_thread(Thread* t) {
    uint32_t prev;
    {
        std::lock_guard<std::mutex> lk(kobj::wait_mutex());
        prev = t->suspend_count;
        if (t->suspend_count) t->suspend_count--;
        if (t->suspend_count == 0) __atomic_store_n(&t->ctx.suspend_req, 0, __ATOMIC_SEQ_CST);
    }
    kobj::notify();
    return prev;
}

// SuspendThread. Recompiled code cannot be stopped at an arbitrary instruction, so
// the target stops at its next safe point (guest function entry, interpreter step,
// return from host code). Like on Windows, this returns once the target no longer
// runs guest code: it is parked, or inside host code that parks before returning.
// NFSU2 relies on this: its audio library is not reentrant, and the main thread
// suspends the audio thread around its own calls into it.
uint32_t suspend_thread(Thread* t) {
    uint32_t prev;
    {
        std::lock_guard<std::mutex> lk(kobj::wait_mutex());
        prev = t->suspend_count++;
        if (t->exited) return prev;
        __atomic_store_n(&t->ctx.suspend_req, 1, __ATOMIC_SEQ_CST);
    }
    if (t == t_thread) return prev;  // parks when this HLE call returns
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    std::unique_lock<std::mutex> lk(kobj::wait_mutex());
    while (!t->parked && !t->exited && __atomic_load_n(&t->ctx.in_host, __ATOMIC_SEQ_CST) == 0) {
        if (kobj::wait_cv().wait_until(lk, std::min(deadline, std::chrono::steady_clock::now() +
                                                                  std::chrono::milliseconds(1))) ==
                std::cv_status::timeout &&
            std::chrono::steady_clock::now() >= deadline) {
            LOGW("SuspendThread: thread %x did not reach a safe point in 500 ms (eip %08x)", t->tid, t->ctx.eip);
            break;
        }
    }
    return prev;
}
}  // namespace proc

namespace cpu {
void safepoint(Cpu& c) {
    proc::Thread* t = proc::current_thread();
    if (!t || &t->ctx != &c) return;
    std::unique_lock<std::mutex> lk(kobj::wait_mutex());
    if (t->suspend_count == 0) return;
    t->parked = true;
    kobj::wait_cv().notify_all();  // wake the suspender
    kobj::wait_cv().wait(lk, [t] { return t->suspend_count == 0; });
    t->parked = false;
}
}  // namespace cpu

namespace proc {

void exit_current_thread(cpu::Cpu& c, uint32_t code) {
    if (Thread* t = t_thread) t->exit_code = code;
    LOGI("thread %x exit code %u", current_tid(), code);
    c.stop = cpu::Stop::ThreadExit;
}

void exit_process(uint32_t code) {
    LOGI("ExitProcess(%u)", code);
    set_status("Game exited (code " + std::to_string(code) + ")");
    aot::dump_profile((g_cfg.user_dir + "/aot_profile.txt").c_str());
    logx::flush();
    g_running = false;
    for (;;) pause();  // GameActivity notices !running() and ends the process
}

void spawn_helper(const char* name, std::function<void(cpu::Cpu&)> body) {
    Thread* t = new Thread();
    if (!setup_thread(t, 256 * 1024)) {
        LOGE("helper thread %s: out of guest memory", name);
        delete t;
        return;
    }
    std::string nm = name;
    start_host_thread([t, nm, body = std::move(body)]() {
        name_host_thread(nm.c_str());
        t_thread = t;
        cpu::set_current(&t->ctx);
        body(t->ctx);
    });
}

bool boot(const Config& cfg, std::string& error) {
    g_cfg = cfg;
    mkdir(cfg.user_dir.c_str(), 0755);
    // Keep the previous run's log: crashes that restart the activity would otherwise erase it.
    rename((cfg.user_dir + "/log.txt").c_str(), (cfg.user_dir + "/log.prev.txt").c_str());
    logx::init((cfg.user_dir + "/log.txt").c_str());
    LOGI("NFSUG2 translation layer starting; game dir %s", cfg.game_dir.c_str());

    if (!mem::init()) {
        error = "cannot reserve guest address space";
        return false;
    }
    install_fault_handler();
    // Null page guard: everything below kNullGuardEnd stays PROT_NONE.
    hle::init();
    hle::set_trace(access((cfg.user_dir + "/trace_hle").c_str(), F_OK) == 0);
    vfs::init(cfg.game_dir, cfg.user_dir);
    g_thread_return_thunk = hle::make_thunk("<thread-return>", thread_return_hle, 0);

    std::string exe = vfs::to_host(std::string(kGameRootWin) + "\\" + kExeName);
    loader::Image img;
    if (!loader::load_exe(exe, img, error)) return false;
    aot::init(img.text_start, img.text_end, exe.c_str());
    setup_peb(img);

    Thread* main = new Thread();
    if (!setup_thread(main, img.stack_reserve ? img.stack_reserve : kDefaultStack)) {
        error = "cannot allocate main thread stack";
        return false;
    }
    main->start = img.base + img.entry;
    main->refs.fetch_add(1);
    kobj::add(main);
    g_running = true;
    set_status("Running (interpreter)");

    // The CRT entry point takes no arguments; give it the same frame as a
    // thread routine so a plain return exits the thread cleanly.
    start_host_thread([main]() {
        name_host_thread("guest-main");
        t_thread = main;
        cpu::set_current(&main->ctx);
        cpu::Cpu& c = main->ctx;
        cpu::push32(c, g_thread_return_thunk);
        c.eip = main->start;
        LOGI("main thread %x entering %08x", main->tid, c.eip);
        cpu::Stop s = cpu::run(c);
        report_stop(main, s);
        if (s == cpu::Stop::ThreadExit) exit_process(main->exit_code);
    });
    return true;
}

}  // namespace proc
