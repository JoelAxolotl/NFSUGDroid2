#include "loader/pe_loader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <vector>

#include "core/log.h"
#include "core/memory.h"
#include "hle/hle.h"

namespace loader {

namespace {
Image g_main;

template <typename T>
T rdh(const std::vector<uint8_t>& f, size_t off) {
    T v{};
    if (off + sizeof(T) <= f.size()) memcpy(&v, f.data() + off, sizeof(T));
    return v;
}

// Compares a resource directory entry name (UTF-16 counted string at guest
// address) with an ANSI name, case-insensitively.
bool rsrc_name_eq(uint32_t ustr, const char* s) {
    uint16_t len = mem::rd16(ustr);
    if (strlen(s) != len) return false;
    for (uint16_t i = 0; i < len; ++i) {
        uint16_t ch = mem::rd16(ustr + 2 + i * 2);
        if (toupper(ch & 0xFF) != toupper((unsigned char)s[i]) || ch > 0xFF) return false;
    }
    return true;
}

// Finds the subdirectory/data entry matching `key` (id or guest string ptr)
// in the resource directory at `dir` (guest address). Returns the entry's
// OffsetToData field or 0xFFFFFFFF.
uint32_t rsrc_lookup(uint32_t root, uint32_t dir, uint32_t key) {
    uint16_t named = mem::rd16(dir + 12);
    uint16_t ids = mem::rd16(dir + 14);
    uint32_t e = dir + 16;
    const char* skey = nullptr;
    if (key >= 0x10000) {
        skey = mem::cstr(key);
        if (skey[0] == '#') {  // "#123" means integer id 123
            key = (uint32_t)atoi(skey + 1);
            skey = nullptr;
        }
    }
    for (uint32_t i = 0; i < (uint32_t)named + ids; ++i, e += 8) {
        uint32_t name = mem::rd32(e);
        uint32_t off = mem::rd32(e + 4);
        bool is_named = name & 0x80000000u;
        if (skey) {
            if (is_named && rsrc_name_eq(root + (name & 0x7FFFFFFF), skey)) return off;
        } else if (!is_named && (name & 0xFFFF) == key) {
            return off;
        }
    }
    return 0xFFFFFFFFu;
}
}  // namespace

const Image& main_image() { return g_main; }

bool load_exe(const std::string& host_path, Image& img, std::string& error) {
    FILE* fp = fopen(host_path.c_str(), "rb");
    if (!fp) {
        error = "cannot open " + host_path;
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> f((size_t)fsz);
    if (fread(f.data(), 1, f.size(), fp) != f.size()) {
        fclose(fp);
        error = "short read";
        return false;
    }
    fclose(fp);

    if (rdh<uint16_t>(f, 0) != 0x5A4D) {
        error = "not an MZ executable";
        return false;
    }
    uint32_t nt = rdh<uint32_t>(f, 0x3C);
    if (rdh<uint32_t>(f, nt) != 0x00004550 || rdh<uint16_t>(f, nt + 4) != 0x14C) {
        error = "not a PE32 x86 image";
        return false;
    }
    uint16_t nsec = rdh<uint16_t>(f, nt + 6);
    uint16_t opt_size = rdh<uint16_t>(f, nt + 20);
    uint32_t opt = nt + 24;
    if (rdh<uint16_t>(f, opt) != 0x10B) {
        error = "not PE32";
        return false;
    }
    img.entry = rdh<uint32_t>(f, opt + 16);
    img.base = rdh<uint32_t>(f, opt + 28);
    img.size = rdh<uint32_t>(f, opt + 56);
    uint32_t hdr_size = rdh<uint32_t>(f, opt + 60);
    img.stack_reserve = rdh<uint32_t>(f, opt + 72);
    uint32_t dd = opt + 96;
    uint32_t imp_rva = rdh<uint32_t>(f, dd + 8);
    img.rsrc_rva = rdh<uint32_t>(f, dd + 16);
    img.rsrc_size = rdh<uint32_t>(f, dd + 20);

    if (mem::reserve(img.base, img.size, true, img.base, img.base + img.size) != img.base) {
        error = "image base range is not available";
        return false;
    }
    memcpy(mem::ptr(img.base), f.data(), std::min<size_t>(hdr_size, f.size()));

    uint32_t sec = opt + opt_size;
    for (uint16_t i = 0; i < nsec; ++i, sec += 40) {
        char name[9] = {};
        memcpy(name, f.data() + sec, 8);
        uint32_t vsize = rdh<uint32_t>(f, sec + 8);
        uint32_t va = rdh<uint32_t>(f, sec + 12);
        uint32_t rawsz = rdh<uint32_t>(f, sec + 16);
        uint32_t rawptr = rdh<uint32_t>(f, sec + 20);
        uint32_t n = std::min(rawsz, vsize ? vsize : rawsz);
        if (rawptr + (uint64_t)n > f.size()) n = rawptr < f.size() ? (uint32_t)(f.size() - rawptr) : 0;
        if (n) memcpy(mem::ptr(img.base + va), f.data() + rawptr, n);
        if (strcmp(name, ".text") == 0) {
            img.text_start = img.base + va;
            img.text_end = img.base + va + vsize;
        }
        LOGI("section %-8s va=%08x vsize=%08x raw=%08x", name, img.base + va, vsize, rawsz);
    }

    // Imports.
    if (imp_rva) {
        for (uint32_t d = img.base + imp_rva;; d += 20) {
            uint32_t ilt = mem::rd32(d), name_rva = mem::rd32(d + 12), iat = mem::rd32(d + 16);
            if (!name_rva && !iat) break;
            std::string dll = mem::cstr(img.base + name_rva);
            uint32_t lookup = img.base + (ilt ? ilt : iat);
            uint32_t slot = img.base + iat;
            for (;; lookup += 4, slot += 4) {
                uint32_t t = mem::rd32(lookup);
                if (!t) break;
                std::string sym;
                if (t & 0x80000000u) {
                    sym = "#" + std::to_string(t & 0xFFFF);
                } else {
                    sym = mem::cstr(img.base + t + 2);
                }
                uint32_t thunk = hle::resolve_import(dll, sym);
                if (hle::find_export(dll, sym))
                    img.imports_bound++;
                else
                    img.imports_missing++;
                mem::wr32(slot, thunk);
            }
        }
    }
    LOGI("image %08x-%08x entry %08x; imports bound %u, missing %u", img.base, img.base + img.size,
         img.base + img.entry, img.imports_bound, img.imports_missing);
    g_main = img;
    return true;
}

uint32_t find_resource(uint32_t name, uint32_t type) {
    if (!g_main.rsrc_rva) return 0;
    uint32_t root = g_main.base + g_main.rsrc_rva;
    uint32_t t = rsrc_lookup(root, root, type);
    if (t == 0xFFFFFFFFu || !(t & 0x80000000u)) return 0;
    uint32_t n = rsrc_lookup(root, root + (t & 0x7FFFFFFF), name);
    if (n == 0xFFFFFFFFu) return 0;
    if (n & 0x80000000u) {
        // Language level: take the first entry.
        uint32_t ldir = root + (n & 0x7FFFFFFF);
        n = mem::rd32(ldir + 16 + 4);
        if (n & 0x80000000u) return 0;
    }
    return root + n;
}

uint32_t resource_data(uint32_t entry) { return entry ? g_main.base + mem::rd32(entry) : 0; }
uint32_t resource_size(uint32_t entry) { return entry ? mem::rd32(entry + 4) : 0; }

}  // namespace loader
