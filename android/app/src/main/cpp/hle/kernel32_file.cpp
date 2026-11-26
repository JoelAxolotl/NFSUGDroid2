// kernel32: files, directories, mappings, disks, resources.
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <mutex>
#include <vector>

#include "hle/hle.h"
#include "hle/kobjects.h"
#include "hle/win_util.h"
#include "loader/pe_loader.h"
#include "runtime/process.h"
#include "runtime/vfs.h"

using namespace hle;
using winu::gstr;

namespace {
struct File : kobj::Object {
    File() : kobj::Object(kobj::Type::File) {}
    ~File() override {
        if (fd >= 0) close(fd);
    }
    int fd = -1;
    std::string host;
    std::string win;
};

struct FindData {
    std::string name;
    bool dir;
    uint64_t size;
    uint64_t mtime_ft;
};

struct Find : kobj::Object {
    Find() : kobj::Object(kobj::Type::Find) {}
    std::vector<FindData> entries;
    size_t next = 0;
};

struct Mapping : kobj::Object {
    Mapping() : kobj::Object(kobj::Type::Mapping) {}
    std::string host;
    uint32_t size = 0;
};

constexpr uint32_t kStdOut = 0x14, kStdErr = 0x18;

uint32_t create_file(cpu::Cpu& c, const std::string& name, uint32_t access, uint32_t disposition) {
    if (name.empty() || name.rfind("\\\\.\\", 0) == 0) {
        set_last_error(win::ERROR_FILE_NOT_FOUND);
        return win::INVALID_HANDLE;
    }
    std::string host = vfs::to_host(name);
    struct stat st;
    bool exists = stat(host.c_str(), &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) {
        set_last_error(win::ERROR_ACCESS_DENIED);
        return win::INVALID_HANDLE;
    }
    int flags = 0;
    bool rd = access & 0x80000000u, wrt = access & 0x40000000u;
    flags = (rd && wrt) ? O_RDWR : wrt ? O_WRONLY : O_RDONLY;
    switch (disposition) {
        case 1:  // CREATE_NEW
            if (exists) {
                set_last_error(win::ERROR_FILE_EXISTS);
                return win::INVALID_HANDLE;
            }
            flags |= O_CREAT | O_EXCL;
            break;
        case 2: flags |= O_CREAT | O_TRUNC; break;  // CREATE_ALWAYS
        case 3:                                       // OPEN_EXISTING
            if (!exists) {
                set_last_error(win::ERROR_FILE_NOT_FOUND);
                if (trace_enabled()) LOGD("CreateFile miss: %s", name.c_str());
                return win::INVALID_HANDLE;
            }
            break;
        case 4: flags |= O_CREAT; break;            // OPEN_ALWAYS
        case 5: flags |= O_TRUNC; break;            // TRUNCATE_EXISTING
        default: break;
    }
    int fd = open(host.c_str(), flags | O_CLOEXEC, 0644);
    if (fd < 0) {
        set_last_error(errno == ENOENT ? win::ERROR_PATH_NOT_FOUND : win::ERROR_ACCESS_DENIED);
        return win::INVALID_HANDLE;
    }
    File* f = new File();
    f->fd = fd;
    f->host = host;
    f->win = name;
    set_last_error((exists && (disposition == 2 || disposition == 4)) ? win::ERROR_ALREADY_EXISTS : 0);
    uint32_t h = kobj::add(f);
    if (trace_enabled()) LOGD("CreateFile(%s) -> %08x", name.c_str(), h);
    (void)c;
    return h;
}

File* file(uint32_t h) { return kobj::get_as<File>(h, kobj::Type::File); }

void signal_overlapped(uint32_t ovl, uint32_t bytes) {
    mem::wr32(ovl + 0, 0);
    mem::wr32(ovl + 4, bytes);
    uint32_t ev = mem::rd32(ovl + 16);
    if (auto* e = kobj::get_as<kobj::Event>(ev, kobj::Type::Event)) {
        {
            std::lock_guard<std::mutex> lk(kobj::wait_mutex());
            e->state = true;
        }
        kobj::notify();
    }
}

void fill_find(uint32_t out, const FindData& d) {
    memset(mem::ptr(out), 0, 320);
    mem::wr32(out + 0, d.dir ? 0x10 : 0x20);
    mem::wr<uint64_t>(out + 4, d.mtime_ft);
    mem::wr<uint64_t>(out + 12, d.mtime_ft);
    mem::wr<uint64_t>(out + 20, d.mtime_ft);
    mem::wr32(out + 28, (uint32_t)(d.size >> 32));
    mem::wr32(out + 32, (uint32_t)d.size);
    mem::write_cstr(out + 44, d.name.c_str(), 260);
}
}  // namespace

