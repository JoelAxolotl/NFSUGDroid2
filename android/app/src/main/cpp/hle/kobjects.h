#pragma once
// Win32 kernel object handles. All waitable state changes happen under one
// process-wide mutex with one condition variable; the game uses a handful of
// threads, so contention is negligible and multi-object waits stay simple.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

namespace kobj {

enum class Type : uint8_t { File, Event, Mutex, Semaphore, Thread, Mapping, Find, Other };

struct Object {
    explicit Object(Type t) : type(t) {}
    virtual ~Object() = default;
    // Waitable objects override these; both are called with wait_mutex() held.
    virtual bool signaled(uint32_t /*tid*/) const { return false; }
    virtual void acquire(uint32_t /*tid*/) {}
    const Type type;
    std::atomic<int> refs{1};
};

struct Event : Object {
    Event(bool manual, bool initial) : Object(Type::Event), manual_reset(manual), state(initial) {}
    bool signaled(uint32_t) const override { return state; }
    void acquire(uint32_t) override {
        if (!manual_reset) state = false;
    }
    bool manual_reset;
    bool state;
};

struct Mutex : Object {
    explicit Mutex(uint32_t owner_tid) : Object(Type::Mutex), owner(owner_tid), count(owner_tid ? 1 : 0) {}
    bool signaled(uint32_t tid) const override { return count == 0 || owner == tid; }
    void acquire(uint32_t tid) override {
        owner = tid;
        ++count;
    }
    uint32_t owner;
    uint32_t count;
};

struct Semaphore : Object {
    Semaphore(int32_t initial, int32_t max) : Object(Type::Semaphore), count(initial), maximum(max) {}
    bool signaled(uint32_t) const override { return count > 0; }
    void acquire(uint32_t) override { --count; }
    int32_t count, maximum;
};

std::mutex& wait_mutex();
std::condition_variable& wait_cv();
// Wakes all waiters after a state change (caller must not hold wait_mutex()).
void notify();

uint32_t add(Object* o);
Object* get(uint32_t handle);  // borrowed pointer; null for invalid handles
template <typename T>
T* get_as(uint32_t handle, Type t) {
    Object* o = get(handle);
    return o && o->type == t ? static_cast<T*>(o) : nullptr;
}
bool close(uint32_t handle);
uint32_t duplicate(uint32_t handle);

// Named objects (CreateEventA / CreateMutexA with a name).
Object* find_named(const std::string& name);
void set_name(Object* o, const std::string& name);

// Win32 WaitForMultipleObjects semantics. Returns WAIT_OBJECT_0 + i,
// WAIT_TIMEOUT or WAIT_FAILED.
uint32_t wait(const uint32_t* handles, uint32_t count, bool wait_all, uint32_t timeout_ms, uint32_t tid);

}  // namespace kobj
