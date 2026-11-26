#include "runtime/vfs.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <cctype>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "runtime/process.h"

namespace vfs {

namespace {
std::string g_game, g_user;
std::string g_cwd = proc::kGameRootWin;
std::mutex g_mu;
// lowercase host path -> actual host path (only for existing entries)
std::unordered_map<std::string, std::string> g_case_cache;

std::string lower(std::string s) {
    for (char& ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}

bool starts_with_ci(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && strncasecmp(s.c_str(), prefix.c_str(), prefix.size()) == 0 &&
           (s.size() == prefix.size() || s[prefix.size()] == '\\');
}

std::vector<std::string> split(const std::string& p) {
    std::vector<std::string> parts;
    std::string cur;
    for (char ch : p) {
        if (ch == '\\') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

// Joins `rel` (backslash-separated) onto host `root`, resolving each component
// case-insensitively while it exists.
std::string resolve_under(const std::string& root, const std::string& rel) {
    std::string host = root;
    bool exists = true;
    for (const std::string& part : split(rel)) {
        std::string cand = host + "/" + part;
        if (exists) {
            std::lock_guard<std::mutex> lk(g_mu);
            std::string key = lower(cand);
            auto it = g_case_cache.find(key);
            if (it != g_case_cache.end()) {
                host = it->second;
                continue;
            }
            struct stat st;
            if (stat(cand.c_str(), &st) == 0) {
                g_case_cache[key] = cand;
                host = cand;
                continue;
            }
            bool found = false;
            if (DIR* d = opendir(host.c_str())) {
                while (dirent* e = readdir(d)) {
                    if (strcasecmp(e->d_name, part.c_str()) == 0) {
                        cand = host + "/" + e->d_name;
                        found = true;
                        break;
                    }
                }
                closedir(d);
            }
            if (found) {
                g_case_cache[key] = cand;
                host = cand;
                continue;
            }
            exists = false;
        }
        host = cand;
    }
    return host;
}
}  // namespace

void init(const std::string& game_dir, const std::string& user_dir) {
    g_game = game_dir;
    g_user = user_dir;
    mkdir((g_user + "/profile").c_str(), 0755);
}

std::string full_win_path(const std::string& guest_path) {
    std::string p = guest_path;
    for (char& ch : p)
        if (ch == '/') ch = '\\';
    std::string abs;
    if (p.size() >= 2 && p[1] == ':') {
        abs = p;
    } else if (!p.empty() && p[0] == '\\') {
        abs = "C:" + p;
    } else {
        std::string c = cwd();
        abs = c + (c.back() == '\\' ? "" : "\\") + p;
    }
    // Normalize "." and ".." components.
    std::string drive = abs.substr(0, 2);
    drive[0] = (char)toupper((unsigned char)drive[0]);
    std::vector<std::string> out;
    for (const std::string& part : split(abs.substr(2))) {
        if (part == ".") continue;
        if (part == "..") {
            if (!out.empty()) out.pop_back();
            continue;
        }
        out.push_back(part);
    }
    std::string r = drive;
    for (const std::string& part : out) r += "\\" + part;
    if (out.empty()) r += "\\";
    return r;
}

std::string to_host(const std::string& guest_path) {
    std::string w = full_win_path(guest_path);
    std::string root = proc::kGameRootWin;
    if (starts_with_ci(w, root)) return resolve_under(g_game, w.substr(root.size()));
    std::string prof = kUserProfileWin;
    if (starts_with_ci(w, prof)) return resolve_under(g_user + "/profile", w.substr(prof.size()));
    std::string drive_dir = g_user + "/drive_" + (char)tolower((unsigned char)w[0]);
    mkdir(drive_dir.c_str(), 0755);
    return resolve_under(drive_dir, w.substr(2));
}

std::string cwd() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_cwd;
}

bool set_cwd(const std::string& guest_path) {
    std::string w = full_win_path(guest_path);
    struct stat st;
    if (stat(to_host(w).c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    g_cwd = w;
    return true;
}

bool wildcard_match(const char* pat, const char* name) {
    // "*.*" matches everything on Windows, including names without a dot.
    if (strcmp(pat, "*.*") == 0 || strcmp(pat, "*") == 0) return true;
    const char* star = nullptr;
    const char* ss = nullptr;
    while (*name) {
        if (*pat == '?' || tolower((unsigned char)*pat) == tolower((unsigned char)*name)) {
            ++pat;
            ++name;
        } else if (*pat == '*') {
            star = pat++;
            ss = name;
        } else if (star) {
            pat = star + 1;
            name = ++ss;
        } else {
            return false;
        }
    }
    while (*pat == '*' || (*pat == '.' && pat[1] == '*')) pat += (*pat == '*') ? 1 : 2;
    return *pat == 0;
}

}  // namespace vfs