HLE_EXPORT(kernel32, CreateFileA, 28) {
    ret(c, create_file(c, gstr(arg(c, 0)), arg(c, 1), arg(c, 4)));
}
HLE_EXPORT(kernel32, CreateFileW, 28) {
    ret(c, create_file(c, winu::gwstr(arg(c, 0)), arg(c, 1), arg(c, 4)));
}

HLE_EXPORT(kernel32, ReadFile, 20) {
    uint32_t h = arg(c, 0), buf = arg(c, 1), n = arg(c, 2), pread_out = arg(c, 3), ovl = arg(c, 4);
    File* f = file(h);
    if (!f) {
        set_last_error(win::ERROR_INVALID_HANDLE);
        return ret(c, 0);
    }
    ssize_t r;
    if (ovl) {
        off64_t off = (off64_t)mem::rd32(ovl + 8) | ((off64_t)mem::rd32(ovl + 12) << 32);
        r = pread64(f->fd, mem::ptr(buf), n, off);
    } else {
        r = read(f->fd, mem::ptr(buf), n);
    }
    if (r < 0) {
        set_last_error(win::ERROR_ACCESS_DENIED);
        return ret(c, 0);
    }
    if (pread_out) mem::wr32(pread_out, (uint32_t)r);
    if (ovl) {
        signal_overlapped(ovl, (uint32_t)r);
        if (r == 0 && n) {
            set_last_error(win::ERROR_HANDLE_EOF);
            return ret(c, 0);
        }
    }
    ret(c, 1);
}

HLE_EXPORT(kernel32, WriteFile, 20) {
    uint32_t h = arg(c, 0), buf = arg(c, 1), n = arg(c, 2), pw = arg(c, 3), ovl = arg(c, 4);
    if (h == kStdOut || h == kStdErr) {
        LOGI("stdout: %.*s", (int)n, (const char*)mem::ptr(buf));
        if (pw) mem::wr32(pw, n);
        return ret(c, 1);
    }
    File* f = file(h);
    if (!f) {
        set_last_error(win::ERROR_INVALID_HANDLE);
        return ret(c, 0);
    }
    ssize_t r;
    if (ovl) {
        off64_t off = (off64_t)mem::rd32(ovl + 8) | ((off64_t)mem::rd32(ovl + 12) << 32);
        r = pwrite64(f->fd, mem::ptr(buf), n, off);
    } else {
        r = write(f->fd, mem::ptr(buf), n);
    }
    if (r < 0) {
        set_last_error(win::ERROR_ACCESS_DENIED);
        return ret(c, 0);
    }
    if (pw) mem::wr32(pw, (uint32_t)r);
    if (ovl) signal_overlapped(ovl, (uint32_t)r);
    ret(c, 1);
}

HLE_EXPORT(kernel32, GetOverlappedResult, 16) {
    uint32_t ovl = arg(c, 1), out = arg(c, 2);
    if (out) mem::wr32(out, mem::rd32(ovl + 4));
    ret(c, 1);
}

HLE_EXPORT(kernel32, SetFilePointer, 16) {
    uint32_t h = arg(c, 0), phigh = arg(c, 2), method = arg(c, 3);
    File* f = file(h);
    if (!f) {
        set_last_error(win::ERROR_INVALID_HANDLE);
        return ret(c, 0xFFFFFFFFu);
    }
    int64_t dist = (int32_t)arg(c, 1);
    if (phigh) dist = (int64_t)(((uint64_t)mem::rd32(phigh) << 32) | arg(c, 1));
    off64_t r = lseek64(f->fd, dist, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) {
        set_last_error(131);  // ERROR_NEGATIVE_SEEK
        return ret(c, 0xFFFFFFFFu);
    }
    if (phigh) mem::wr32(phigh, (uint32_t)((uint64_t)r >> 32));
    set_last_error(0);
    ret(c, (uint32_t)r);
}

HLE_EXPORT(kernel32, GetFileSize, 8) {
    File* f = file(arg(c, 0));
    if (!f) return ret(c, 0xFFFFFFFFu);
    struct stat st;
    fstat(f->fd, &st);
    if (arg(c, 1)) mem::wr32(arg(c, 1), (uint32_t)((uint64_t)st.st_size >> 32));
    ret(c, (uint32_t)st.st_size);
}

HLE_EXPORT(kernel32, SetEndOfFile, 4) {
    File* f = file(arg(c, 0));
    if (!f) return ret(c, 0);
    off64_t pos = lseek64(f->fd, 0, SEEK_CUR);
    ret(c, ftruncate64(f->fd, pos) == 0 ? 1 : 0);
}
HLE_EXPORT(kernel32, FlushFileBuffers, 4) {
    if (File* f = file(arg(c, 0))) fsync(f->fd);
    ret(c, 1);
}
HLE_EXPORT(kernel32, GetFileType, 4) {
    uint32_t h = arg(c, 0);
    if (h >= 0x10 && h <= 0x18) return ret(c, 2);  // FILE_TYPE_CHAR (std handles)
    ret(c, file(h) ? 1 : 0);
}

