#include "hle/kobjects.h"

#include <chrono>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "hle/hle.h"

namespace kobj {

namespace {
std::mutex g_wait_mu;
std::condition_variable g_wait_cv;

std::mutex g_tab_mu;
std::vector<Object*> g_table;  // handle = 0x100 + 4 * index
std::unordered_map<std::string, Object*> g_named;

constexpr uint32_t kHandleBase = 0x100;

inline int index_of(uint32_t h) {
    if (h < kHandleBase || (h & 3)) return -1;
    return (int)((h - kHandleBase) >> 2);
}

void unref(Object* o) {
    if (o->refs.fetch_sub(1) == 1) {
        for (auto it = g_named.begin(); it != g_named.end(); ++it) {
            if (it->second == o) {
                g_named.erase(it);
                break;
            }
        }
        delete o;
    }
}
}  // namespace

std::mutex& wait_mutex() { return g_wait_mu; }
std::condition_variable& wait_cv() { return g_wait_cv; }
void notify() { g_wait_cv.notify_all(); }

uint32_t add(Object* o) {
    std::lock_guard<std::mutex> lk(g_tab_mu);
    for (size_t i = 0; i < g_table.size(); ++i) {
        if (!g_table[i]) {
            g_table[i] = o;
            return kHandleBase + (uint32_t)i * 4;
        }
    }
    g_table.push_back(o);
    return kHandleBase + (uint32_t)(g_table.size() - 1) * 4;
}

Object* get(uint32_t handle) {
    std::lock_guard<std::mutex> lk(g_tab_mu);
    int i = index_of(handle);
    if (i < 0 || i >= (int)g_table.size()) return nullptr;
    return g_table[i];
}

bool close(uint32_t handle) {
    std::lock_guard<std::mutex> lk(g_tab_mu);
    int i = index_of(handle);
    if (i < 0 || i >= (int)g_table.size() || !g_table[i]) return false;
    Object* o = g_table[i];
    g_table[i] = nullptr;
    unref(o);
    return true;
}

uint32_t duplicate(uint32_t handle) {
    Object* o = get(handle);
    if (!o) return 0;
    o->refs.fetch_add(1);
    return add(o);
}

Object* find_named(const std::string& name) {
    std::lock_guard<std::mutex> lk(g_tab_mu);
    auto it = g_named.find(name);
    if (it == g_named.end()) return nullptr;
    it->second->refs.fetch_add(1);
    return it->second;
}

void set_name(Object* o, const std::string& name) {
    std::lock_guard<std::mutex> lk(g_tab_mu);
    g_named[name] = o;
}

uint32_t wait(const uint32_t* handles, uint32_t count, bool wait_all, uint32_t timeout_ms, uint32_t tid) {
    if (count == 0 || count > 64) return win::WAIT_FAILED;
    Object* objs[64];
    for (uint32_t i = 0; i < count; ++i) {
        objs[i] = get(handles[i]);
        if (!objs[i]) {
            LOGW("wait on invalid handle %08x", handles[i]);
            return win::WAIT_FAILED;
        }
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lk(g_wait_mu);
    for (;;) {
        if (wait_all) {
            bool all = true;
            for (uint32_t i = 0; i < count && all; ++i) all = objs[i]->signaled(tid);
            if (all) {
                for (uint32_t i = 0; i < count; ++i) objs[i]->acquire(tid);
                return win::WAIT_OBJECT_0;
            }
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                if (objs[i]->signaled(tid)) {
                    objs[i]->acquire(tid);
                    return win::WAIT_OBJECT_0 + i;
                }
            }
        }
        if (timeout_ms == 0) return win::WAIT_TIMEOUT;
        if (timeout_ms == win::INFINITE) {
            g_wait_cv.wait(lk);
        } else {
            if (std::chrono::steady_clock::now() >= deadline) return win::WAIT_TIMEOUT;
            g_wait_cv.wait_until(lk, deadline);  // loop re-checks the objects either way
        }
    }
}

}  // namespace kobj
