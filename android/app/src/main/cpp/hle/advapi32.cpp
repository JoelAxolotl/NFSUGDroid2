// advapi32: registry, backed by <user>/registry.txt.
#include <strings.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

#include "hle/hle.h"
#include "hle/win_util.h"
#include "runtime/process.h"

using namespace hle;
using winu::gstr;

namespace {
constexpr uint32_t REG_SZ = 1, REG_BINARY = 3, REG_DWORD = 4;

struct Value {
    uint32_t type;
    std::vector<uint8_t> data;
};

struct CiLess {
    bool operator()(const std::string& a, const std::string& b) const { return strcasecmp(a.c_str(), b.c_str()) < 0; }
};

// key path ("HKLM\Software\...") -> value name -> value
std::map<std::string, std::map<std::string, Value, CiLess>, CiLess> g_reg;
std::map<uint32_t, std::string> g_open;
uint32_t g_next = 0x00A00000;
std::mutex g_mu;
bool g_loaded = false;

std::string root_name(uint32_t hkey) {
    switch (hkey) {
        case 0x80000000u: return "HKCR";
        case 0x80000001u: return "HKCU";
        case 0x80000002u: return "HKLM";
        case 0x80000003u: return "HKU";
        default: {
            auto it = g_open.find(hkey);
            return it == g_open.end() ? "" : it->second;
        }
    }
}

std::string join(const std::string& base, const std::string& sub) {
    if (sub.empty()) return base;
    std::string s = sub;
    while (!s.empty() && s.back() == '\\') s.pop_back();
    return base + "\\" + s;
}

void set_sz(const std::string& key, const std::string& name, const std::string& v) {
    Value val{REG_SZ, std::vector<uint8_t>(v.begin(), v.end())};
    val.data.push_back(0);
    g_reg[key][name] = val;
}

std::string reg_path() { return proc::config().user_dir + "/registry.txt"; }

void save_locked() {
    FILE* f = fopen(reg_path().c_str(), "w");
    if (!f) return;
    for (auto& [k, vals] : g_reg) {
        fprintf(f, "[%s]\n", k.c_str());
        for (auto& [n, v] : vals) {
            fprintf(f, "%s=%u:", n.c_str(), v.type);
            for (uint8_t b : v.data) fprintf(f, "%02x", b);
            fprintf(f, "\n");
        }
    }
    fclose(f);
}

void load_locked() {
    if (g_loaded) return;
    g_loaded = true;
    const std::string game = "HKLM\\Software\\EA Games\\Need for Speed Underground 2";
    set_sz(game, "Install Dir", proc::kGameRootWin);
    set_sz(game, "CD Drive", "C:\\");
    set_sz(game, "Language", "English US");
    set_sz(game, "StreamingInstall", "0");
    set_sz(game, "SwapSize", "0");
    set_sz(game, "CacheSize", "0");
    set_sz(game, "DisplayName", "Need for Speed Underground 2");
    set_sz(game, "Locale", "en_US");
    set_sz(game, "Region", "NA");
    FILE* f = fopen(reg_path().c_str(), "r");
    if (!f) return;
    char line[4096];
    std::string key;
    while (fgets(line, sizeof line, f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.size() > 2 && s[0] == '[') {
            key = s.substr(1, s.size() - 2);
            g_reg[key];
            continue;
        }
        size_t eq = s.find('='), colon = s.find(':', eq);
        if (eq == std::string::npos || colon == std::string::npos) continue;
        Value v;
        v.type = (uint32_t)strtoul(s.substr(eq + 1, colon - eq - 1).c_str(), nullptr, 10);
        for (size_t i = colon + 1; i + 1 < s.size(); i += 2)
            v.data.push_back((uint8_t)strtoul(s.substr(i, 2).c_str(), nullptr, 16));
        g_reg[key][s.substr(0, eq)] = v;
    }
    fclose(f);
}

uint32_t open_key(uint32_t hkey, uint32_t sub, uint32_t out, bool create) {
    std::lock_guard<std::mutex> lk(g_mu);
    load_locked();
    std::string base = root_name(hkey);
    if (base.empty()) return win::ERROR_INVALID_HANDLE;
    std::string path = join(base, gstr(sub));
    if (!g_reg.count(path)) {
        if (!create) {
            LOGI("RegOpenKey miss: %s", path.c_str());
            return win::ERROR_FILE_NOT_FOUND;
        }
        g_reg[path];
        save_locked();
    }
    uint32_t h = g_next += 4;
    g_open[h] = path;
    mem::wr32(out, h);
    return win::ERROR_SUCCESS;
}
}  // namespace

HLE_EXPORT(advapi32, RegOpenKeyExA, 20) { ret(c, open_key(arg(c, 0), arg(c, 1), arg(c, 4), false)); }
HLE_EXPORT(advapi32, RegOpenKeyA, 12) { ret(c, open_key(arg(c, 0), arg(c, 1), arg(c, 2), false)); }
HLE_EXPORT(advapi32, RegCreateKeyA, 12) { ret(c, open_key(arg(c, 0), arg(c, 1), arg(c, 2), true)); }

HLE_EXPORT(advapi32, RegCloseKey, 4) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_open.erase(arg(c, 0));
    ret(c, 0);
}

HLE_EXPORT(advapi32, RegQueryValueExA, 24) {
    uint32_t hkey = arg(c, 0), name = arg(c, 1), ptype = arg(c, 3), data = arg(c, 4), pcb = arg(c, 5);
    std::lock_guard<std::mutex> lk(g_mu);
    load_locked();
    std::string key = root_name(hkey);
    std::string vname = gstr(name);
    auto k = g_reg.find(key);
    if (k == g_reg.end() || !k->second.count(vname)) {
        LOGI("RegQueryValue miss: %s\\%s", key.c_str(), vname.c_str());
        return ret(c, win::ERROR_FILE_NOT_FOUND);
    }
    const Value& v = k->second[vname];
    if (ptype) mem::wr32(ptype, v.type);
    uint32_t need = (uint32_t)v.data.size();
    if (!pcb) return ret(c, data ? win::ERROR_INVALID_PARAMETER : 0);
    uint32_t cap = mem::rd32(pcb);
    mem::wr32(pcb, need);
    if (!data) return ret(c, 0);
    if (cap < need) return ret(c, win::ERROR_MORE_DATA);
    memcpy(mem::ptr(data), v.data.data(), need);
    LOGI("RegQueryValue %s\\%s (%u bytes)", key.c_str(), vname.c_str(), need);
    ret(c, 0);
}

HLE_EXPORT(advapi32, RegSetValueExA, 24) {
    uint32_t hkey = arg(c, 0), name = arg(c, 1), type = arg(c, 3), data = arg(c, 4), cb = arg(c, 5);
    std::lock_guard<std::mutex> lk(g_mu);
    load_locked();
    std::string key = root_name(hkey);
    if (key.empty()) return ret(c, win::ERROR_INVALID_HANDLE);
    Value v{type, std::vector<uint8_t>(mem::ptr(data), mem::ptr(data) + cb)};
    g_reg[key][gstr(name)] = v;
    save_locked();
    ret(c, 0);
}