HLE_EXPORT(kernel32, DeleteFileA, 4) {
    std::string host = vfs::to_host(gstr(arg(c, 0)));
    bool ok = unlink(host.c_str()) == 0;
    if (!ok) set_last_error(win::ERROR_FILE_NOT_FOUND);
    ret(c, ok ? 1 : 0);
}
HLE_EXPORT(kernel32, MoveFileA, 8) {
    std::string a = vfs::to_host(gstr(arg(c, 0))), b = vfs::to_host(gstr(arg(c, 1)));
    struct stat st;
    if (stat(b.c_str(), &st) == 0) {
        set_last_error(win::ERROR_ALREADY_EXISTS);
        return ret(c, 0);
    }
    bool ok = rename(a.c_str(), b.c_str()) == 0;
    if (!ok) set_last_error(win::ERROR_FILE_NOT_FOUND);
    ret(c, ok ? 1 : 0);
}
HLE_EXPORT(kernel32, CreateDirectoryA, 8) {
    std::string host = vfs::to_host(gstr(arg(c, 0)));
    if (mkdir(host.c_str(), 0755) == 0) return ret(c, 1);
    set_last_error(errno == EEXIST ? win::ERROR_ALREADY_EXISTS : win::ERROR_PATH_NOT_FOUND);
    ret(c, 0);
}
HLE_EXPORT(kernel32, RemoveDirectoryA, 4) {
    ret(c, rmdir(vfs::to_host(gstr(arg(c, 0))).c_str()) == 0 ? 1 : 0);
}

HLE_EXPORT(kernel32, FindFirstFileA, 8) {
    std::string pat = vfs::full_win_path(gstr(arg(c, 0)));
    size_t slash = pat.find_last_of('\\');
    std::string dir = pat.substr(0, slash);
    std::string mask = pat.substr(slash + 1);
    if (dir.size() == 2) dir += "\\";
    std::string host = vfs::to_host(dir);
    Find* fd = new Find();
    if (DIR* d = opendir(host.c_str())) {
        while (dirent* e = readdir(d)) {
            if (!vfs::wildcard_match(mask.c_str(), e->d_name)) continue;
            struct stat st;
            std::string full = host + "/" + e->d_name;
            if (stat(full.c_str(), &st) != 0) continue;
            fd->entries.push_back({e->d_name, S_ISDIR(st.st_mode), (uint64_t)st.st_size,
                                   winu::unix_to_filetime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec)});
        }
        closedir(d);
    }
    if (trace_enabled()) LOGD("FindFirstFile(%s) -> %zu entries", pat.c_str(), fd->entries.size());
    if (fd->entries.empty()) {
        delete fd;
        set_last_error(win::ERROR_FILE_NOT_FOUND);
        return ret(c, win::INVALID_HANDLE);
    }
    std::sort(fd->entries.begin(), fd->entries.end(),
              [](const FindData& a, const FindData& b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });
    fill_find(arg(c, 1), fd->entries[0]);
    fd->next = 1;
    ret(c, kobj::add(fd));
}
HLE_EXPORT(kernel32, FindNextFileA, 8) {
    Find* fd = kobj::get_as<Find>(arg(c, 0), kobj::Type::Find);
    if (!fd || fd->next >= fd->entries.size()) {
        set_last_error(win::ERROR_NO_MORE_FILES);
        return ret(c, 0);
    }
    fill_find(arg(c, 1), fd->entries[fd->next++]);
    ret(c, 1);
}
HLE_EXPORT(kernel32, FindClose, 4) { ret(c, kobj::close(arg(c, 0)) ? 1 : 0); }

HLE_EXPORT(kernel32, GetCurrentDirectoryA, 8) {
    ret(c, winu::put_str(arg(c, 1), arg(c, 0), vfs::cwd()));
}
HLE_EXPORT(kernel32, GetFullPathNameA, 16) {
    std::string full = vfs::full_win_path(gstr(arg(c, 0)));
    uint32_t cap = arg(c, 1), buf = arg(c, 2), filepart = arg(c, 3);
    uint32_t n = winu::put_str(buf, cap, full);
    if (filepart && n < cap) {
        size_t slash = full.find_last_of('\\');
        mem::wr32(filepart, slash == std::string::npos || slash + 1 == full.size() ? 0 : buf + (uint32_t)slash + 1);
    }
    ret(c, n);
}
HLE_EXPORT(kernel32, GetLongPathNameA, 12) {
    ret(c, winu::put_str(arg(c, 1), arg(c, 2), gstr(arg(c, 0))));
}
HLE_EXPORT(kernel32, GetDriveTypeA, 4) {
    std::string root = gstr(arg(c, 0));
    if (root.empty() || toupper((unsigned char)root[0]) == 'C') return ret(c, 3);  // DRIVE_FIXED
    ret(c, 1);  // DRIVE_NO_ROOT_DIR
}
HLE_EXPORT(kernel32, GetLogicalDrives, 0) { ret(c, 1u << 2); }

namespace {
uint64_t free_bytes() {
    struct statvfs sv;
    if (statvfs(proc::config().user_dir.c_str(), &sv) != 0) return 4ull << 30;
    return (uint64_t)sv.f_bavail * sv.f_frsize;
}
}  // namespace

HLE_EXPORT(kernel32, GetDiskFreeSpaceExA, 16) {
    uint64_t f = free_bytes();
    if (arg(c, 1)) mem::wr<uint64_t>(arg(c, 1), f);
    if (arg(c, 2)) mem::wr<uint64_t>(arg(c, 2), f * 2);
    if (arg(c, 3)) mem::wr<uint64_t>(arg(c, 3), f);
    ret(c, 1);
}
HLE_EXPORT(kernel32, GetDiskFreeSpaceA, 20) {
    uint64_t clusters = std::min<uint64_t>(free_bytes() / 4096, 0x7FFFFFFF);
    if (arg(c, 1)) mem::wr32(arg(c, 1), 8);     // sectors per cluster
    if (arg(c, 2)) mem::wr32(arg(c, 2), 512);   // bytes per sector
    if (arg(c, 3)) mem::wr32(arg(c, 3), (uint32_t)clusters);
    if (arg(c, 4)) mem::wr32(arg(c, 4), (uint32_t)(clusters * 2));
    ret(c, 1);
}

HLE_EXPORT(kernel32, CreateFileMappingA, 24) {
    File* f = file(arg(c, 0));
    if (!f) {
        set_last_error(win::ERROR_INVALID_HANDLE);
        return ret(c, 0);
    }
    Mapping* m = new Mapping();
    m->host = f->host;
    struct stat st;
    fstat(f->fd, &st);
    m->size = arg(c, 4) ? arg(c, 4) : (uint32_t)st.st_size;
    ret(c, kobj::add(m));
}
HLE_EXPORT(kernel32, MapViewOfFile, 20) {
    Mapping* m = kobj::get_as<Mapping>(arg(c, 0), kobj::Type::Mapping);
    if (!m) return ret(c, 0);
    uint32_t off = arg(c, 3), n = arg(c, 4);
    if (!n) n = m->size - off;
    uint32_t a = mem::reserve(0, n, true);
    if (!a) return ret(c, 0);
    int fd = open(m->host.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        pread64(fd, mem::ptr(a), n, off);
        close(fd);
    }
    LOGI("MapViewOfFile(%s, off=%x, %x) -> %08x (copy; writes are not flushed)", m->host.c_str(), off, n, a);
    ret(c, a);
}
HLE_EXPORT(kernel32, UnmapViewOfFile, 4) { ret(c, mem::release(arg(c, 0)) ? 1 : 0); }

// ---------------------------------------------------------------- resources
HLE_EXPORT(kernel32, FindResourceA, 12) {
    uint32_t mod = arg(c, 0);
    if (mod && mod != loader::main_image().base) return ret(c, 0);
    uint32_t r = loader::find_resource(arg(c, 1), arg(c, 2));
    if (!r) set_last_error(1814);  // ERROR_RESOURCE_NAME_NOT_FOUND
    ret(c, r);
}
HLE_EXPORT(kernel32, FindResourceW, 12) {
    // Wide names: only integer ids are supported (strings are converted).
    uint32_t name = arg(c, 1), type = arg(c, 2);
    uint32_t tmp_n = 0, tmp_t = 0;
    if (name >= 0x10000) {
        std::string s = winu::gwstr(name);
        tmp_n = mem::alloc_system((uint32_t)s.size() + 1);
        mem::write_cstr(tmp_n, s.c_str(), (uint32_t)s.size() + 1);
        name = tmp_n;
    }
    if (type >= 0x10000) {
        std::string s = winu::gwstr(type);
        tmp_t = mem::alloc_system((uint32_t)s.size() + 1);
        mem::write_cstr(tmp_t, s.c_str(), (uint32_t)s.size() + 1);
        type = tmp_t;
    }
    ret(c, loader::find_resource(name, type));
}
HLE_EXPORT(kernel32, LoadResource, 8) { ret(c, loader::resource_data(arg(c, 1))); }
HLE_EXPORT(kernel32, LockResource, 4) { ret(c, arg(c, 0)); }
HLE_EXPORT(kernel32, SizeofResource, 8) { ret(c, loader::resource_size(arg(c, 1))); }
